/*
 * client_simulator.cpp - PWAR Client Simulator (Windows)
 *
 * Simulates a PWAR client (like Windows ASIO driver) for testing
 * Receives audio from PWAR server, processes it, and sends it back
 *
 * (c) 2025 Philip K. Gisslow
 * This file is part of the PipeWire ASIO Relay (PWAR) project.
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <ctime>
#include <csignal>

extern "C" {
#include "../../protocol/pwar_packet.h"
#include "../../protocol/pwar_router.h"
#include "../../protocol/latency_manager.h"
}

#pragma comment(lib, "ws2_32.lib")

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR,12)
#endif

// Default configuration (matching your existing setup)
#define DEFAULT_SERVER_IP "196.168.66.2"
#define DEFAULT_SERVER_PORT 8321
#define DEFAULT_CLIENT_PORT 8321
#define DEFAULT_CHANNELS 2
#define DEFAULT_BUFFER_SIZE 512

// Configuration structure
typedef struct {
    char server_ip[64];
    int server_port;
    int client_port;
    int channels;
    int buffer_size;
    int verbose;
} client_config_t;

// Global state
static SOCKET recv_sockfd = INVALID_SOCKET;
static SOCKET send_sockfd = INVALID_SOCKET;
static HANDLE packet_mutex;
static HANDLE packet_event;
static pwar_packet_t latest_packet;
static int packet_available = 0;
static pwar_router_t router;
static struct sockaddr_in servaddr;
static volatile int keep_running = 1;
static client_config_t config;

static void print_usage(const char *program_name) {
    printf("PWAR Client Simulator - Simulates a PWAR client for testing\n\n");
    printf("Usage: %s [options]\n", program_name);
    printf("Options:\n");
    printf("  -s, --server <ip>      Server IP address (default: %s)\n", DEFAULT_SERVER_IP);
    printf("  -p, --port <port>      Server port (default: %d)\n", DEFAULT_SERVER_PORT);
    printf("  -c, --client-port <port> Client listening port (default: %d)\n", DEFAULT_CLIENT_PORT);
    printf("  -b, --buffer <size>    Buffer size in samples (default: %d)\n", DEFAULT_BUFFER_SIZE);
    printf("  -n, --channels <count> Number of channels (default: %d)\n", DEFAULT_CHANNELS);
    printf("  -v, --verbose          Enable verbose output\n");
    printf("  -h, --help             Show this help message\n");
    printf("\nExamples:\n");
    printf("  %s                           # Connect to localhost with defaults\n", program_name);
    printf("  %s -s 192.168.1.100 -p 9000  # Connect to remote server\n", program_name);
    printf("  %s -v -b 256 -c 1            # Verbose mode, smaller buffer, mono\n", program_name);
    printf("\nDescription:\n");
    printf("  This simulator acts like a PWAR client (e.g., Windows ASIO driver).\n");
    printf("  It receives audio packets from a PWAR server, processes them,\n");
    printf("  and sends them back, creating a loopback test environment.\n");
}

static int parse_arguments(int argc, char *argv[], client_config_t *cfg) {
    // Set defaults
    strcpy_s(cfg->server_ip, sizeof(cfg->server_ip), DEFAULT_SERVER_IP);
    cfg->server_port = DEFAULT_SERVER_PORT;
    cfg->client_port = DEFAULT_CLIENT_PORT;
    cfg->channels = DEFAULT_CHANNELS;
    cfg->buffer_size = DEFAULT_BUFFER_SIZE;
    cfg->verbose = 0;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return -1;
        } else if ((strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--server") == 0) && i + 1 < argc) {
            strcpy_s(cfg->server_ip, sizeof(cfg->server_ip), argv[++i]);
        } else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) && i + 1 < argc) {
            cfg->server_port = atoi(argv[++i]);
        } else if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--client-port") == 0) && i + 1 < argc) {
            cfg->client_port = atoi(argv[++i]);
        } else if ((strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--buffer") == 0) && i + 1 < argc) {
            cfg->buffer_size = atoi(argv[++i]);
        } else if ((strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--channels") == 0) && i + 1 < argc) {
            cfg->channels = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            cfg->verbose = 1;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            print_usage(argv[0]);
            return -1;
        }
    }
    
    // Validate configuration
    if (cfg->server_port <= 0 || cfg->server_port > 65535) {
        fprintf(stderr, "Invalid server port: %d\n", cfg->server_port);
        return -1;
    }
    if (cfg->client_port <= 0 || cfg->client_port > 65535) {
        fprintf(stderr, "Invalid client port: %d\n", cfg->client_port);
        return -1;
    }
    if (cfg->channels < 1 || cfg->channels > 8) {
        fprintf(stderr, "Invalid channel count: %d (must be 1-8)\n", cfg->channels);
        return -1;
    }
    if (cfg->buffer_size < 32 || cfg->buffer_size > 4096) {
        fprintf(stderr, "Invalid buffer size: %d (must be 32-4096)\n", cfg->buffer_size);
        return -1;
    }
    
    return 0;
}

static void setup_recv_socket(int port) {
    recv_sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (recv_sockfd == INVALID_SOCKET) {
        fprintf(stderr, "recv socket creation failed: %d\n", WSAGetLastError());
        exit(EXIT_FAILURE);
    }
    
    // Disable ICMP port unreachable messages
    BOOL new_behavior = FALSE;
    DWORD bytes_returned = 0;
    WSAIoctl(recv_sockfd, SIO_UDP_CONNRESET, &new_behavior, sizeof(new_behavior),
             NULL, 0, &bytes_returned, NULL, NULL);
    
    // Set socket options for better performance
    int rcvbuf = 1024 * 1024;
    if (setsockopt(recv_sockfd, SOL_SOCKET, SO_RCVBUF, (char*)&rcvbuf, sizeof(rcvbuf)) == SOCKET_ERROR) {
        fprintf(stderr, "Warning: Failed to set receive buffer size: %d\n", WSAGetLastError());
    }
    
    // Set socket timeout to allow periodic checking of keep_running flag
    DWORD timeout = 100; // 100ms timeout
    if (setsockopt(recv_sockfd, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout)) == SOCKET_ERROR) {
        fprintf(stderr, "Warning: Failed to set socket timeout: %d\n", WSAGetLastError());
    }
    
    struct sockaddr_in recv_addr;
    memset(&recv_addr, 0, sizeof(recv_addr));
    recv_addr.sin_family = AF_INET;
    recv_addr.sin_addr.s_addr = INADDR_ANY;
    recv_addr.sin_port = htons((u_short)port);
    
    if (bind(recv_sockfd, (struct sockaddr *)&recv_addr, sizeof(recv_addr)) == SOCKET_ERROR) {
        fprintf(stderr, "recv socket bind failed: %d\n", WSAGetLastError());
        exit(EXIT_FAILURE);
    }
    
    if (config.verbose) {
        printf("[Client Simulator] Listening on port %d\n", port);
    }
}

static void setup_send_socket(const char *server_ip, int server_port) {
    send_sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (send_sockfd == INVALID_SOCKET) {
        fprintf(stderr, "send socket creation failed: %d\n", WSAGetLastError());
        exit(EXIT_FAILURE);
    }
    
    // Disable ICMP port unreachable messages
    BOOL new_behavior = FALSE;
    DWORD bytes_returned = 0;
    WSAIoctl(send_sockfd, SIO_UDP_CONNRESET, &new_behavior, sizeof(new_behavior),
             NULL, 0, &bytes_returned, NULL, NULL);
    
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons((u_short)server_port);
    
    if (inet_pton(AF_INET, server_ip, &servaddr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid server IP address: %s\n", server_ip);
        exit(EXIT_FAILURE);
    }
    
    if (config.verbose) {
        printf("[Client Simulator] Sending to %s:%d\n", server_ip, server_port);
    }
}

static DWORD WINAPI receiver_thread(LPVOID userdata) {
    // Set thread priority to high for better real-time performance
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST)) {
        fprintf(stderr, "Warning: Failed to set high thread priority: %lu\n", GetLastError());
    }
    
    pwar_packet_t packet;
    pwar_packet_t output_packets[32];
    uint32_t packets_to_send = 0;
    uint64_t packets_processed = 0;
    
    float *output_buffers = (float*)malloc(config.channels * config.buffer_size * sizeof(float));
    if (!output_buffers) {
        fprintf(stderr, "Failed to allocate output buffers\n");
        return 1;
    }
    
    printf("[Client Simulator] Receiver thread started\n");
    
    while (keep_running) {
        int n = recvfrom(recv_sockfd, (char*)&packet, sizeof(packet), 0, NULL, NULL);
        
        if (n == sizeof(packet)) {
            WaitForSingleObject(packet_mutex, INFINITE);
            latest_packet = packet;
            packet_available = 1;
            
            // Clear output buffer
            memset(output_buffers, 0, config.channels * config.buffer_size * sizeof(float));
            
            uint32_t chunk_size = packet.n_samples;
            packet.num_packets = config.buffer_size / chunk_size;
            
            // Process packet with latency measurement
            latency_manager_process_packet_client(&packet);
            int samples_ready = pwar_router_process_streaming_packet(&router, &packet, 
                                                                   output_buffers, 
                                                                   config.buffer_size, 
                                                                   config.channels);
            
            if (samples_ready > 0) {
                uint32_t seq = packet.seq;
                
                latency_manager_start_audio_cbk_begin();
                
                // Simple audio processing: copy channel 0 to all other channels
                // This simulates what a real audio application might do
                for (uint32_t ch = 1; ch < config.channels; ch++) {
                    for (uint32_t i = 0; i < samples_ready; i++) {
                        output_buffers[ch * config.buffer_size + i] = 
                            output_buffers[i]; // Copy from channel 0
                    }
                }
                
                latency_manager_start_audio_cbk_end();
                
                // Send processed audio back to server
                pwar_router_send_buffer(&router, chunk_size, output_buffers, 
                                      samples_ready, config.channels, 
                                      output_packets, 32, &packets_to_send);
                
                uint64_t timestamp = latency_manager_timestamp_now();
                
                // Set sequence and timestamp for all packets
                for (uint32_t i = 0; i < packets_to_send; i++) {
                    output_packets[i].seq = seq;
                    output_packets[i].timestamp = timestamp;
                }
                
                // Send all packets
                for (uint32_t i = 0; i < packets_to_send; i++) {
                    int sent = sendto(send_sockfd, (char*)&output_packets[i], 
                                    sizeof(output_packets[i]), 0, 
                                    (struct sockaddr *)&servaddr, 
                                    sizeof(servaddr));
                    if (sent == SOCKET_ERROR) {
                        int error = WSAGetLastError();
                        if (error != WSAEWOULDBLOCK) {
                            fprintf(stderr, "sendto failed: %d\n", error);
                        }
                    }
                }
                
                packets_processed++;
                if (config.verbose && packets_processed % 1000 == 0) {
                    printf("[Client Simulator] Processed %llu packets\n", 
                           (unsigned long long)packets_processed);
                }
            }
            
            // Handle latency info
            pwar_latency_info_t latency_info;
            if (latency_manager_time_for_sending_latency_info(&latency_info)) {
                int sent = sendto(send_sockfd, (char*)&latency_info, sizeof(latency_info), 0,
                                (struct sockaddr *)&servaddr, sizeof(servaddr));
                if (sent == SOCKET_ERROR) {
                    int error = WSAGetLastError();
                    if (error != WSAEWOULDBLOCK) {
                        fprintf(stderr, "latency info sendto failed: %d\n", error);
                    }
                }
            }
            
            SetEvent(packet_event);
            ReleaseMutex(packet_mutex);
        } else if (n == SOCKET_ERROR) {
            int error = WSAGetLastError();
            // Check if it's a timeout (expected) vs a real error
            if (error == WSAETIMEDOUT || error == WSAEWOULDBLOCK) {
                // Timeout is expected - just continue and check keep_running
                continue;
            } else if (keep_running) {
                // Only print error if we're not shutting down
                fprintf(stderr, "recvfrom error: %d\n", error);
            }
        }
        // If n == 0 or some other size, just continue
    }
    
    printf("[Client Simulator] Receiver thread stopped\n");
    free(output_buffers);
    return 0;
}

static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    switch (ctrl_type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        printf("\n[Client Simulator] Received shutdown signal, shutting down...\n");
        keep_running = 0;
        return TRUE;
    default:
        return FALSE;
    }
}

int main(int argc, char *argv[]) {
    printf("PWAR Client Simulator - Testing tool for PWAR protocol (Windows)\n");
    printf("Simulates a PWAR client (like Windows ASIO driver)\n\n");
    
    // Initialize Winsock
    WSADATA wsa_data;
    int wsa_result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (wsa_result != 0) {
        fprintf(stderr, "WSAStartup failed: %d\n", wsa_result);
        return 1;
    }
    
    // Parse command line arguments
    if (parse_arguments(argc, argv, &config) < 0) {
        WSACleanup();
        return 1;
    }
    
    // Print configuration
    printf("Configuration:\n");
    printf("  Server:        %s:%d\n", config.server_ip, config.server_port);
    printf("  Client port:   %d\n", config.client_port);
    printf("  Channels:      %d\n", config.channels);
    printf("  Buffer size:   %d samples\n", config.buffer_size);
    printf("  Verbose:       %s\n", config.verbose ? "enabled" : "disabled");
    printf("\n");
    
    // Set up console control handler for graceful shutdown
    if (!SetConsoleCtrlHandler(console_ctrl_handler, TRUE)) {
        fprintf(stderr, "Warning: Failed to set console control handler\n");
    }
    
    // Create synchronization objects
    packet_mutex = CreateMutex(NULL, FALSE, NULL);
    if (packet_mutex == NULL) {
        fprintf(stderr, "Failed to create mutex: %lu\n", GetLastError());
        WSACleanup();
        return 1;
    }
    
    packet_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (packet_event == NULL) {
        fprintf(stderr, "Failed to create event: %lu\n", GetLastError());
        CloseHandle(packet_mutex);
        WSACleanup();
        return 1;
    }
    
    // Initialize PWAR components
    latency_manager_init();
    pwar_router_init(&router, config.channels);
    
    // Set up networking
    setup_recv_socket(config.client_port);
    setup_send_socket(config.server_ip, config.server_port);
    
    // Start receiver thread
    HANDLE recv_thread = CreateThread(NULL, 0, receiver_thread, NULL, 0, NULL);
    if (recv_thread == NULL) {
        fprintf(stderr, "Failed to create receiver thread: %lu\n", GetLastError());
        closesocket(recv_sockfd);
        closesocket(send_sockfd);
        CloseHandle(packet_event);
        CloseHandle(packet_mutex);
        WSACleanup();
        return 1;
    }
    
    printf("[Client Simulator] Started successfully. Press Ctrl+C to stop.\n");
    printf("[Client Simulator] Waiting for audio packets from PWAR server...\n");
    
    // Main loop
    while (keep_running) {
        Sleep(100); // 100ms
    }
    
    // Cleanup
    printf("[Client Simulator] Shutting down...\n");
    keep_running = 0;
    WaitForSingleObject(recv_thread, 5000); // Wait up to 5 seconds for thread to finish
    CloseHandle(recv_thread);
    
    if (recv_sockfd != INVALID_SOCKET) {
        closesocket(recv_sockfd);
    }
    if (send_sockfd != INVALID_SOCKET) {
        closesocket(send_sockfd);
    }
    
    CloseHandle(packet_event);
    CloseHandle(packet_mutex);
    WSACleanup();
    
    printf("[Client Simulator] Shutdown complete\n");
    return 0;
}