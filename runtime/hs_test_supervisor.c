/* SPDX-License-Identifier: GPL-3.0-or-later */

#define _GNU_SOURCE 1
#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#include <sys/proc.h>
#endif
#ifdef __linux__
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>

#define HS_MAX_TIMEOUT_MS UINT64_C(600000)
#define HS_TERM_GRACE_MS UINT64_C(150)
#define HS_REAP_GRACE_MS UINT64_C(600)
#define HS_POLL_SLICE_MS 10
#define HS_INTERNAL 125
#define HS_EXEC_FAILED 127
#define HS_TIMEOUT 124

static volatile sig_atomic_t interrupted;

static void on_signal(int signo) { if (!interrupted) interrupted = signo; }

static int clock_ms(uint64_t *value)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) || ts.tv_sec < 0) return -1;
    *value = (uint64_t)ts.tv_sec * UINT64_C(1000) + (uint64_t)ts.tv_nsec / UINT64_C(1000000);
    return 0;
}

static int parse_timeout(const char *text, uint64_t *value)
{
    uint64_t n = 0;
    if (!text || !*text) return -1;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p < '0' || *p > '9') return -1;
        n = n * UINT64_C(10) + (uint64_t)(*p - '0');
        if (n > HS_MAX_TIMEOUT_MS) return -1;
    }
    if (n < 1) return -1;
    *value = n;
    return 0;
}

static int close_extra_fds(void)
{
#ifdef __linux__
#ifdef SYS_close_range
    if (syscall(SYS_close_range, 3U, UINT_MAX, 0U) == 0) return 0;

#endif
    DIR *dir = opendir("/proc/self/fd");
#else

    DIR *dir = opendir("/dev/fd");
#endif
    if (!dir) return -1;
    int dfd = dirfd(dir);
    if (dfd < 0) { (void)closedir(dir); return -1; }
    struct dirent *ent;
    errno = 0;
    while ((ent = readdir(dir)) != NULL) {
        char *end = NULL;
        errno = 0;
        long n = strtol(ent->d_name, &end, 10);
        if (errno || end == ent->d_name || *end || n < 3 || n > INT_MAX || n == dfd) {
            errno = 0;
            continue;
        }

        if (close((int)n) && errno != EBADF && errno != EINTR) {
            (void)closedir(dir); return -1;
        }
        errno = 0;
    }
    int failed = errno;
    if (closedir(dir)) return -1;
    return failed ? -1 : 0;
}

static int install_handlers(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof action);
    sigemptyset(&action.sa_mask);
    action.sa_handler = on_signal;
    const int names[] = {SIGTERM, SIGHUP, SIGINT};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i)
        if (sigaction(names[i], &action, NULL)) return -1;
    action.sa_handler = SIG_DFL;

    if (sigaction(SIGCHLD, &action, NULL)) return -1;
    action.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &action, NULL)) return -1;
    sigset_t empty;
    sigemptyset(&empty);
    return sigprocmask(SIG_SETMASK, &empty, NULL);
}

static int reset_child_signals(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof action);
    sigemptyset(&action.sa_mask);
    action.sa_handler = SIG_DFL;

    for (int signo = 1; signo < NSIG; ++signo) {
        if (signo == SIGKILL || signo == SIGSTOP) continue;
        if (sigaction(signo, &action, NULL) && errno != EINVAL) return -1;
    }
    sigset_t empty;
    sigemptyset(&empty);
    return sigprocmask(SIG_SETMASK, &empty, NULL);
}

static void child_record(int fd, char kind)
{
    ssize_t n;
    do { n = write(fd, &kind, 1); } while (n < 0 && errno == EINTR);
    if (n != 1) _exit(HS_INTERNAL);
}

