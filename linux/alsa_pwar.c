// ALSA PWAR - ALSA audio interface with PWAR protocol support
// Combines ALSA audio handling with PWAR networking for low-latency audio streaming
// Compile: gcc -O2 -o alsa_pwar alsa_pwar.c -lasound -lm -lpthread
// Run: ./alsa_pwar

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <signal.h>
#include <math.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <alsa/asoundlib.h>

// Include PWAR protocol headers
#include "../protocol/pwar_packet.h"
#include "../protocol/pwar_router.h"
#include "../protocol/pwar_rcv_buffer.h"
#include "../protocol/latency_manager.h"

// ---- PWAR Config (using defines instead of command line) ----
#define PWAR_STREAM_IP          "192.168.66.3"
#define PWAR_STREAM_PORT        8321
#define PWAR_RECV_PORT          8321
#define PWAR_ONESHOT_MODE       0        // 1 = oneshot, 0 = ping-pong
#define PWAR_PASSTHROUGH_TEST   0        // 1 = local passthrough test

// ---- ALSA Config ----------------------------------------
#define PCM_DEVICE_PLAYBACK "hw:3,0"
#define PCM_DEVICE_CAPTURE  "hw:3,0"
#define SAMPLE_RATE         48000
#define FRAMES              64
#define PB_CHANNELS         2
#define CAP_CHANNELS        2
// ---------------------------------------------------------

// Statistics
static unsigned long total_iterations = 0;
static unsigned long capture_xruns = 0;
static unsigned long playback_xruns = 0;
static double total_loop_time = 0;
static double min_loop_time = 999999;
static double max_loop_time = 0;
static struct timeval start_time;

static volatile int keep_running = 1;

// PWAR data structure
typedef struct {
    int sockfd;
    struct sockaddr_in servaddr;
    int recv_sockfd;
    
    pthread_mutex_t packet_mutex;
    pthread_cond_t packet_cond;
    pwar_packet_t latest_packet;
    int packet_available;
    
    pwar_router_t router;
    pthread_mutex_t pwar_rcv_mutex;
    
    uint32_t seq;
    uint32_t current_windows_buffer_size;
} pwar_data_t;

static pwar_data_t g_pwar_data;

void sigint_handler(int sig) {
    keep_running = 0;
}

// PWAR networking functions
static void setup_socket(pwar_data_t *data, const char *ip, int port) {
    data->sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (data->sockfd < 0) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }
    memset(&data->servaddr, 0, sizeof(data->servaddr));
    data->servaddr.sin_family = AF_INET;
    data->servaddr.sin_port = htons(port);
    data->servaddr.sin_addr.s_addr = inet_addr(ip);
}

static void setup_recv_socket(pwar_data_t *data, int port) {
    data->recv_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (data->recv_sockfd < 0) {
        perror("recv socket creation failed");
        exit(EXIT_FAILURE);
    }
    
    // Increase UDP receive buffer to 1MB to reduce risk of overrun
    int rcvbuf = 1024 * 1024;
    if (setsockopt(data->recv_sockfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
        perror("setsockopt SO_RCVBUF failed");
    }
    
    struct sockaddr_in recv_addr;
    memset(&recv_addr, 0, sizeof(recv_addr));
    recv_addr.sin_family = AF_INET;
    recv_addr.sin_addr.s_addr = INADDR_ANY;
    recv_addr.sin_port = htons(port);
    if (bind(data->recv_sockfd, (struct sockaddr *)&recv_addr, sizeof(recv_addr)) < 0) {
        perror("recv socket bind failed");
        exit(EXIT_FAILURE);
    }
}

static void *receiver_thread(void *userdata) {
    // Set real-time scheduling to minimize jitter
    struct sched_param sp = { .sched_priority = 90 };
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
        perror("Warning: Failed to set SCHED_FIFO for receiver_thread");
    }

    pwar_data_t *data = (pwar_data_t *)userdata;
    char recv_buffer[sizeof(pwar_packet_t) > sizeof(pwar_latency_info_t) ? sizeof(pwar_packet_t) : sizeof(pwar_latency_info_t)];
    float output_buffers[PWAR_CHANNELS * PWAR_PACKET_MAX_CHUNK_SIZE] = {0};

    while (keep_running) {
        ssize_t n = recvfrom(data->recv_sockfd, recv_buffer, sizeof(recv_buffer), 0, NULL, NULL);
        if (n == (ssize_t)sizeof(pwar_packet_t)) {
            pwar_packet_t *packet = (pwar_packet_t *)recv_buffer;
            latency_manager_process_packet_server(packet);
            data->current_windows_buffer_size = packet->n_samples * packet->num_packets;
            
            if (PWAR_ONESHOT_MODE) {
                pthread_mutex_lock(&data->packet_mutex);
                data->latest_packet = *packet;
                data->packet_available = 1;
                pthread_cond_signal(&data->packet_cond);
                pthread_mutex_unlock(&data->packet_mutex);
            } else {
                int samples_ready = pwar_router_process_packet(&data->router, packet, output_buffers, 
                                                               PWAR_PACKET_MAX_CHUNK_SIZE, PWAR_CHANNELS);
                if (samples_ready > 0) {
                    pthread_mutex_lock(&data->pwar_rcv_mutex);
                    pwar_rcv_buffer_add_buffer(output_buffers, samples_ready, PWAR_CHANNELS);
                    pthread_mutex_unlock(&data->pwar_rcv_mutex);
                }
            }
        } else if (n == (ssize_t)sizeof(pwar_latency_info_t)) {
            pwar_latency_info_t *latency_info = (pwar_latency_info_t *)recv_buffer;
            latency_manager_handle_latency_info(latency_info);
        }
    }
    return NULL;
}

