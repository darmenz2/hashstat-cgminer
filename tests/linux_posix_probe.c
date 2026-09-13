/* SPDX-License-Identifier: GPL-3.0-only */

#define _POSIX_C_SOURCE 200809L
#include <netdb.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static void *thread_check(void *arg)
{
    uint32_t *value = arg;
    *value = UINT32_C(0x48534142);
    return arg;
}
int main(void)
{
    pthread_t thread;
    uint32_t value = 0;
    void *result = NULL;
    struct timespec now;
    struct addrinfo hints = {0}, *address = NULL;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 ||
        now.tv_nsec < 0 || now.tv_nsec >= 1000000000L) return 1;
    if (pthread_create(&thread, NULL, thread_check, &value) != 0) return 2;
    if (pthread_join(thread, &result) != 0 || result != &value ||
        value != UINT32_C(0x48534142)) return 3;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    if (getaddrinfo("127.0.0.1", "1", &hints, &address) != 0 || address == NULL) return 4;
    freeaddrinfo(address);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 5;
    if (close(fd) != 0) return 6;
    if (puts("HASHSTAT ARM LINUX POSIX PROBE PASS; NO HARDWARE TEST") < 0) return 7;
    return 0;
}
