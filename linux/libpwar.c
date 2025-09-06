// Core PWAR Library - Audio backend agnostic implementation
// Contains the core PWAR protocol logic without direct audio API dependencies

#include "libpwar.h"
#include "audio_backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <unistd.h>

#include "../protocol/latency_manager.h"
#include "../protocol/pwar_packet.h"
#include "../protocol/pwar_router.h"
#include "../protocol/pwar_rcv_buffer.h"

#define DEFAULT_STREAM_IP "192.168.66.3"
#define DEFAULT_STREAM_PORT 8321
#define MAX_BUFFER_SIZE 4096
#define NUM_CHANNELS 2

// Global data for GUI mode
static struct pwar_core_data *g_pwar_data = NULL;
static pthread_t g_recv_thread;
static int g_pwar_initialized = 0;
static int g_pwar_running = 0;
static pwar_config_t g_current_config;

// Core PWAR data structure (audio backend agnostic)
struct pwar_core_data {
    // Audio backend
    audio_backend_t *audio_backend;
    
    // PWAR configuration
    pwar_config_t config;
    
    // Network
    int sockfd;
    struct sockaddr_in servaddr;
    int recv_sockfd;
    
    // PWAR protocol state
    uint32_t seq;
    pthread_mutex_t packet_mutex;
    pthread_cond_t packet_cond;
    pwar_packet_t latest_packet;
    int packet_available;
    
    pwar_router_t router;
    pthread_mutex_t pwar_rcv_mutex;
    
    uint32_t current_windows_buffer_size;
    volatile int should_stop;
};

// Forward declarations
static void setup_socket(struct pwar_core_data *data, const char *ip, int port);
static void setup_recv_socket(struct pwar_core_data *data, int port);
static void *receiver_thread(void *userdata);
static void audio_process_callback(float *in, float *out_left, float *out_right, 
                                 uint32_t n_samples, void *userdata);
static void process_one_shot(struct pwar_core_data *data, float *in, uint32_t n_samples, 
                           float *left_out, float *right_out);
static void process_ping_pong(struct pwar_core_data *data, float *in, uint32_t n_samples, 
                            float *left_out, float *right_out);

