// PWAR CLI - Unified CLI application supporting both ALSA and PipeWire backends
// Uses the new unified PWAR architecture
// Compile: gcc -O2 -o pwar_cli_new pwar_cli_new.c libpwar_new.c alsa_backend.c pipewire_backend.c audio_backend.c -lasound -lpipewire-0.3 -lspa-0.2 -lm -lpthread

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "libpwar.h"

// Default configuration
#define DEFAULT_STREAM_IP          "192.168.66.3"
#define DEFAULT_STREAM_PORT        8321
#define DEFAULT_ONESHOT_MODE       0        // 1 = oneshot, 0 = ping-pong
#define DEFAULT_PASSTHROUGH_TEST   0        // 1 = local passthrough test

// Audio defaults
#define DEFAULT_SAMPLE_RATE         48000
#define DEFAULT_FRAMES              32
#define DEFAULT_CHANNELS            2

// ALSA specific defaults
#define DEFAULT_PCM_DEVICE_PLAYBACK "hw:3,0"
#define DEFAULT_PCM_DEVICE_CAPTURE  "hw:3,0"

static void print_usage(const char *program_name) {
    printf("Usage: %s [options]\n", program_name);
    printf("Options:\n");
    printf("  -b, --backend <backend>    Audio backend: alsa or pipewire (default: pipewire)\n");
    printf("  -i, --ip <ip>              Target IP address (default: %s)\n", DEFAULT_STREAM_IP);
    printf("  -p, --port <port>          Target port (default: %d)\n", DEFAULT_STREAM_PORT);
    printf("  -o, --oneshot              Use oneshot mode (default: ping-pong)\n");
    printf("  -t, --passthrough          Enable passthrough test mode\n");
    printf("  -f, --frames <frames>      Buffer size in frames (default: %d)\n", DEFAULT_FRAMES);
    printf("  -r, --rate <rate>          Sample rate (default: %d)\n", DEFAULT_SAMPLE_RATE);
    printf("  --capture-device <device>  ALSA capture device (ALSA only, default: %s)\n", DEFAULT_PCM_DEVICE_CAPTURE);
    printf("  --playback-device <device> ALSA playback device (ALSA only, default: %s)\n", DEFAULT_PCM_DEVICE_PLAYBACK);
    printf("  -h, --help                 Show this help message\n");
    printf("\nBackends:\n");
    printf("  alsa                       Use ALSA for audio I/O\n");
    printf("  pipewire                   Use PipeWire for audio I/O\n");
    printf("\nExamples:\n");
    printf("  %s                         # Use PipeWire with default settings\n", program_name);
    printf("  %s -b alsa -i 192.168.1.100 -p 9000 -f 64\n", program_name);
    printf("  %s --backend pipewire --oneshot --frames 128\n", program_name);
    printf("  %s -b alsa --passthrough   # ALSA local passthrough test\n", program_name);
}

static audio_backend_type_t parse_backend(const char *backend_str) {
    if (strcmp(backend_str, "alsa") == 0) {
        return AUDIO_BACKEND_ALSA;
    } else if (strcmp(backend_str, "pipewire") == 0) {
        return AUDIO_BACKEND_PIPEWIRE;
    } else {
        return AUDIO_BACKEND_PIPEWIRE; // Default fallback
    }
}

