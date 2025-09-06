// Simulated Audio Backend for PWAR
// Provides perfect timing simulation for testing without hardware

#define _GNU_SOURCE
#include "audio_backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

// Simulated backend private data
typedef struct {
    pthread_t thread;
    volatile int running;
    audio_process_callback_t callback;
    void *userdata;
    
    // Audio configuration
    uint32_t sample_rate;
    uint32_t frames;
    uint32_t channels_in;
    uint32_t channels_out;
    
    // Test signal generation
    double phase;
    double freq;        // Low frequency for latency measurement
    
    // Timing statistics
    uint64_t total_callbacks;
    struct timespec start_time;
} simulated_backend_data_t;

static uint64_t timespec_to_ns(const struct timespec *ts) {
    return (uint64_t)ts->tv_sec * 1000000000ULL + ts->tv_nsec;
}

static void* simulated_thread(void* arg) {
    audio_backend_t *backend = (audio_backend_t *)arg;
    simulated_backend_data_t *data = (simulated_backend_data_t*)backend->private_data;
    
    printf("[Simulated Audio] Starting audio simulation thread\n");
    printf("[Simulated Audio] Sample rate: %u Hz, Buffer size: %u frames\n", 
           data->sample_rate, data->frames);
    printf("[Simulated Audio] Test signal: %.1f Hz\n", data->freq);
    
    // Calculate precise timing for buffer delivery
    uint64_t frame_time_ns = (uint64_t)data->frames * 1000000000ULL / data->sample_rate;
    struct timespec sleep_time = {
        .tv_sec = frame_time_ns / 1000000000ULL,
        .tv_nsec = frame_time_ns % 1000000000ULL
    };
    
    printf("[Simulated Audio] Buffer interval: %.3f ms\n", frame_time_ns / 1000000.0);
    
    // Allocate buffers (single channel input)
    float *input_buffer = calloc(data->frames, sizeof(float));
    float *output_left = calloc(data->frames, sizeof(float));
    float *output_right = calloc(data->frames, sizeof(float));
    
    if (!input_buffer || !output_left || !output_right) {
        printf("[Simulated Audio] Failed to allocate buffers\n");
        goto cleanup;
    }
    
    // Record start time
    clock_gettime(CLOCK_MONOTONIC, &data->start_time);
    
    while (data->running) {
        // Generate test input signal (single channel sine wave)
        for (uint32_t i = 0; i < data->frames; i++) {
            // Generate single channel input with low frequency for latency measurement
            float sample = 0.3f * sinf(2.0f * M_PI * data->phase);
            
            // Single channel input (not interleaved)
            input_buffer[i] = sample;
            
            // Update phase
            data->phase += data->freq / data->sample_rate;
            
            // Keep phase in range [0, 1)
            if (data->phase >= 1.0) data->phase -= 1.0;
        }
        
        // Call the audio processing callback (PWAR protocol processing)
        if (data->callback) {
            data->callback(input_buffer, output_left, output_right, data->frames, data->userdata);
        }
        
        data->total_callbacks++;
        
        // Print periodic status (every 5 seconds)
        if (0 && data->total_callbacks % (5 * data->sample_rate / data->frames) == 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed = (timespec_to_ns(&now) - timespec_to_ns(&data->start_time)) / 1000000000.0;
            printf("[Simulated Audio] Running for %.1fs, %llu callbacks processed\n", 
                   elapsed, (unsigned long long)data->total_callbacks);
        }
        
        // Simulate precise hardware timing
        nanosleep(&sleep_time, NULL);
    }
    
cleanup:
    printf("[Simulated Audio] Stopping audio simulation thread\n");
    free(input_buffer);
    free(output_left);
    free(output_right);
    return NULL;
}

static int simulated_init(audio_backend_t *backend, const audio_config_t *config,
                         audio_process_callback_t callback, void *userdata) {
    simulated_backend_data_t *data = calloc(1, sizeof(simulated_backend_data_t));
    if (!data) {
        printf("[Simulated Audio] Failed to allocate backend data\n");
        return -1;
    }
    
    data->callback = callback;
    data->userdata = userdata;
    data->sample_rate = config->sample_rate;
    data->frames = config->frames;
    data->channels_in = config->capture_channels;
    data->channels_out = config->playback_channels;
    data->running = 0;
    data->total_callbacks = 0;
    
    // Test signal frequency (low frequency for latency measurement)
    // At 50 Hz, zero crossings are ~10ms apart, good for measuring 0.8-30ms latency
    data->freq = 50.0;    // 50 Hz sine wave
    data->phase = 0.0;
    
    backend->private_data = data;
    backend->callback = callback;
    backend->userdata = userdata;
    
    printf("[Simulated Audio] Backend initialized successfully\n");
    return 0;
}

static int simulated_start(audio_backend_t *backend) {
    simulated_backend_data_t *data = (simulated_backend_data_t*)backend->private_data;
    if (!data) return -1;
    
    if (data->running) {
        printf("[Simulated Audio] Already running\n");
        return 0;
    }
    
    data->running = 1;
    backend->running = 1;
    
    if (pthread_create(&data->thread, NULL, simulated_thread, backend) != 0) {
        printf("[Simulated Audio] Failed to create audio thread\n");
        data->running = 0;
        backend->running = 0;
        return -1;
    }
    
    printf("[Simulated Audio] Started successfully\n");
    return 0;
}

static int simulated_stop(audio_backend_t *backend) {
    simulated_backend_data_t *data = (simulated_backend_data_t*)backend->private_data;
    if (!data || !data->running) return 0;
    
    printf("[Simulated Audio] Stopping...\n");
    data->running = 0;
    backend->running = 0;
    
    pthread_join(data->thread, NULL);
    printf("[Simulated Audio] Stopped successfully\n");
    return 0;
}

static void simulated_cleanup(audio_backend_t *backend) {
    simulated_backend_data_t *data = (simulated_backend_data_t*)backend->private_data;
    if (!data) return;
    
    if (data->running) {
        simulated_stop(backend);
    }
    
    printf("[Simulated Audio] Cleaning up\n");
    free(data);
    backend->private_data = NULL;
}

static int simulated_is_running(audio_backend_t *backend) {
    simulated_backend_data_t *data = (simulated_backend_data_t*)backend->private_data;
    return data ? data->running : 0;
}

static void simulated_get_stats(audio_backend_t *backend, void *stats) {
    simulated_backend_data_t *data = (simulated_backend_data_t*)backend->private_data;
    if (!data || !stats) return;
    
    // For now, just print some basic stats
    printf("[Simulated Audio Stats] Total callbacks: %llu\n", 
           (unsigned long long)data->total_callbacks);
}

static const audio_backend_ops_t simulated_ops = {
    .init = simulated_init,
    .start = simulated_start,
    .stop = simulated_stop,
    .cleanup = simulated_cleanup,
    .is_running = simulated_is_running,
    .get_stats = simulated_get_stats
};

audio_backend_t* audio_backend_create_simulated(void) {
    audio_backend_t *backend = calloc(1, sizeof(audio_backend_t));
    if (!backend) {
        printf("[Simulated Audio] Failed to allocate backend\n");
        return NULL;
    }
    
    backend->ops = &simulated_ops;
    backend->private_data = NULL;
    backend->running = 0;
    
    return backend;
}
