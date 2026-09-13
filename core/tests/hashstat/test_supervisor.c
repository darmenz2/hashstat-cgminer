/* SPDX-License-Identifier: GPL-3.0-only */

#define _POSIX_C_SOURCE 200809L
#include "hashstat-supervisor.h"
#include <arpa/inet.h>
#include <errno.h>
#include <jansson.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static unsigned checks;
static pid_t owned_child = -1;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static uint64_t now_ms(void)
{
    struct timespec ts; CHECK(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (uint64_t)ts.tv_sec*1000+(uint64_t)ts.tv_nsec/1000000;
}
static void pause_ms(unsigned ms)
{
    struct timespec ts = {(time_t)(ms/1000), (long)(ms%1000)*1000000};
    while (nanosleep(&ts, &ts) < 0) CHECK(errno == EINTR);
}
static int wait_fd(int fd, short events, int timeout)
{
    struct pollfd p = {fd, events, 0};
    int result;
    do { result = poll(&p, 1, timeout); } while (result < 0 && errno == EINTR);
    CHECK(result >= 0);
    return result ? p.revents : 0;
}
static json_t *decode(const char *data, size_t size)
{
    CHECK(size > 1 && data[size-1] == '\0');
    json_error_t error;
    json_t *root = json_loadb(data, size-1, JSON_REJECT_DUPLICATES, &error);
    CHECK(json_is_object(root));
    json_t *hs = json_array_get(json_object_get(root, "HASHSTAT"), 0);
    CHECK(json_is_object(hs));
    CHECK(!strcmp(json_string_value(json_object_get(hs, "Version")), HS_SUPERVISOR_VERSION));
    CHECK(json_is_false(json_object_get(hs, "HardwareInitialized")));
    CHECK(json_is_false(json_object_get(hs, "MiningEnabled")));
    CHECK(json_is_false(json_object_get(hs, "FullCgminerAPICompatible")));
    CHECK(json_is_null(json_object_get(hs, "ASICCount")));
    CHECK(json_is_null(json_object_get(hs, "FanCount")));
    return root;
}
static bool error_response(json_t *root)
{
    json_t *status = json_array_get(json_object_get(root, "STATUS"), 0);
    CHECK(json_is_object(status));
    return !strcmp(json_string_value(json_object_get(status, "STATUS")), "E");
}
static enum hs_supervisor_action pure(struct hs_supervisor_state *state, const char *request,
                                     size_t length, bool error)
{
    char output[HS_SUPERVISOR_MAX_RESPONSE]; size_t written;
    enum hs_supervisor_action action;
    CHECK(hashstat_supervisor_handle(state, request, length, 123456, output,
          sizeof(output), &written, &action) == HS_SUPERVISOR_RESPONSE);
    json_t *root = decode(output, written);
    CHECK(error_response(root) == error);
    json_decref(root); return action;
}
static void pure_tests(void)
{
    struct hs_supervisor_state state = HS_SUPERVISOR_STATE_INIT;
    const char *json = "{\"command\":\"status\"}";
    char output[HS_SUPERVISOR_MAX_RESPONSE]; size_t written;
    enum hs_supervisor_action action;
    for (size_t i = 0; i < strlen(json); ++i) {
        CHECK(hashstat_supervisor_handle(&state, json, i, 0, output, sizeof(output),
              &written, &action) == HS_SUPERVISOR_NEED_MORE);
        CHECK(state.phase == HS_SUPERVISOR_WAITING_HARDWARE && written == 0 && action == HS_SUPERVISOR_CONTINUE);
    }
    CHECK(pure(&state, json, strlen(json), false) == HS_SUPERVISOR_CONTINUE);
    const char *bad[] = { "{\"command\":\"quit\",\"command\":\"status\"}",
        "{\"command\":\"quit\",\"extra\":1}", "{\"command\":\"quit\\u0000extra\"}",
        "{\"command\":[\"quit\"]}", "{\"command\":\"quit\"}{}", "unknown-command",
        "{\"command\":\"status\",\"parameter\":\"secret\"}",
        "{\"command\":\"check\",\"parameter\":{}}", "{\"command\":7}",
        "{\"command\\u0000extra\":\"quit\"}" };
    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) {
        CHECK(pure(&state, bad[i], strlen(bad[i]), true) == HS_SUPERVISOR_CONTINUE);
        CHECK(state.phase == HS_SUPERVISOR_WAITING_HARDWARE);
    }
    char over[HS_SUPERVISOR_MAX_REQUEST+1]; memset(over, 'x', sizeof(over));
    CHECK(pure(&state, over, sizeof(over), true) == HS_SUPERVISOR_CONTINUE);
    const char *quit_json = "{\"command\":\"quit\"}";
    char trailing[64]; size_t quit_length = strlen(quit_json);
    memcpy(trailing, quit_json, quit_length);
    for (unsigned byte = 0; byte < 256; ++byte) {
        trailing[quit_length] = (char)byte;
        bool permitted = byte == 0 || byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n';
        struct hs_supervisor_state fresh = HS_SUPERVISOR_STATE_INIT;
        CHECK(pure(&fresh, trailing, quit_length+1, !permitted) ==
              (permitted ? HS_SUPERVISOR_QUIT : HS_SUPERVISOR_CONTINUE));
        CHECK(fresh.phase == (permitted ? HS_SUPERVISOR_SHUTDOWN : HS_SUPERVISOR_WAITING_HARDWARE));
    }
    CHECK(pure(&state, "stop", 4, false) == HS_SUPERVISOR_CONTINUE);
    CHECK(state.phase == HS_SUPERVISOR_STOPPED);
    CHECK(pure(&state, "stop", 4, false) == HS_SUPERVISOR_CONTINUE);
    CHECK(pure(&state, "start", 5, true) == HS_SUPERVISOR_CONTINUE);
    CHECK(pure(&state, "resume", 6, true) == HS_SUPERVISOR_CONTINUE);
    CHECK(state.phase == HS_SUPERVISOR_STOPPED);
    CHECK(hashstat_supervisor_handle(&state, "quit", 4, 0, output, 1, &written,
          &action) == HS_SUPERVISOR_ERROR);
    CHECK(state.phase == HS_SUPERVISOR_STOPPED && action == HS_SUPERVISOR_CONTINUE && written == 0);
    CHECK(pure(&state, "quit", 4, false) == HS_SUPERVISOR_QUIT);
    CHECK(pure(&state, "quit", 4, false) == HS_SUPERVISOR_QUIT);
    CHECK(pure(&state, "restart", 7, true) == HS_SUPERVISOR_CONTINUE);
    CHECK(pure(&state, "stop", 4, true) == HS_SUPERVISOR_CONTINUE);
    CHECK(state.phase == HS_SUPERVISOR_SHUTDOWN);
    state.phase = HS_SUPERVISOR_WAITING_HARDWARE;
    CHECK(pure(&state, "recover", 7, false) == HS_SUPERVISOR_RECOVER);
    CHECK(pure(&state, "restart", 7, false) == HS_SUPERVISOR_RECOVER);
    CHECK(state.phase == HS_SUPERVISOR_RECOVERY);
    const char *invalid_args[][8] = {
        {"fixture", "--other", NULL}, {"fixture", "--hashstat-standby", "--pool", "ignored", NULL},
        {"fixture", "--hashstat-standby", "--hashstat-standby-port", "0", NULL},
        {"fixture", "--hashstat-standby", "--hashstat-standby-port", "65536", NULL},
        {"fixture", "--hashstat-standby", "--hashstat-standby-port", "+4029", NULL},
        {"fixture", "--hashstat-standby", "--hashstat-standby-port", "99999999999999999999", NULL},
        {"fixture", "--hashstat-standby", "--hashstat-standby-timeout-ms", "10001", NULL},
        {"fixture", "--hashstat-standby", "--hashstat-standby-max-clients", "5", NULL},
        {"fixture", "--hashstat-standby", "--hashstat-standby-port", "4029", "--hashstat-standby-port", "4030", NULL},
        {"fixture", "--hashstat-standby", "--hashstat-standby-port", NULL}
    };
    for (unsigned i = 0; i < sizeof(invalid_args)/sizeof(invalid_args[0]); ++i) {
        int argc = 0; while (invalid_args[i][argc]) ++argc;
        CHECK(hashstat_supervisor_main(argc, (char **)invalid_args[i]) == 2);
    }
}
static void cleanup(void)
{
    if (owned_child > 0) {
        kill(owned_child, SIGKILL);
        while (waitpid(owned_child, NULL, 0) < 0 && errno == EINTR) {}
        owned_child = -1;
    }
}
static void listening(void *context, unsigned port)
{
    int fd = *(int *)context;
    uint16_t value = (uint16_t)port;
    if (write(fd, &value, sizeof(value)) != sizeof(value)) _exit(90);
    close(fd);
}
static unsigned launch(unsigned timeout, unsigned clients)
{
    CHECK(owned_child < 0);
    int channel[2]; CHECK(pipe(channel) == 0);
    owned_child = fork(); CHECK(owned_child >= 0);
    if (!owned_child) {
        close(channel[0]); alarm(10);
        struct hs_supervisor_config config = HS_SUPERVISOR_CONFIG_INIT;
        config.port = 0; config.timeout_ms = timeout; config.max_clients = clients;
        config.listening = listening; config.context = &channel[1];
        _exit(hashstat_supervisor_run(&config));
    }
    close(channel[1]);
    CHECK(wait_fd(channel[0], POLLIN, 3000) & POLLIN);
    uint16_t port = 0; CHECK(read(channel[0], &port, sizeof(port)) == sizeof(port));
    close(channel[0]); CHECK(port != 0); return port;
}
static void finish(int expected)
{
    uint64_t start = now_ms(); int status = 0;
    for (;;) {
        pid_t found = waitpid(owned_child, &status, WNOHANG);
        if (found == owned_child) { owned_child = -1; break; }
        CHECK(found == 0 || (found < 0 && errno == EINTR));
        CHECK(now_ms()-start < 3000); pause_ms(10);
    }
    CHECK(WIFEXITED(status)); CHECK(WEXITSTATUS(status) == expected);
}
static int connect_local(unsigned port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0); CHECK(fd >= 0);
    struct sockaddr_in address; memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(UINT32_C(0x7f000001));
    address.sin_port = htons((uint16_t)port);
    CHECK(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    return fd;
}
static void send_bytes(int fd, const char *data, size_t size)
{
    size_t sent = 0;
    while (sent < size) {
        CHECK(wait_fd(fd, POLLOUT, 1000) & POLLOUT);
        ssize_t n = send(fd, data+sent, size-sent, 0); CHECK(n > 0); sent += (size_t)n;
    }
}
static json_t *receive(int fd)
{
    char data[HS_SUPERVISOR_MAX_RESPONSE]; size_t used = 0;
    while (!used || data[used-1] != '\0') {
        CHECK(wait_fd(fd, POLLIN, 1500) & (POLLIN | POLLHUP));
        ssize_t n = recv(fd, data+used, sizeof(data)-used, 0); CHECK(n > 0);
        used += (size_t)n; CHECK(used < sizeof(data));
    }
    json_t *root = decode(data, used);
    close(fd); return root;
}
static json_t *request(unsigned port, const char *data)
{
    int fd = connect_local(port); send_bytes(fd, data, strlen(data)); return receive(fd);
}
static void closed(int fd)
{
    CHECK(wait_fd(fd, POLLIN, 1500) & (POLLIN | POLLHUP | POLLERR));
    char byte;
    ssize_t result = recv(fd, &byte, 1, 0);
    CHECK(result == 0 || (result < 0 && (errno == ECONNRESET || errno == ECONNABORTED)));
    close(fd);
}
static void socket_tests(void)
{
    unsigned port = launch(400, 4);
    struct hs_supervisor_config occupied = HS_SUPERVISOR_CONFIG_INIT;
    occupied.port = port;
    CHECK(hashstat_supervisor_run(&occupied) == 1);
    const char *commands[] = {"version", "summary", "devs", "pools", "stats",
                              "{\"command\":\"check\",\"parameter\":\"start\"}"};
    for (unsigned i = 0; i < sizeof(commands)/sizeof(commands[0]); ++i) {
        json_t *root = request(port, commands[i]); CHECK(!error_response(root));
        if (i == 2) CHECK(json_array_size(json_object_get(root, "DEVS")) == 0);
        if (i == 3) CHECK(json_array_size(json_object_get(root, "POOLS")) == 0);
        if (i == 5) CHECK(json_is_false(json_object_get(json_array_get(json_object_get(root, "CHECK"), 0), "Access")));
        json_decref(root);
    }
    int fragmented = connect_local(port);
    send_bytes(fragmented, "{\"command\":", 11);
    CHECK(wait_fd(fragmented, POLLIN, 40) == 0);
    send_bytes(fragmented, "\"status\"}", 9);
    json_t *root = receive(fragmented); CHECK(!error_response(root)); json_decref(root);
    int slow = connect_local(port);
    uint64_t since = now_ms(); send_bytes(slow, "{", 1); pause_ms(100);
    root = request(port, "status"); CHECK(!error_response(root)); json_decref(root);
    send_bytes(slow, "\"command\":", 10); pause_ms(100); send_bytes(slow, "\"sta", 4);
    closed(slow); CHECK(now_ms()-since < 1100);
    char over[HS_SUPERVISOR_MAX_REQUEST+1]; memset(over, 'x', sizeof(over));
    int large = connect_local(port); send_bytes(large, over, sizeof(over));
    root = receive(large); CHECK(error_response(root)); json_decref(root);
    int abandoned = connect_local(port); send_bytes(abandoned, "summary", 7); close(abandoned);
    root = request(port, "stop"); CHECK(!error_response(root)); json_decref(root);
    root = request(port, "stop"); CHECK(!error_response(root)); json_decref(root);
    root = request(port, "resume"); CHECK(error_response(root)); json_decref(root);
    root = request(port, "quit"); CHECK(!error_response(root)); json_decref(root); finish(0);

    port = launch(300, 2);
    int first = connect_local(port), second = connect_local(port);
    send_bytes(first, "{", 1); send_bytes(second, "{", 1);
    CHECK(wait_fd(first, POLLIN, 30) == 0);
    int excess = connect_local(port); closed(excess);
    closed(first); closed(second);
    root = request(port, "status"); CHECK(!error_response(root)); json_decref(root);
    CHECK(kill(owned_child, SIGTERM) == 0); finish(0);

    port = launch(1000, 1);
    root = request(port, "recover"); CHECK(!error_response(root)); json_decref(root);
    finish(HS_SUPERVISOR_EXIT_RECOVERY);

    port = launch(1000, 4);
    int interrupted = connect_local(port); send_bytes(interrupted, "{", 1);
    CHECK(kill(owned_child, SIGINT) == 0); finish(0); close(interrupted);
}
int main(void)
{
    CHECK(atexit(cleanup) == 0);
    signal(SIGPIPE, SIG_IGN);
    pure_tests(); socket_tests();
    printf("hashstat supervisor: %u checks; synthetic localhost only; no hardware\n", checks);
    return 0;
}