static void execute_child(int read_fd, int write_fd, int gate_read, int gate_write,
                          pid_t supervisor, char **argv)
{
    (void)close(read_fd);
    (void)close(gate_write);
#ifdef __linux__

    if (prctl(PR_SET_PDEATHSIG, (unsigned long)SIGKILL, 0UL, 0UL, 0UL) || getppid() != supervisor) {
        child_record(write_fd, 'S'); _exit(HS_INTERNAL);
    }
#else
    if (getppid() != supervisor) { child_record(write_fd, 'S'); _exit(HS_INTERNAL); }
#endif
    if (reset_child_signals()) { child_record(write_fd, 'S'); _exit(HS_INTERNAL); }

    char permission = 0;
    ssize_t got;
    do { got = read(gate_read, &permission, 1); } while (got < 0 && errno == EINTR);
    (void)close(gate_read);
    if (got != 1 || permission != 'G' || getpgrp() != getpid()) {
        child_record(write_fd, 'S'); _exit(HS_INTERNAL);
    }
    int null_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (null_fd < 0 || dup2(null_fd, STDIN_FILENO) < 0) {
        child_record(write_fd, 'S'); _exit(HS_INTERNAL);
    }
    if (null_fd != STDIN_FILENO) (void)close(null_fd);

    child_record(write_fd, 'R');
    execv(argv[0], argv);
    child_record(write_fd, 'E');
    _exit(HS_EXEC_FAILED);
}

static int signal_owned(pid_t child, int isolated, int signo)
{
    if (child <= 1) return -1;
    if (kill(isolated ? -child : child, signo) == 0 || errno == ESRCH) return 0;
#ifdef __APPLE__

    if (isolated && errno == EPERM) {
        struct kinfo_proc members[64];
        int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PGRP, child};
        uint64_t began;
        if (clock_ms(&began)) return -1;
        for (;;) {
            size_t length = sizeof members;
            if (sysctl(mib, 4, members, &length, NULL, 0) || length % sizeof members[0]) return -1;
            int live = 0;
            for (size_t i = 0; i < length / sizeof members[0]; ++i)
                if (members[i].kp_proc.p_stat != SZOMB) live = 1;
            if (!live) return 0;

            uint64_t now;
            if (clock_ms(&now) || now - began >= UINT64_C(20)) return -1;
            (void)poll(NULL, 0, 1);
        }
    }
#endif
    return -1;
}

static int observe_child(pid_t child, int *done)
{
    siginfo_t info;
    memset(&info, 0, sizeof info);
    if (waitid(P_PID, (id_t)child, &info, WEXITED | WNOHANG | WNOWAIT)) {
        if (errno == EINTR) { *done = 0; return 0; }
        return -1;
    }
    *done = info.si_pid == child;
    return 0;
}

static int final_reap(pid_t child, uint64_t deadline, int *status)
{
    int direct_reaped = 0;
    for (;;) {
        int st = 0;
        pid_t got = waitpid(direct_reaped ? -1 : child, &st, WNOHANG);
        if (got == child && !direct_reaped) { *status = st; direct_reaped = 1; continue; }
        if (got > 0) continue;
        if (got < 0 && errno == ECHILD) return direct_reaped ? 0 : -1;
        if (got < 0 && errno != EINTR) return -1;
        uint64_t now;
        if (clock_ms(&now) || now >= deadline) return -1;
        (void)poll(NULL, 0, HS_POLL_SLICE_MS);
    }
}