static void setup_socket(struct pwar_core_data *data, const char *ip, int port) {
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

static void setup_recv_socket(struct pwar_core_data *data, int port) {
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

    struct pwar_core_data *data = (struct pwar_core_data *)userdata;
    char recv_buffer[sizeof(pwar_packet_t) > sizeof(pwar_latency_info_t) ? sizeof(pwar_packet_t) : sizeof(pwar_latency_info_t)];
    float output_buffers[NUM_CHANNELS * MAX_BUFFER_SIZE] = {0};

    while (!data->should_stop) {
        ssize_t n = recvfrom(data->recv_sockfd, recv_buffer, sizeof(recv_buffer), 0, NULL, NULL);
        if (n == (ssize_t)sizeof(pwar_packet_t)) {
            pwar_packet_t *packet = (pwar_packet_t *)recv_buffer;
            latency_manager_process_packet_server(packet);
            data->current_windows_buffer_size = packet->n_samples * packet->num_packets;
            
            if (data->config.oneshot_mode) {
                pthread_mutex_lock(&data->packet_mutex);
                data->latest_packet = *packet;
                data->packet_available = 1;
                pthread_cond_signal(&data->packet_cond);
                pthread_mutex_unlock(&data->packet_mutex);
            } else {
                int samples_ready = pwar_router_process_packet(&data->router, packet, output_buffers, 
                                                               MAX_BUFFER_SIZE, NUM_CHANNELS);
                if (samples_ready > 0) {
                    pthread_mutex_lock(&data->pwar_rcv_mutex);
                    pwar_rcv_buffer_add_buffer(output_buffers, samples_ready, NUM_CHANNELS);
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

static void audio_process_callback(float *in, float *out_left, float *out_right, 
                                 uint32_t n_samples, void *userdata) {
    struct pwar_core_data *data = (struct pwar_core_data *)userdata;
    
    if (data->config.passthrough_test) {
        // Local passthrough test - just copy input to output
        if (out_left) memcpy(out_left, in, n_samples * sizeof(float));
        if (out_right) memcpy(out_right, in, n_samples * sizeof(float));
        return;
    }

    if (data->config.oneshot_mode) {
        process_one_shot(data, in, n_samples, out_left, out_right);
    } else {
        process_ping_pong(data, in, n_samples, out_left, out_right);
    }
}

static void process_one_shot(struct pwar_core_data *data, float *in, uint32_t n_samples, 
                           float *left_out, float *right_out) {
    // Send input to Windows
    pwar_packet_t packet;
    packet.seq = data->seq++;
    packet.n_samples = n_samples;
    packet.packet_index = 0;
    packet.num_packets = 1;
    
    // Copy input samples to both channels
    memcpy(packet.samples[0], in, n_samples * sizeof(float));
    memcpy(packet.samples[1], in, n_samples * sizeof(float));
    
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
        if (left_out) memcpy(left_out, data->latest_packet.samples[0], n_samples * sizeof(float));
        if (right_out) memcpy(right_out, data->latest_packet.samples[1], n_samples * sizeof(float));
        got_packet = 1;
        data->packet_available = 0;
    }
    pthread_mutex_unlock(&data->packet_mutex);
    
    if (!got_packet) {
        latency_manager_report_xrun();
        printf("\033[0;31m--- ERROR -- No valid packet received, outputting silence\n");
        printf("Wanted seq: %u, got seq: %lu\033[0m\n", data->seq - 1, data->latest_packet.seq);
        if (left_out) memset(left_out, 0, n_samples * sizeof(float));
        if (right_out) memset(right_out, 0, n_samples * sizeof(float));
    }
}

static void process_ping_pong(struct pwar_core_data *data, float *in, uint32_t n_samples, 
                            float *left_out, float *right_out) {
    // Send input to Windows
    pwar_packet_t packet;
    packet.seq = data->seq++;
    packet.n_samples = n_samples;
    packet.packet_index = 0;
    packet.num_packets = 1;
    
    // Copy input samples to both channels
    memcpy(packet.samples[0], in, n_samples * sizeof(float));
    memcpy(packet.samples[1], in, n_samples * sizeof(float));
    
    packet.timestamp = latency_manager_timestamp_now();
    packet.seq_timestamp = packet.timestamp;
    
    pthread_mutex_lock(&data->pwar_rcv_mutex);
    
    if (sendto(data->sockfd, &packet, sizeof(packet), 0, 
               (struct sockaddr *)&data->servaddr, sizeof(data->servaddr)) < 0) {
        perror("sendto failed");
    }
    
    // Get processed samples from previous iteration
    float rcv_buffers[NUM_CHANNELS * n_samples];
    memset(rcv_buffers, 0, sizeof(rcv_buffers));
    
    if (!pwar_rcv_get_chunk(rcv_buffers, NUM_CHANNELS, n_samples)) {
        printf("\033[0;31m--- ERROR -- No valid buffer ready, outputting silence\033[0m\n");
        latency_manager_report_xrun();
    }
    
    pthread_mutex_unlock(&data->pwar_rcv_mutex);
    
    // Copy to output channels
    if (left_out) memcpy(left_out, rcv_buffers, n_samples * sizeof(float));
    if (right_out) memcpy(right_out, rcv_buffers + n_samples, n_samples * sizeof(float));
}

// Extract common initialization logic
static int init_core_data(struct pwar_core_data *data, const pwar_config_t *config) {
    memset(data, 0, sizeof(struct pwar_core_data));
    data->config = *config;
    
    setup_socket(data, config->stream_ip, config->stream_port);
    setup_recv_socket(data, DEFAULT_STREAM_PORT);
    
    pthread_mutex_init(&data->packet_mutex, NULL);
    pthread_cond_init(&data->packet_cond, NULL);
    data->packet_available = 0;
    pthread_mutex_init(&data->pwar_rcv_mutex, NULL);
    
    data->seq = 0;
    pwar_router_init(&data->router, NUM_CHANNELS);
    
    // Create appropriate audio backend using unified factory
    if (!audio_backend_is_available(config->backend_type)) {
        fprintf(stderr, "Audio backend type %d is not available (not compiled in)\n", config->backend_type);
        return -1;
    }
    
    data->audio_backend = audio_backend_create(config->backend_type);
    
    if (!data->audio_backend) {
        fprintf(stderr, "Failed to create audio backend\n");
        return -1;
    }
    
    // Initialize the audio backend
    if (audio_backend_init(data->audio_backend, &config->audio_config, 
                          audio_process_callback, data) < 0) {
        fprintf(stderr, "Failed to initialize audio backend\n");
        audio_backend_cleanup(data->audio_backend);
        data->audio_backend = NULL;
        return -1;
    }
    
    return 0;
}

// Signal handler for CLI mode
static volatile int cli_keep_running = 1;
static void cli_sigint_handler(int sig) {
    cli_keep_running = 0;
}

// Public API Implementation
int pwar_requires_restart(const pwar_config_t *old_config, const pwar_config_t *new_config) {
    if (old_config->buffer_size != new_config->buffer_size ||
        strcmp(old_config->stream_ip, new_config->stream_ip) != 0 ||
        old_config->stream_port != new_config->stream_port ||
        old_config->backend_type != new_config->backend_type) {
        return 1;
    }
    return 0;
}

int pwar_update_config(const pwar_config_t *config) {
    if (!g_pwar_initialized) {
        return -1;
    }

    if (pwar_requires_restart(&g_current_config, config)) {
        return -2; // Signal that restart is needed
    }

    // Apply runtime-changeable settings
    g_pwar_data->config.passthrough_test = config->passthrough_test;
    g_pwar_data->config.oneshot_mode = config->oneshot_mode;
    g_current_config = *config;
    
    return 0;
}

int pwar_init(const pwar_config_t *config) {
    if (g_pwar_initialized) {
        return -1;
    }

    g_current_config = *config;

    g_pwar_data = malloc(sizeof(struct pwar_core_data));
    if (!g_pwar_data) {
        return -1;
    }

    if (init_core_data(g_pwar_data, config) < 0) {
        free(g_pwar_data);
        g_pwar_data = NULL;
        return -1;
    }

    // Start receiver thread
    g_pwar_data->should_stop = 0;
    pthread_create(&g_recv_thread, NULL, receiver_thread, g_pwar_data);

    g_pwar_initialized = 1;
    return 0;
}

int pwar_start(void) {
    if (!g_pwar_initialized || g_pwar_running) {
        return -1;
    }

    if (audio_backend_start(g_pwar_data->audio_backend) < 0) {
        return -1;
    }

    g_pwar_running = 1;
    return 0;
}

int pwar_stop(void) {
    if (!g_pwar_running) {
        return -1;
    }

    audio_backend_stop(g_pwar_data->audio_backend);
    g_pwar_running = 0;
    return 0;
}

void pwar_cleanup(void) {
    if (g_pwar_running) {
        pwar_stop();
    }

    if (g_pwar_initialized) {
        g_pwar_data->should_stop = 1;
        pthread_cancel(g_recv_thread);
        pthread_join(g_recv_thread, NULL);

        if (g_pwar_data->audio_backend) {
            audio_backend_cleanup(g_pwar_data->audio_backend);
            g_pwar_data->audio_backend = NULL;
        }

        if (g_pwar_data->sockfd > 0) {
            close(g_pwar_data->sockfd);
        }
        if (g_pwar_data->recv_sockfd > 0) {
            close(g_pwar_data->recv_sockfd);
        }

        pthread_mutex_destroy(&g_pwar_data->packet_mutex);
        pthread_cond_destroy(&g_pwar_data->packet_cond);
        pthread_mutex_destroy(&g_pwar_data->pwar_rcv_mutex);

        free(g_pwar_data);
        g_pwar_data = NULL;
        g_pwar_initialized = 0;
    }
}

int pwar_is_running(void) {
    return g_pwar_running;
}

int pwar_cli_run(const pwar_config_t *config) {
    struct pwar_core_data data;
    pthread_t recv_thread;

    // Set up signal handler for CLI mode
    signal(SIGINT, cli_sigint_handler);
    signal(SIGTERM, cli_sigint_handler);

    // Initialize core data
    if (init_core_data(&data, config) < 0) {
        return -1;
    }

    // Start receiver thread
    data.should_stop = 0;
    pthread_create(&recv_thread, NULL, receiver_thread, &data);

    // Start audio backend
    if (audio_backend_start(data.audio_backend) < 0) {
        fprintf(stderr, "Failed to start audio backend\n");
        data.should_stop = 1;
        pthread_join(recv_thread, NULL);
        audio_backend_cleanup(data.audio_backend);
        return -1;
    }

    printf("PWAR CLI started with %s backend. Press Ctrl+C to stop.\n",
           config->backend_type == AUDIO_BACKEND_ALSA ? "ALSA" : "PipeWire");

    // Wait for shutdown signal
    while (cli_keep_running) {
        usleep(100000); // 100ms
    }

    printf("\nShutting down PWAR CLI...\n");

    // Cleanup
    data.should_stop = 1;
    audio_backend_stop(data.audio_backend);
    pthread_join(recv_thread, NULL);
    audio_backend_cleanup(data.audio_backend);

    if (data.sockfd > 0) close(data.sockfd);
    if (data.recv_sockfd > 0) close(data.recv_sockfd);

    pthread_mutex_destroy(&data.packet_mutex);
    pthread_cond_destroy(&data.packet_cond);
    pthread_mutex_destroy(&data.pwar_rcv_mutex);

    return 0;
}

void pwar_get_latency_metrics(pwar_latency_metrics_t *metrics) {
    if (!metrics) return;
    
    if (g_pwar_initialized && g_pwar_running) {
        latency_manager_get_current_metrics(metrics);
    } else {
        // Return zeros if not running
        metrics->audio_proc_min_ms = 0.0;
        metrics->audio_proc_max_ms = 0.0;
        metrics->audio_proc_avg_ms = 0.0;
        metrics->jitter_min_ms = 0.0;
        metrics->jitter_max_ms = 0.0;
        metrics->jitter_avg_ms = 0.0;
        metrics->rtt_min_ms = 0.0;
        metrics->rtt_max_ms = 0.0;
        metrics->rtt_avg_ms = 0.0;
        metrics->xruns = 0;
    }
}

uint32_t pwar_get_current_windows_buffer_size(void) {
    if (g_pwar_initialized && g_pwar_running && g_pwar_data) {
        return g_pwar_data->current_windows_buffer_size;
    }
    return 0;
}