static int parse_arguments(int argc, char *argv[], pwar_config_t *config) {
    // Set defaults
    strcpy(config->stream_ip, DEFAULT_STREAM_IP);
    config->stream_port = DEFAULT_STREAM_PORT;
    config->oneshot_mode = DEFAULT_ONESHOT_MODE;
    config->passthrough_test = DEFAULT_PASSTHROUGH_TEST;
    config->buffer_size = DEFAULT_FRAMES;
    config->backend_type = AUDIO_BACKEND_PIPEWIRE; // Default to PipeWire
    
    // Audio config defaults
    config->audio_config.device_playback = DEFAULT_PCM_DEVICE_PLAYBACK;
    config->audio_config.device_capture = DEFAULT_PCM_DEVICE_CAPTURE;
    config->audio_config.sample_rate = DEFAULT_SAMPLE_RATE;
    config->audio_config.frames = DEFAULT_FRAMES;
    config->audio_config.playback_channels = DEFAULT_CHANNELS;
    config->audio_config.capture_channels = DEFAULT_CHANNELS;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return -1;
        } else if ((strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--backend") == 0) && i + 1 < argc) {
            config->backend_type = parse_backend(argv[++i]);
        } else if ((strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--ip") == 0) && i + 1 < argc) {
            strcpy(config->stream_ip, argv[++i]);
        } else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) && i + 1 < argc) {
            config->stream_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--oneshot") == 0) {
            config->oneshot_mode = 1;
        } else if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--passthrough") == 0) {
            config->passthrough_test = 1;
        } else if ((strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--frames") == 0) && i + 1 < argc) {
            int frames = atoi(argv[++i]);
            config->buffer_size = frames;
            config->audio_config.frames = frames;
        } else if ((strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--rate") == 0) && i + 1 < argc) {
            config->audio_config.sample_rate = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--capture-device") == 0 && i + 1 < argc) {
            config->audio_config.device_capture = argv[++i];
        } else if (strcmp(argv[i], "--playback-device") == 0 && i + 1 < argc) {
            config->audio_config.device_playback = argv[++i];
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            print_usage(argv[0]);
            return -1;
        }
    }
    
    return 0;
}

int main(int argc, char *argv[]) {
    pwar_config_t config;
    
    printf("PWAR CLI - Low-latency audio streaming with PWAR protocol\n");
    printf("Unified architecture supporting multiple audio backends\n\n");
    
    // Parse command line arguments
    if (parse_arguments(argc, argv, &config) < 0) {
        return 1;
    }
    
    // Validate backend availability
    if (!audio_backend_is_available(config.backend_type)) {
        const char *backend_name = (config.backend_type == AUDIO_BACKEND_ALSA) ? "ALSA" : "PipeWire";
        printf("Error: %s backend is not available (not compiled in)\n", backend_name);
        printf("Available backends:\n");
        if (audio_backend_is_available(AUDIO_BACKEND_ALSA)) {
            printf("  - ALSA\n");
        }
        if (audio_backend_is_available(AUDIO_BACKEND_PIPEWIRE)) {
            printf("  - PipeWire\n");
        }
        return 1;
    }
    
    // Print configuration
    printf("Configuration:\n");
    printf("  Target: %s:%d\n", config.stream_ip, config.stream_port);
    printf("  Mode: %s\n", config.oneshot_mode ? "oneshot" : "ping-pong");
    printf("  Passthrough test: %s\n", config.passthrough_test ? "enabled" : "disabled");
    printf("  Backend: %s\n", config.backend_type == AUDIO_BACKEND_ALSA ? "ALSA" : "PipeWire");
    printf("  Sample rate: %u Hz\n", config.audio_config.sample_rate);
    printf("  Buffer size: %u frames (%.2f ms)\n", 
           config.audio_config.frames,
           (double)config.audio_config.frames * 1000.0 / config.audio_config.sample_rate);
    
    if (config.backend_type == AUDIO_BACKEND_ALSA) {
        printf("  Capture device: %s (%u channels)\n", 
               config.audio_config.device_capture, config.audio_config.capture_channels);
        printf("  Playback device: %s (%u channels)\n", 
               config.audio_config.device_playback, config.audio_config.playback_channels);
    } else {
        printf("  Audio I/O: PipeWire filter with %u channels\n", config.audio_config.capture_channels);
    }
    printf("\n");
    
    // Run PWAR CLI
    int result = pwar_cli_run(&config);
    
    if (result < 0) {
        fprintf(stderr, "PWAR CLI failed to start\n");
        return 1;
    }
    
    printf("PWAR CLI finished successfully\n");
    return 0;
}