static int supervise(uint64_t timeout, char **argv)
{
#ifdef __linux__

    pid_t launcher = getppid();
#endif
    uint64_t start;
    if (clock_ms(&start) || close_extra_fds() || install_handlers()) return HS_INTERNAL;
#ifdef __linux__
    if (launcher <= 1 || prctl(PR_SET_PDEATHSIG, (unsigned long)SIGTERM, 0UL, 0UL, 0UL) ||
        prctl(PR_SET_CHILD_SUBREAPER, 1UL, 0UL, 0UL, 0UL)) return HS_INTERNAL;
    if (getppid() != launcher) return 128 + SIGTERM;
#endif

    for (int fd = 0; fd < 3; ++fd) {
        if (fcntl(fd, F_GETFD) < 0) {
            if (errno != EBADF) return HS_INTERNAL;
            int opened = open("/dev/null", fd == 0 ? O_RDONLY : O_WRONLY);
            if (opened != fd) { if (opened >= 0) (void)close(opened); return HS_INTERNAL; }
        }
    }
    int pipe_fds[2], gate[2];
    if (pipe(pipe_fds)) return HS_INTERNAL;
    if (pipe(gate)) { (void)close(pipe_fds[0]); (void)close(pipe_fds[1]); return HS_INTERNAL; }
    if (fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC) ||
        fcntl(pipe_fds[1], F_SETFD, FD_CLOEXEC) ||
        fcntl(gate[0], F_SETFD, FD_CLOEXEC) || fcntl(gate[1], F_SETFD, FD_CLOEXEC) ||
        fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK)) {
        (void)close(pipe_fds[0]); (void)close(pipe_fds[1]);
        (void)close(gate[0]); (void)close(gate[1]); return HS_INTERNAL;
    }
    pid_t supervisor = getpid();
    pid_t child = fork();
    if (child < 0) {
        (void)close(pipe_fds[0]); (void)close(pipe_fds[1]);
        (void)close(gate[0]); (void)close(gate[1]); return HS_INTERNAL;
    }
    if (child == 0) execute_child(pipe_fds[0], pipe_fds[1], gate[0], gate[1], supervisor, argv);
    (void)close(pipe_fds[1]);
    (void)close(gate[0]);
    int isolated = setpgid(child, child) == 0;
    int failure = isolated ? 0 : HS_INTERNAL;
    if (isolated) {
        ssize_t n;
        do { n = write(gate[1], "G", 1); } while (n < 0 && errno == EINTR);
        if (n != 1) failure = HS_INTERNAL;
    }
    (void)close(gate[1]);
    int ready = 0, pipe_eof = 0, done = 0;
    int result = 0;
    for (;;) {
        if (!pipe_eof) {
            char bytes[4];
            ssize_t n = read(pipe_fds[0], bytes, sizeof bytes);
            if (n == 0) pipe_eof = 1;
            else if (n > 0) {
                for (ssize_t i = 0; i < n; ++i) {
                    if (bytes[i] == 'R' && !ready && !failure) ready = 1;
                    else if (bytes[i] == 'E' && ready && !failure) failure = HS_EXEC_FAILED;
                    else if (bytes[i] == 'S' && !failure) failure = HS_INTERNAL;
                    else failure = HS_INTERNAL;
                }
                continue;
            } else if (errno != EINTR && errno != EAGAIN) { result = HS_INTERNAL; break; }
        }
        uint64_t now;
        if (clock_ms(&now)) { result = HS_INTERNAL; break; }
        if (interrupted) { result = 128 + interrupted; break; }
        if (observe_child(child, &done)) { result = HS_INTERNAL; break; }
        if (done) { result = failure; break; }

        if (now - start >= timeout) { result = failure ? failure : HS_TIMEOUT; break; }

        struct pollfd pfd = {pipe_fds[0], POLLIN, 0};
        (void)poll(pipe_eof ? NULL : &pfd, pipe_eof ? 0 : 1, HS_POLL_SLICE_MS);
    }
    (void)close(pipe_fds[0]);

    if (!done && signal_owned(child, isolated, SIGTERM)) result = HS_INTERNAL;
    uint64_t cleanup_start;
    if (clock_ms(&cleanup_start)) cleanup_start = start + timeout;
    if (!done) {
        for (;;) {
            uint64_t now;
            if (clock_ms(&now) || now - cleanup_start >= HS_TERM_GRACE_MS) break;
            (void)poll(NULL, 0, HS_POLL_SLICE_MS);
        }
    }
    if (signal_owned(child, isolated, SIGKILL)) result = HS_INTERNAL;
    int status = 0;
    uint64_t reap_start;
    if (clock_ms(&reap_start)) reap_start = cleanup_start;
    if (final_reap(child, reap_start + HS_REAP_GRACE_MS, &status)) {
        return HS_INTERNAL;
    }
    if (result) return result;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return HS_INTERNAL;
}

int main(int argc, char **argv)
{
    uint64_t timeout;
    if (argc < 3 || parse_timeout(argv[1], &timeout) || argv[2][0] != '/' || !argv[2][1]) {
        static const char usage[] = "usage: hashstat-test-supervisor TIMEOUT_MS /absolute/test [args...] (1..600000 ms)\n";
        (void)write(STDERR_FILENO, usage, sizeof usage - 1);
        return HS_INTERNAL;
    }
    return supervise(timeout, &argv[2]);
}
