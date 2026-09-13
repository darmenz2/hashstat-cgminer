/* SPDX-License-Identifier: GPL-3.0-or-later */

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static void term_exit(int signo) { (void)signo; _exit(42); }

static void record_pids(const char *path, pid_t descendant)
{
    FILE *stream = fopen(path, "wx");
    if (!stream) _exit(90);
    if (fprintf(stream, "%ld %ld\n", (long)getpid(), (long)descendant) < 0 ||
        fclose(stream)) _exit(91);
}

int main(int argc, char **argv)
{
    if (argc < 2) return 92;

    (void)alarm(15);
    if (!strcmp(argv[1], "exit") && argc == 3) return atoi(argv[2]);
    if (!strcmp(argv[1], "signal")) { (void)raise(SIGTERM); return 93; }
    if (!strcmp(argv[1], "stdin")) {
        char byte;
        return read(STDIN_FILENO, &byte, 1) == 0 ? 0 : 94;
    }
    if (!strcmp(argv[1], "fd") && argc == 3) {
        errno = 0;
        return fcntl(atoi(argv[2]), F_GETFD) == -1 && errno == EBADF ? 0 : 95;
    }
    if (!strcmp(argv[1], "blocked-write")) {
        char bytes[4096];
        memset(bytes, 'x', sizeof bytes);
        for (;;) { if (write(STDOUT_FILENO, bytes, sizeof bytes) < 0) return 96; }
    }
    if (!strcmp(argv[1], "blocked-read")) {
        int fds[2];
        char byte;
        if (pipe(fds)) return 97;

        return read(fds[0], &byte, 1) == 0 ? 98 : 99;
    }
    if (argc != 3) return 100;
    if (!strcmp(argv[1], "ignore-term") || !strcmp(argv[1], "descendants") ||
        !strcmp(argv[1], "exit-descendant")) {
        if (signal(SIGTERM, SIG_IGN) == SIG_ERR) return 101;
    } else if (!strcmp(argv[1], "handle-term")) {
        if (signal(SIGTERM, term_exit) == SIG_ERR) return 102;
    } else if (strcmp(argv[1], "sleep")) return 103;
    pid_t descendant = 0;
    if (!strcmp(argv[1], "descendants") || !strcmp(argv[1], "exit-descendant")) {
        descendant = fork();
        if (descendant < 0) return 104;
        if (!descendant) {
            (void)alarm(15);
            for (;;) pause();
        }
    }
    record_pids(argv[2], descendant);
    if (!strcmp(argv[1], "exit-descendant")) return 23;
    for (;;) pause();
}