// Convert float samples to int32_t for ALSA
static void float_to_int32(float *input, int32_t *output, int samples) {
    for (int i = 0; i < samples; i++) {
        // Clamp to [-1.0, 1.0] and convert to int32
        float clamped = fmaxf(-1.0f, fminf(1.0f, input[i]));
        output[i] = (int32_t)(clamped * 2147483647.0f);  // 2^31 - 1
    }
}

// Convert int32_t samples from ALSA to float
static void int32_to_float(int32_t *input, float *output, int samples) {
    for (int i = 0; i < samples; i++) {
        output[i] = (float)input[i] / 2147483648.0f;  // 2^31
    }
}

static int setup_pcm(snd_pcm_t **handle,
                     const char *device,
                     snd_pcm_stream_t stream,
                     unsigned int rate,
                     unsigned int channels,
                     snd_pcm_uframes_t period)
{
    int err;
    snd_pcm_hw_params_t *hw;
    snd_pcm_sw_params_t *sw;

    if ((err = snd_pcm_open(handle, device, stream, 0)) < 0) {
        fprintf(stderr, "%s open error: %s\n",
                stream == SND_PCM_STREAM_PLAYBACK ? "Playback" : "Capture",
                snd_strerror(err));
        return err;
    }

    // Hardware parameters
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(*handle, hw);
    snd_pcm_hw_params_set_access(*handle, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(*handle, hw, SND_PCM_FORMAT_S32_LE);
    snd_pcm_hw_params_set_channels(*handle, hw, channels);

    unsigned int rr = rate;
    int dir = 0;
    snd_pcm_hw_params_set_rate_near(*handle, hw, &rr, &dir);

    snd_pcm_uframes_t per = period;
    snd_pcm_hw_params_set_period_size_near(*handle, hw, &per, &dir);

    snd_pcm_uframes_t buf = per * 2;  // 2 periods for slightly more safety
    snd_pcm_hw_params_set_buffer_size_near(*handle, hw, &buf);

    if ((err = snd_pcm_hw_params(*handle, hw)) < 0) {
        fprintf(stderr, "hw_params error: %s\n", snd_strerror(err));
        return err;
    }

    // Software parameters - important for xrun behavior
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(*handle, sw);
    
    // Start threshold
    snd_pcm_sw_params_set_start_threshold(*handle, sw, per);
    
    // Minimum available frames to wake up
    snd_pcm_sw_params_set_avail_min(*handle, sw, per);
    
    if ((err = snd_pcm_sw_params(*handle, sw)) < 0) {
        fprintf(stderr, "sw_params error: %s\n", snd_strerror(err));
        return err;
    }

    snd_pcm_prepare(*handle);

    // Print configuration
    snd_pcm_hw_params_get_rate(hw, &rr, &dir);
    snd_pcm_hw_params_get_period_size(hw, &per, &dir);
    snd_pcm_hw_params_get_buffer_size(hw, &buf);
    printf("%s: %u Hz, %u ch, period=%lu, buffer=%lu (%.2f ms buffer)\n",
           stream == SND_PCM_STREAM_PLAYBACK ? "Playback" : "Capture",
           rr, channels, (unsigned long)per, (unsigned long)buf,
           (double)buf * 1000.0 / rr);

    return 0;
}

static void print_stats(void) {
    struct timeval now;
    gettimeofday(&now, NULL);
    double runtime = (now.tv_sec - start_time.tv_sec) + 
                    (now.tv_usec - start_time.tv_usec) / 1000000.0;
    
    printf("\n========== Statistics ==========\n");
    printf("Runtime: %.1f seconds\n", runtime);
    printf("Total iterations: %lu\n", total_iterations);
    printf("Capture XRUNs: %lu (%.3f%%)\n", 
           capture_xruns, 
           total_iterations > 0 ? (100.0 * capture_xruns / total_iterations) : 0);
    printf("Playback XRUNs: %lu (%.3f%%)\n", 
           playback_xruns,
           total_iterations > 0 ? (100.0 * playback_xruns / total_iterations) : 0);
    
    if (total_iterations > 0) {
        double avg_loop = total_loop_time / total_iterations;
        printf("Loop time: avg=%.3f ms, min=%.3f ms, max=%.3f ms\n",
               avg_loop, min_loop_time, max_loop_time);
        printf("Theoretical min latency: %.3f ms (%.1f samples @ %d Hz)\n",
               (double)FRAMES * 1000.0 / SAMPLE_RATE, (double)FRAMES, SAMPLE_RATE);
    }
    printf("================================\n");
}

static double get_ms_elapsed(const struct timeval *t0, const struct timeval *t1) {
    return (t1->tv_sec - t0->tv_sec) * 1000.0 + 
           (t1->tv_usec - t0->tv_usec) / 1000.0;
}

// PWAR audio processing functions
static void process_audio_oneshot(pwar_data_t *data, float *input_samples, float *output_left, float *output_right, uint32_t n_samples) {
    // Send input to Windows
    pwar_packet_t packet;
    packet.seq = data->seq++;
    packet.n_samples = n_samples;
    packet.packet_index = 0;
    packet.num_packets = 1;
    
    // Copy input samples - guitar is mono on right channel
    for (int i = 0; i < n_samples; i++) {
        if (CAP_CHANNELS > 1) {
            // Guitar input is on right channel, copy to both packet channels
            float guitar_sample = input_samples[i * CAP_CHANNELS + 1];  // Right channel input
            packet.samples[0][i] = guitar_sample;  // Send to left
            packet.samples[1][i] = guitar_sample;  // Send to right
        } else {
            // Mono input case
            packet.samples[0][i] = input_samples[i];
            packet.samples[1][i] = input_samples[i];
        }
    }
    
    packet.timestamp = latency_manager_timestamp_now();
    packet.seq_timestamp = packet.timestamp;
    
    if (sendto(data->sockfd, &packet, sizeof(packet), 0, 
               (struct sockaddr *)&data->servaddr, sizeof(data->servaddr)) < 0) {
        perror("sendto failed");
        return;
    }
    
    // Wait for response with timeout
    int got_packet = 0;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 2 * 1000 * 1000; // 2ms timeout
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000;
    }
    
    pthread_mutex_lock(&data->packet_mutex);
    while (!data->packet_available) {
        int rc = pthread_cond_timedwait(&data->packet_cond, &data->packet_mutex, &ts);
        if (rc == ETIMEDOUT)
            break;
    }
    
    if (data->packet_available) {
        // Copy received samples to output
        memcpy(output_left, data->latest_packet.samples[0], n_samples * sizeof(float));
        memcpy(output_right, data->latest_packet.samples[1], n_samples * sizeof(float));
        got_packet = 1;
        data->packet_available = 0;
    }
    pthread_mutex_unlock(&data->packet_mutex);
    
    if (!got_packet) {
        latency_manager_report_xrun();
        printf("\033[0;31m--- ERROR -- No valid packet received, outputting silence\n");
        printf("Wanted seq: %u, got seq: %lu\033[0m\n", data->seq - 1, data->latest_packet.seq);
        memset(output_left, 0, n_samples * sizeof(float));
        memset(output_right, 0, n_samples * sizeof(float));
    }
}

