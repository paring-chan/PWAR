/*
 * simulator_main.cpp - PWAR Windows Client Simulator
 *
 * Simulates the PWAR ASIO driver's network mechanics for standalone testing
 * Uses the same UDP protocol, IOCP, and processing logic as pwarASIO.cpp
 *
 * (c) 2025 Philip K. Gisslow
 * This file is part of the PipeWire ASIO Relay (PWAR) project.
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>
#include <thread>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <fstream>
#include <sstream>
#include <iostream>
#include <signal.h>

#include "../../protocol/pwar_packet.h"
#include "../../protocol/pwar_router.h"
#include "../../protocol/latency_manager.h"

#include <avrt.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "avrt.lib")

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR,12)
#endif

#define PWAR_MAX_CHANNELS 2
#define DEFAULT_BUFFER_SIZE 512
#define DEFAULT_SAMPLE_RATE 48000.0

// Configuration structure
struct SimulatorConfig {
    std::string server_ip = "192.168.66.2";
    int server_port = 8321;
    int client_port = 8321;
    int buffer_size = DEFAULT_BUFFER_SIZE;
    int channels = PWAR_MAX_CHANNELS;
    double sample_rate = DEFAULT_SAMPLE_RATE;
    bool verbose = false;
    std::string config_file = "";
};

// Simulator state
class PWARClientSimulator {
private:
    SimulatorConfig config;
    pwar_router_t router;
    
    // Network components
    SOCKET udp_recv_socket;
    SOCKET udp_send_socket;
    sockaddr_in udp_send_addr;
    bool wsa_initialized;
    bool listener_running;
    std::thread listener_thread;
    
    // Audio buffers
    float* input_buffers;
    float* output_buffers;
    
    // Statistics
    uint64_t packets_processed;
    uint64_t packets_sent;
    std::chrono::steady_clock::time_point start_time;
    
    // Timing simulation
    int toggle;
    double sample_position;
    
public:
    static volatile bool keep_running;

public:
    PWARClientSimulator() : 
        udp_recv_socket(INVALID_SOCKET),
        udp_send_socket(INVALID_SOCKET),
        wsa_initialized(false),
        listener_running(false),
        input_buffers(nullptr),
        output_buffers(nullptr),
        packets_processed(0),
        packets_sent(0),
        toggle(0),
        sample_position(0.0)
    {
    }
    
    ~PWARClientSimulator() {
        stop();
        cleanup();
    }
    
    static void signal_handler(int sig) {
        printf("\nReceived signal %d, shutting down...\n", sig);
        keep_running = false;
    }
    
    void print_usage(const char* program_name) {
        printf("PWAR Windows Client Simulator - Standalone testing tool\n\n");
        printf("Usage: %s [options]\n", program_name);
        printf("Options:\n");
        printf("  -s, --server <ip>      Server IP address (default: %s)\n", config.server_ip.c_str());
        printf("  -p, --port <port>      Server port (default: %d)\n", config.server_port);
        printf("  -c, --client-port <port> Client listening port (default: %d)\n", config.client_port);
        printf("  -b, --buffer <size>    Buffer size in samples (default: %d)\n", config.buffer_size);
        printf("  -n, --channels <count> Number of channels (default: %d)\n", config.channels);
        printf("  -r, --rate <rate>      Sample rate (default: %.0f)\n", config.sample_rate);
        printf("  -f, --config <file>    Config file path (default: %%USERPROFILE%%\\pwarASIO.cfg)\n");
        printf("  -v, --verbose          Enable verbose output\n");
        printf("  -h, --help             Show this help message\n");
        printf("\nExamples:\n");
        printf("  %s                           # Connect with defaults\n", program_name);
        printf("  %s -s 192.168.1.100 -p 9000  # Connect to remote server\n", program_name);
        printf("  %s -v -b 256 -n 1            # Verbose mode, smaller buffer, mono\n", program_name);
        printf("\nDescription:\n");
        printf("  This simulator replicates the PWAR ASIO driver's network mechanics\n");
        printf("  for standalone testing without requiring a DAW or ASIO registration.\n");
        printf("  Uses the same IOCP-based UDP processing as the real driver.\n");
    }
    
    bool parse_arguments(int argc, char* argv[]) {
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
                print_usage(argv[0]);
                return false;
            } else if ((strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--server") == 0) && i + 1 < argc) {
                config.server_ip = argv[++i];
            } else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) && i + 1 < argc) {
                config.server_port = atoi(argv[++i]);
            } else if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--client-port") == 0) && i + 1 < argc) {
                config.client_port = atoi(argv[++i]);
            } else if ((strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--buffer") == 0) && i + 1 < argc) {
                config.buffer_size = atoi(argv[++i]);
            } else if ((strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--channels") == 0) && i + 1 < argc) {
                config.channels = atoi(argv[++i]);
            } else if ((strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--rate") == 0) && i + 1 < argc) {
                config.sample_rate = atof(argv[++i]);
            } else if ((strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--config") == 0) && i + 1 < argc) {
                config.config_file = argv[++i];
            } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
                config.verbose = true;
            } else {
                fprintf(stderr, "Unknown argument: %s\n", argv[i]);
                print_usage(argv[0]);
                return false;
            }
        }
        
        // Validate configuration
        if (config.server_port <= 0 || config.server_port > 65535) {
            fprintf(stderr, "Invalid server port: %d\n", config.server_port);
            return false;
        }
        if (config.client_port <= 0 || config.client_port > 65535) {
            fprintf(stderr, "Invalid client port: %d\n", config.client_port);
            return false;
        }
        if (config.channels < 1 || config.channels > 8) {
            fprintf(stderr, "Invalid channel count: %d (must be 1-8)\n", config.channels);
            return false;
        }
        if (config.buffer_size < 32 || config.buffer_size > 4096) {
            fprintf(stderr, "Invalid buffer size: %d (must be 32-4096)\n", config.buffer_size);
            return false;
        }
        if (config.sample_rate < 8000.0 || config.sample_rate > 192000.0) {
            fprintf(stderr, "Invalid sample rate: %.0f (must be 8000-192000)\n", config.sample_rate);
            return false;
        }
        
        return true;
    }
    
    void parse_config_file() {
        std::string config_path;
        if (!config.config_file.empty()) {
            config_path = config.config_file;
        } else {
            const char* home = getenv("USERPROFILE");
            if (home && *home) {
                config_path = std::string(home) + "\\pwarASIO.cfg";
            } else {
                config_path = "pwarASIO.cfg";
            }
        }
        
        std::ifstream cfg(config_path);
        if (!cfg.is_open()) {
            if (config.verbose) {
                printf("Config file not found: %s (using defaults)\n", config_path.c_str());
            }
            return;
        }
        
        printf("Reading config from: %s\n", config_path.c_str());
        std::string line;
        while (std::getline(cfg, line)) {
            std::istringstream iss(line);
            std::string key, value;
            if (std::getline(iss, key, '=') && std::getline(iss, value)) {
                if (key == "udp_send_ip") {
                    config.server_ip = value;
                    if (config.verbose) {
                        printf("Config: server IP = %s\n", value.c_str());
                    }
                }
            }
        }
    }
    
    bool initialize() {
        // Parse config file first
        parse_config_file();
        
        printf("PWAR Windows Client Simulator Configuration:\n");
        printf("  Server:        %s:%d\n", config.server_ip.c_str(), config.server_port);
        printf("  Client port:   %d\n", config.client_port);
        printf("  Channels:      %d\n", config.channels);
        printf("  Buffer size:   %d samples\n", config.buffer_size);
        printf("  Sample rate:   %.0f Hz\n", config.sample_rate);
        printf("  Verbose:       %s\n", config.verbose ? "enabled" : "disabled");
        printf("\n");
        
        // Initialize WSA
        WSADATA wsa_data;
        if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
            fprintf(stderr, "WSAStartup failed\n");
            return false;
        }
        wsa_initialized = true;
        
        // Initialize PWAR components
        latency_manager_init();
        pwar_router_init(&router, config.channels);
        
        // Allocate audio buffers
        input_buffers = new float[config.channels * config.buffer_size];
        output_buffers = new float[config.channels * config.buffer_size];
        if (!input_buffers || !output_buffers) {
            fprintf(stderr, "Failed to allocate audio buffers\n");
            return false;
        }
        
        // Initialize UDP sockets
        if (!init_udp_sender() || !init_udp_receiver()) {
            return false;
        }
        
        start_time = std::chrono::steady_clock::now();
        return true;
    }
    
    bool init_udp_sender() {
        udp_send_socket = socket(AF_INET, SOCK_DGRAM, 0);
        if (udp_send_socket == INVALID_SOCKET) {
            fprintf(stderr, "Failed to create UDP send socket: %d\n", WSAGetLastError());
            return false;
        }
        
        // Set up destination address
        memset(&udp_send_addr, 0, sizeof(udp_send_addr));
        udp_send_addr.sin_family = AF_INET;
        udp_send_addr.sin_port = htons(config.server_port);
        if (inet_pton(AF_INET, config.server_ip.c_str(), &udp_send_addr.sin_addr) <= 0) {
            fprintf(stderr, "Invalid server IP address: %s\n", config.server_ip.c_str());
            return false;
        }
        
        // Set SO_SNDBUF to minimal size for low latency (like ASIO driver)
        int sndbuf = 1024;
        setsockopt(udp_send_socket, SOL_SOCKET, SO_SNDBUF, (const char*)&sndbuf, sizeof(sndbuf));
        
        // Disable UDP connection reset behavior
        DWORD bytes_returned = 0;
        BOOL new_behavior = FALSE;
        WSAIoctl(udp_send_socket, SIO_UDP_CONNRESET, &new_behavior, sizeof(new_behavior),
                 NULL, 0, &bytes_returned, NULL, NULL);
        
        if (config.verbose) {
            printf("UDP sender initialized, target: %s:%d\n", 
                   config.server_ip.c_str(), config.server_port);
        }
        
        return true;
    }
    
    bool init_udp_receiver() {
        udp_recv_socket = WSASocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED);
        if (udp_recv_socket == INVALID_SOCKET) {
            fprintf(stderr, "Failed to create UDP receive socket: %d\n", WSAGetLastError());
            return false;
        }
        
        // Set minimal buffer sizes for ultra-low latency (like ASIO driver)
        int rcvbuf = 1024;
        setsockopt(udp_recv_socket, SOL_SOCKET, SO_RCVBUF, (const char*)&rcvbuf, sizeof(rcvbuf));
        
        // Disable UDP connection reset behavior
        DWORD bytes_returned = 0;
        BOOL new_behavior = FALSE;
        WSAIoctl(udp_recv_socket, SIO_UDP_CONNRESET, &new_behavior, sizeof(new_behavior),
                 NULL, 0, &bytes_returned, NULL, NULL);
        
        // Bind socket
        sockaddr_in recv_addr{};
        memset(&recv_addr, 0, sizeof(recv_addr));
        recv_addr.sin_family = AF_INET;
        recv_addr.sin_addr.s_addr = INADDR_ANY;
        recv_addr.sin_port = htons(config.client_port);
        
        if (bind(udp_recv_socket, reinterpret_cast<sockaddr*>(&recv_addr), sizeof(recv_addr)) == SOCKET_ERROR) {
            fprintf(stderr, "Failed to bind UDP receive socket to port %d: %d\n", 
                    config.client_port, WSAGetLastError());
            return false;
        }
        
        if (config.verbose) {
            printf("UDP receiver bound to port %d\n", config.client_port);
        }
        
        return true;
    }
    
    void output_packet(const pwar_packet_t& packet) {
        if (udp_send_socket != INVALID_SOCKET) {
            WSABUF buffer;
            buffer.buf = reinterpret_cast<CHAR*>(const_cast<pwar_packet_t*>(&packet));
            buffer.len = sizeof(pwar_packet_t);
            DWORD bytes_sent = 0;
            int flags = 0;
            int result = WSASendTo(udp_send_socket, &buffer, 1, &bytes_sent, flags,
                      reinterpret_cast<sockaddr*>(&udp_send_addr), sizeof(udp_send_addr), NULL, NULL);
            if (result == SOCKET_ERROR) {
                if (config.verbose) {
                    fprintf(stderr, "WSASendTo failed: %d\n", WSAGetLastError());
                }
            } else {
                packets_sent++;
            }
        }
    }
    
    // IOCP-based UDP listener (identical to ASIO driver)
    void udp_iocp_listener() {
        char buffer[2048];
        HANDLE iocp = NULL;
        
        // Declare variables at the top to avoid goto warnings
        pwar_packet_t output_packets[32];
        uint32_t packets_to_send = 0;
        OVERLAPPED overlapped = {0};
        WSABUF wsa_buf;
        DWORD bytes_received = 0;
        DWORD flags = 0;
        sockaddr_in cli_addr{};
        int len = sizeof(cli_addr);
        
        // --- Raise thread priority and register with MMCSS (like ASIO driver) ---
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        DWORD mmcss_task_index = 0;
        HANDLE mmcss_handle = AvSetMmThreadCharacteristicsA("Pro Audio", &mmcss_task_index);
        
        if (GetThreadPriority(GetCurrentThread()) != THREAD_PRIORITY_TIME_CRITICAL) {
            printf("Warning: Thread priority not set to TIME_CRITICAL!\n");
        } else if (config.verbose) {
            printf("IOCP Thread priority set to TIME_CRITICAL.\n");
        }
        
        if (!mmcss_handle) {
            printf("Warning: MMCSS registration failed!\n");
        } else if (config.verbose) {
            printf("IOCP MMCSS registration succeeded.\n");
        }

        // Create I/O Completion Port
        iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
        if (!iocp) {
            printf("Failed to create IOCP!\n");
            goto cleanup;
        }

        // Associate socket with IOCP
        if (!CreateIoCompletionPort((HANDLE)udp_recv_socket, iocp, (ULONG_PTR)udp_recv_socket, 0)) {
            printf("Failed to associate socket with IOCP!\n");
            goto cleanup;
        }

        listener_running = true;

        // Start the first async receive
        wsa_buf.buf = buffer;
        wsa_buf.len = sizeof(buffer);
        bytes_received = 0;
        flags = 0;
        len = sizeof(cli_addr);
        
        int result = WSARecvFrom(udp_recv_socket, &wsa_buf, 1, &bytes_received, &flags, 
                                reinterpret_cast<sockaddr*>(&cli_addr), &len, &overlapped, NULL);
        
        if (result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
            printf("Initial WSARecvFrom failed: %d\n", WSAGetLastError());
            goto cleanup;
        }

        printf("Started IOCP listener thread, waiting for packets...\n");

        while (listener_running && keep_running) {
            DWORD dw_bytes_transferred = 0;
            ULONG_PTR dw_completion_key = 0;
            LPOVERLAPPED lp_overlapped = NULL;
            
            // Wait for completion with minimal timeout for responsiveness
            BOOL success = GetQueuedCompletionStatus(iocp, &dw_bytes_transferred, &dw_completion_key, 
                                                    &lp_overlapped, 1); // 1ms timeout
            
            if (success && lp_overlapped && dw_bytes_transferred >= sizeof(pwar_packet_t)) {
                // Process the received packet (identical to ASIO driver)
                pwar_packet_t pkt;
                memcpy(&pkt, buffer, sizeof(pwar_packet_t));

                uint32_t chunk_size = pkt.n_samples;
                pkt.num_packets = config.buffer_size / chunk_size;
                latency_manager_process_packet_client(&pkt);

                int samples_ready = pwar_router_process_streaming_packet(&router, &pkt, input_buffers, 
                                                                       config.buffer_size, config.channels);

                if (samples_ready > 0) {
                    uint32_t seq = pkt.seq;
                    latency_manager_start_audio_cbk_begin();

                    // Simulate audio processing: copy input to output with simple processing
                    memset(output_buffers, 0, config.channels * config.buffer_size * sizeof(float));
                    
                    // Copy channel 0 to all channels (simple audio processing simulation)
                    for (int ch = 0; ch < config.channels; ch++) {
                        for (int i = 0; i < samples_ready; i++) {
                            output_buffers[ch * config.buffer_size + i] = input_buffers[i];
                        }
                    }

                    sample_position += config.buffer_size;
                    packets_processed++;

                    latency_manager_start_audio_cbk_end();

                    // Send the result
                    pwar_router_send_buffer(&router, chunk_size, output_buffers, samples_ready, 
                                          config.channels, output_packets, 32, &packets_to_send);

                    uint64_t timestamp = latency_manager_timestamp_now();
                    for (uint32_t i = 0; i < packets_to_send; ++i) {
                        output_packets[i].seq = seq;
                        output_packets[i].timestamp = timestamp;
                        output_packet(output_packets[i]);
                    }
                    toggle = toggle ? 0 : 1;

                    // Send latency info if needed
                    pwar_latency_info_t latency_info;
                    if (latency_manager_time_for_sending_latency_info(&latency_info)) {
                        if (udp_send_socket != INVALID_SOCKET) {
                            WSABUF latency_buffer;
                            latency_buffer.buf = reinterpret_cast<CHAR*>(&latency_info);
                            latency_buffer.len = sizeof(latency_info);
                            DWORD bytes_sent = 0;
                            int send_flags = 0;
                            WSASendTo(udp_send_socket, &latency_buffer, 1, &bytes_sent, send_flags,
                                      reinterpret_cast<sockaddr*>(&udp_send_addr), sizeof(udp_send_addr), NULL, NULL);
                        }
                    }
                    
                    if (config.verbose && packets_processed % 1000 == 0) {
                        auto now = std::chrono::steady_clock::now();
                        auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - start_time);
                        printf("Processed %llu packets in %lld seconds (sent: %llu)\n", 
                               packets_processed, duration.count(), packets_sent);
                    }
                }

                // Start the next async receive immediately
                memset(&overlapped, 0, sizeof(overlapped));
                wsa_buf.buf = buffer;
                wsa_buf.len = sizeof(buffer);
                bytes_received = 0;
                flags = 0;
                len = sizeof(cli_addr);
                
                result = WSARecvFrom(udp_recv_socket, &wsa_buf, 1, &bytes_received, &flags, 
                                    reinterpret_cast<sockaddr*>(&cli_addr), &len, &overlapped, NULL);
                
                if (result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
                    printf("WSARecvFrom failed: %d\n", WSAGetLastError());
                    break;
                }
            } else if (!success && lp_overlapped) {
                // Operation failed
                DWORD error = GetLastError();
                if (error != WAIT_TIMEOUT) {
                    printf("IOCP operation failed: %d\n", error);
                    break;
                }
            }
            // If timeout occurred, just continue the loop - keeps thread responsive
        }

cleanup:
        if (iocp) {
            CloseHandle(iocp);
        }
        
        if (mmcss_handle) {
            AvRevertMmThreadCharacteristics(mmcss_handle);
        }
        
        printf("IOCP listener thread stopped\n");
    }
    
    void start() {
        if (!listener_running) {
            listener_thread = std::thread(&PWARClientSimulator::udp_iocp_listener, this);
        }
    }
    
    void stop() {
        listener_running = false;
        if (listener_thread.joinable()) {
            listener_thread.join();
        }
    }
    
    void cleanup() {
        stop();
        
        if (udp_send_socket != INVALID_SOCKET) {
            closesocket(udp_send_socket);
            udp_send_socket = INVALID_SOCKET;
        }
        
        if (udp_recv_socket != INVALID_SOCKET) {
            closesocket(udp_recv_socket);
            udp_recv_socket = INVALID_SOCKET;
        }
        
        if (wsa_initialized) {
            WSACleanup();
            wsa_initialized = false;
        }
        
        if (input_buffers) {
            delete[] input_buffers;
            input_buffers = nullptr;
        }
        
        if (output_buffers) {
            delete[] output_buffers;
            output_buffers = nullptr;
        }
    }
    
    void print_stats() {
        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - start_time);
        
        printf("\nFinal Statistics:\n");
        printf("  Runtime:           %lld seconds\n", duration.count());
        printf("  Packets processed: %llu\n", packets_processed);
        printf("  Packets sent:      %llu\n", packets_sent);
        if (duration.count() > 0) {
            printf("  Average rate:      %.2f packets/sec\n", 
                   (double)packets_processed / duration.count());
        }
        printf("  Sample position:   %.0f samples (%.2f seconds)\n", 
               sample_position, sample_position / config.sample_rate);
    }
};

// Static member definition
volatile bool PWARClientSimulator::keep_running = true;

int main(int argc, char* argv[]) {
    printf("PWAR Windows Client Simulator - Standalone testing tool\n");
    printf("Replicates ASIO driver network mechanics for testing\n\n");
    
    PWARClientSimulator simulator;
    
    // Parse command line arguments
    if (!simulator.parse_arguments(argc, argv)) {
        return 1;
    }
    
    // Set up signal handlers
    signal(SIGINT, PWARClientSimulator::signal_handler);
    signal(SIGTERM, PWARClientSimulator::signal_handler);
    
    // Initialize simulator
    if (!simulator.initialize()) {
        fprintf(stderr, "Failed to initialize simulator\n");
        return 1;
    }
    
    // Start the simulator
    simulator.start();
    
    printf("Simulator started successfully. Press Ctrl+C to stop.\n");
    printf("Waiting for audio packets from PWAR server...\n\n");
    
    // Main loop
    while (PWARClientSimulator::keep_running) {
        Sleep(100); // 100ms
    }
    
    // Cleanup and show stats
    printf("\nShutting down...\n");
    simulator.stop();
    simulator.print_stats();
    
    printf("Shutdown complete\n");
    return 0;
}