static void process_audio_pingpong(pwar_data_t *data, float *input_samples, float *output_left, float *output_right, uint32_t n_samples) {
    // Send input to Windows
    pwar_packet_t packet;
    packet.seq = data->seq++;
    packet.n_samples = n_samples;
    packet.packet_index = 0;
    packet.num_packets = 1;
    
    // Copy input samples - guitar is mono on right channel
    for (int i = 0; i < n_samples; i++) {
        if (CAP_CHANNELS > 1) {
            // Guitar input is on right channel, copy to both packet channels
            float guitar_sample = input_samples[i * CAP_CHANNELS + 1];  // Right channel input
            packet.samples[0][i] = guitar_sample;  // Send to left
            packet.samples[1][i] = guitar_sample;  // Send to right
        } else {
            // Mono input case
            packet.samples[0][i] = input_samples[i];
            packet.samples[1][i] = input_samples[i];
        }
    }
    
    packet.timestamp = latency_manager_timestamp_now();
    packet.seq_timestamp = packet.timestamp;
    
    pthread_mutex_lock(&data->pwar_rcv_mutex);
    
    if (sendto(data->sockfd, &packet, sizeof(packet), 0, 
               (struct sockaddr *)&data->servaddr, sizeof(data->servaddr)) < 0) {
        perror("sendto failed");
    }
    
    // Get processed samples from previous iteration
    float rcv_buffers[PWAR_CHANNELS * n_samples];
    memset(rcv_buffers, 0, sizeof(rcv_buffers));
    
    if (!pwar_rcv_get_chunk(rcv_buffers, PWAR_CHANNELS, n_samples)) {
        printf("\033[0;31m--- ERROR -- No valid buffer ready, outputting silence\033[0m\n");
        latency_manager_report_xrun();
    }
    
    pthread_mutex_unlock(&data->pwar_rcv_mutex);
    
    // Copy to output channels
    memcpy(output_left, rcv_buffers, n_samples * sizeof(float));
    memcpy(output_right, rcv_buffers + n_samples, n_samples * sizeof(float));
}

int main(void) {
    int err;
    snd_pcm_t *pb = NULL, *cap = NULL;
    pthread_t recv_thread;

    signal(SIGINT, sigint_handler);
    
    // Initialize PWAR data
    memset(&g_pwar_data, 0, sizeof(g_pwar_data));
    setup_socket(&g_pwar_data, PWAR_STREAM_IP, PWAR_STREAM_PORT);
    setup_recv_socket(&g_pwar_data, PWAR_RECV_PORT);
    
    pthread_mutex_init(&g_pwar_data.packet_mutex, NULL);
    pthread_cond_init(&g_pwar_data.packet_cond, NULL);
    pthread_mutex_init(&g_pwar_data.pwar_rcv_mutex, NULL);
    
    g_pwar_data.packet_available = 0;
    g_pwar_data.seq = 0;
    
    // Initialize PWAR components
    pwar_router_init(&g_pwar_data.router, PWAR_CHANNELS);
    
    // Start receiver thread
    pthread_create(&recv_thread, NULL, receiver_thread, &g_pwar_data);

    // Setup ALSA
    if ((err = setup_pcm(&pb, PCM_DEVICE_PLAYBACK, SND_PCM_STREAM_PLAYBACK,
                         SAMPLE_RATE, PB_CHANNELS, FRAMES)) < 0) return 1;
    if ((err = setup_pcm(&cap, PCM_DEVICE_CAPTURE,  SND_PCM_STREAM_CAPTURE,
                         SAMPLE_RATE, CAP_CHANNELS, FRAMES)) < 0) return 1;

    // Allocate buffers
    int32_t *pb_buf  = calloc(FRAMES * PB_CHANNELS,  sizeof(int32_t));
    int32_t *cap_buf = calloc(FRAMES * CAP_CHANNELS, sizeof(int32_t));
    float *input_float = calloc(FRAMES * CAP_CHANNELS, sizeof(float));
    float *output_left = calloc(FRAMES, sizeof(float));
    float *output_right = calloc(FRAMES, sizeof(float));
    
    if (!pb_buf || !cap_buf || !input_float || !output_left || !output_right) { 
        perror("calloc"); 
        return 1; 
    }

    printf("\nStarting ALSA PWAR with %s mode. Press Ctrl+C for statistics.\n", 
           PWAR_ONESHOT_MODE ? "oneshot" : "ping-pong");
    printf("PWAR target: %s:%d\n", PWAR_STREAM_IP, PWAR_STREAM_PORT);
    printf("Legend: . = 1000 clean loops, C = capture xrun, P = playback xrun\n\n");
    
    gettimeofday(&start_time, NULL);
    
    struct timeval loop_start, loop_end;
    unsigned long clean_loops = 0;

    while (keep_running) {
        gettimeofday(&loop_start, NULL);
        
        // 1) Capture
        err = snd_pcm_readi(cap, cap_buf, FRAMES);
        if (err == -EPIPE || err == -ESTRPIPE) {
            printf("C");
            fflush(stdout);
            capture_xruns++;
            snd_pcm_prepare(cap);
            continue;
        } else if (err < 0) {
            fprintf(stderr, "\nCapture error: %s\n", snd_strerror(err));
            snd_pcm_prepare(cap);
            continue;
        }

        // 2) Convert captured samples to float and process with PWAR
        if (PWAR_PASSTHROUGH_TEST) {
            // Local passthrough test - just copy input to output
            for (int i = 0; i < FRAMES; i++) {
                for (int ch = 0; ch < PB_CHANNELS; ch++) {
                    int in_ch = (ch < CAP_CHANNELS) ? ch : 0;
                    pb_buf[i * PB_CHANNELS + ch] = cap_buf[i * CAP_CHANNELS + in_ch];
                }
            }
        } else {
            // PWAR processing
            int32_to_float(cap_buf, input_float, FRAMES * CAP_CHANNELS);
            
            if (PWAR_ONESHOT_MODE) {
                process_audio_oneshot(&g_pwar_data, input_float, output_left, output_right, FRAMES);
            } else {
                process_audio_pingpong(&g_pwar_data, input_float, output_left, output_right, FRAMES);
            }

            // Convert float output back to int32 for ALSA with proper interleaving
            for (int i = 0; i < FRAMES; i++) {
                // Convert left channel
                float clamped_left = fmaxf(-1.0f, fminf(1.0f, output_left[i]));
                pb_buf[i * PB_CHANNELS + 0] = (int32_t)(clamped_left * 2147483647.0f);
                
                // Convert right channel
                if (PB_CHANNELS > 1) {
                    float clamped_right = fmaxf(-1.0f, fminf(1.0f, output_right[i]));
                    pb_buf[i * PB_CHANNELS + 1] = (int32_t)(clamped_right * 2147483647.0f);
                }
            }
        }

        // 3) Playback
        err = snd_pcm_writei(pb, pb_buf, FRAMES);
        if (err == -EPIPE || err == -ESTRPIPE) {
            printf("P");
            fflush(stdout);
            playback_xruns++;
            snd_pcm_prepare(pb);
            continue;
        } else if (err < 0) {
            fprintf(stderr, "\nPlayback error: %s\n", snd_strerror(err));
            snd_pcm_prepare(pb);
            continue;
        }

        // Update statistics
        gettimeofday(&loop_end, NULL);
        double loop_time = get_ms_elapsed(&loop_start, &loop_end);
        
        total_loop_time += loop_time;
        if (loop_time < min_loop_time) min_loop_time = loop_time;
        if (loop_time > max_loop_time) max_loop_time = loop_time;
        
        total_iterations++;
        clean_loops++;
        
        // Progress indicator every 1000 clean loops
        if (clean_loops >= 1000) {
            printf(".");
            fflush(stdout);
            clean_loops = 0;
        }
    }

    print_stats();
    
    // Check final device state
    snd_pcm_state_t cap_state = snd_pcm_state(cap);
    snd_pcm_state_t pb_state = snd_pcm_state(pb);
    printf("\nFinal device states:\n");
    printf("Capture: %s\n", snd_pcm_state_name(cap_state));
    printf("Playback: %s\n", snd_pcm_state_name(pb_state));

    // Cleanup
    pthread_cancel(recv_thread);
    pthread_join(recv_thread, NULL);
    
    close(g_pwar_data.sockfd);
    close(g_pwar_data.recv_sockfd);
    pthread_mutex_destroy(&g_pwar_data.packet_mutex);
    pthread_cond_destroy(&g_pwar_data.packet_cond);
    pthread_mutex_destroy(&g_pwar_data.pwar_rcv_mutex);

    free(pb_buf);
    free(cap_buf);
    free(input_float);
    free(output_left);
    free(output_right);
    snd_pcm_close(pb);
    snd_pcm_close(cap);
    return 0;
}
