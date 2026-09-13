/* SPDX-License-Identifier: GPL-3.0-only
 * Original HashStat restricted no-HAL management mode. Not a mining driver.
 */
#define _POSIX_C_SOURCE 200809L
#include "hashstat-supervisor.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <jansson.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum command { C_BAD = 0, C_STATUS, C_VERSION, C_SUMMARY, C_DEVS, C_POOLS,
    C_STATS, C_CHECK, C_STOP, C_QUIT, C_SHUTDOWN, C_RECOVER, C_RESTART,
    C_START, C_RESUME };
static const char *const command_names[] = { "", "status", "version", "summary",
    "devs", "pools", "stats", "check", "stop", "quit", "shutdown", "recover",
    "restart", "start", "resume" };
static enum command command_id(const char *s)
{
    for (unsigned i = 1; i < sizeof(command_names)/sizeof(command_names[0]); ++i)
        if (!strcmp(s, command_names[i])) return (enum command)i;
    return C_BAD;
}
static bool space(unsigned char c)
{ return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static const char *phase_name(enum hs_supervisor_phase phase)
{
    static const char *const names[] = { "WAITING_HARDWARE", "STOPPED",
                                        "SHUTDOWN", "RECOVERY_REQUESTED" };
    return names[phase];
}
static bool bounded_text(json_t *value, size_t maximum)
{
    if (!json_is_string(value)) return false;
    const char *s = json_string_value(value);
    size_t n = json_string_length(value);
    if (n == 0 || n > maximum || strlen(s) != n) return false;
    for (size_t i = 0; i < n; ++i)
        if ((unsigned char)s[i] < 32 || (unsigned char)s[i] >= 127) return false;
    return true;
}

static int frame(const char *s, size_t n)
{
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0) return -1;
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '{' || c == '[') {
            if (++depth > 32) return -1;
        } else if (c == '}' || c == ']') {
            if (depth == 0) return -1;
            if (--depth == 0) return i+1 == n ? 1 : -1;
        }
    }
    return 0;
}

enum hs_supervisor_response hashstat_supervisor_handle(
    struct hs_supervisor_state *state, const char *request, size_t length,
    uint64_t elapsed_ms, char *output, size_t capacity, size_t *written,
    enum hs_supervisor_action *action)
{
    if (!state || !request || !output || !written || !action || !capacity ||
        (unsigned)state->phase > HS_SUPERVISOR_RECOVERY) return HS_SUPERVISOR_ERROR;
    *written = 0; *action = HS_SUPERVISOR_CONTINUE;
    const char *message = "Restricted standby management; hardware is not initialized";
    enum command command = C_BAD;
    bool valid = length <= HS_SUPERVISOR_MAX_REQUEST;
    bool exists = false, access = false;
    json_t *root = NULL;
    if (valid) {
        while (length && space((unsigned char)request[length-1])) --length;
        if (length && request[length-1] == '\0') --length;
        while (length && space((unsigned char)request[length-1])) --length;
        while (length && space((unsigned char)*request)) { ++request; --length; }
        if (!length) return HS_SUPERVISOR_NEED_MORE;
        if (*request == '{') {
            int framed = frame(request, length);
            if (!framed) return HS_SUPERVISOR_NEED_MORE;
            if (framed < 0) valid = false;
            else {
                json_error_t error;
                root = json_loadb(request, length, JSON_REJECT_DUPLICATES, &error);
                valid = json_is_object(root);
                if (valid) {
                    const char *key; json_t *value;
                    json_object_foreach(root, key, value)
                        if (strcmp(key, "command") && strcmp(key, "parameter")) valid = false;
                    json_t *name = json_object_get(root, "command");
                    valid = valid && bounded_text(name, 32);
                    if (valid) command = command_id(json_string_value(name));
                    json_t *parameter = json_object_get(root, "parameter");
                    if (command == C_CHECK) {
                        valid = valid && bounded_text(parameter, 32);
                        if (valid) {
                            enum command checked = command_id(json_string_value(parameter));
                            exists = checked != C_BAD;
                            access = exists && checked != C_START && checked != C_RESUME;
                        }
                    } else if (parameter) valid = false;
                }
            }
        } else {
            bool prefix = false;
            for (unsigned i = 1; i < sizeof(command_names)/sizeof(command_names[0]); ++i) {
                size_t n = strlen(command_names[i]);
                if (length <= n && !memcmp(request, command_names[i], length)) {
                    prefix = true;
                    if (length == n) command = (enum command)i;
                }
            }
            if (prefix && command == C_BAD) return HS_SUPERVISOR_NEED_MORE;
            valid = command != C_BAD && command != C_CHECK;
        }
    }
    if (root) json_decref(root);
    valid = valid && command != C_BAD;
    enum hs_supervisor_phase next = state->phase;
    enum hs_supervisor_action next_action = HS_SUPERVISOR_CONTINUE;
    if (!valid) message = "Invalid or unsupported bounded standby request";
    else if (command == C_START || command == C_RESUME) {
        valid = false; message = "Hardware lifecycle unavailable; start and resume are unsupported";
    } else if (command == C_STOP) {
        if (next >= HS_SUPERVISOR_SHUTDOWN) {
            valid = false; message = "Shutdown already requested; state cannot be reopened";
        } else {
            next = HS_SUPERVISOR_STOPPED;
            message = "Stop recorded; work remains disabled; physical power state is unknown";
        }
    } else if (command == C_QUIT || command == C_SHUTDOWN ||
               command == C_RECOVER || command == C_RESTART) {
        bool recovery = command == C_RECOVER || command == C_RESTART;
        enum hs_supervisor_phase requested = recovery ? HS_SUPERVISOR_RECOVERY : HS_SUPERVISOR_SHUTDOWN;
        if (next >= HS_SUPERVISOR_SHUTDOWN && next != requested) {
            valid = false; message = "A different terminal request is already pending";
        } else {
            next = requested;
            next_action = recovery ? HS_SUPERVISOR_RECOVER : HS_SUPERVISOR_QUIT;
            message = recovery ? "Recovery exit requested; outer owner must decide recovery" : "Clean shutdown requested";
        }
    }
    char section[1024] = "";
    unsigned long long seconds = (unsigned long long)(elapsed_ms/1000);
    if (valid) switch (command) {
    case C_VERSION:
        snprintf(section, sizeof(section), "\"VERSION\":[{\"HashStat\":\"" HS_SUPERVISOR_VERSION "\",\"API\":\"restricted-standby-v1\"}],"); break;
    case C_SUMMARY:
        snprintf(section, sizeof(section), "\"SUMMARY\":[{\"Elapsed\":%llu,\"MHS av\":0.0,\"MHS 5s\":0.0,\"Accepted\":0,\"Rejected\":0,\"Hardware Errors\":0,\"Total MH\":0.0}],", seconds); break;
    case C_DEVS: strcpy(section, "\"DEVS\":[],"); break;
    case C_POOLS: strcpy(section, "\"POOLS\":[],"); break;
    case C_STATS:
        snprintf(section, sizeof(section), "\"STATS\":[{\"ID\":\"HashStatStandby\",\"Elapsed\":%llu}],", seconds); break;
    case C_CHECK:
        snprintf(section, sizeof(section), "\"CHECK\":[{\"Exists\":%s,\"Access\":%s}],", exists ? "true" : "false", access ? "true" : "false"); break;
    default: break;
    }
    int n = snprintf(output, capacity,
        "{\"STATUS\":[{\"STATUS\":\"%s\",\"Code\":%d,\"Msg\":\"%s\",\"Description\":\"HashStat restricted standby API\"}],%s"
        "\"HASHSTAT\":[{\"Version\":\"" HS_SUPERVISOR_VERSION "\",\"State\":\"%s\",\"HardwareInitialized\":false,\"MiningEnabled\":false,"
        "\"ASICCount\":null,\"FanCount\":null,\"PowerState\":\"unknown\",\"PoolConnections\":0,\"FullCgminerAPICompatible\":false}],\"id\":1}",
        valid ? "S" : "E", valid ? 0 : 1, message, section, phase_name(next));
    if (n < 0 || (size_t)n >= capacity) return HS_SUPERVISOR_ERROR;
    state->phase = next; *action = next_action; *written = (size_t)n+1;
    return HS_SUPERVISOR_RESPONSE;
}

struct client {
    int fd;
    uint64_t since;
    size_t received, written, sent;
    char request[HS_SUPERVISOR_MAX_REQUEST+1];
    char response[HS_SUPERVISOR_MAX_RESPONSE];
};
static volatile sig_atomic_t interrupted;
static void signal_stop(int signo) { (void)signo; interrupted = 1; }
static bool milliseconds(uint64_t *value)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) || now.tv_sec < 0 || now.tv_nsec < 0 ||
        now.tv_nsec >= 1000000000L || (uint64_t)now.tv_sec > UINT64_MAX/1000) return false;
    uint64_t base = (uint64_t)now.tv_sec*1000, fraction = (uint64_t)now.tv_nsec/1000000;
    if (fraction > UINT64_MAX-base) return false;
    *value = base+fraction;
    return true;
}
static bool nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return false;
    flags = fcntl(fd, F_GETFD);
    return flags >= 0 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}
static void close_owned(int *fd)
{
    if (*fd >= 0) { int owned = *fd; *fd = -1; (void)close(owned); }
}
int hashstat_supervisor_run(const struct hs_supervisor_config *config)
{
    if (!config || config->port > 65535 || config->timeout_ms == 0 ||
        config->timeout_ms > 10000 || config->max_clients == 0 ||
        config->max_clients > HS_SUPERVISOR_MAX_CLIENTS) return 1;
    struct client clients[HS_SUPERVISOR_MAX_CLIENTS];
    memset(clients, 0, sizeof(clients));
    for (unsigned i = 0; i < HS_SUPERVISOR_MAX_CLIENTS; ++i) clients[i].fd = -1;
    int listener = -1, result = 1, exiting_client = -1;
    enum hs_supervisor_action exiting = HS_SUPERVISOR_CONTINUE;
    struct hs_supervisor_state state = HS_SUPERVISOR_STATE_INIT;
    struct sigaction prior_term, prior_int, prior_pipe, handler;
    bool term_set = false, int_set = false, pipe_set = false;
    uint64_t started, now, previous;
    if (!milliseconds(&started)) return 1;
    previous = started; interrupted = 0;
    memset(&handler, 0, sizeof(handler)); sigemptyset(&handler.sa_mask);
    handler.sa_handler = signal_stop;
    if (sigaction(SIGTERM, &handler, &prior_term)) goto done;
    term_set = true;
    if (sigaction(SIGINT, &handler, &prior_int)) goto done;
    int_set = true;
    handler.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &handler, &prior_pipe)) goto done;
    pipe_set = true;
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0 || !nonblocking(listener)) goto done;
    int reuse = 1;
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse))) goto done;
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address)); address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(UINT32_C(0x7f000001)); address.sin_port = htons((uint16_t)config->port);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) ||
        listen(listener, (int)config->max_clients)) goto done;
    socklen_t address_size = sizeof(address);
    if (getsockname(listener, (struct sockaddr *)&address, &address_size)) goto done;
    fprintf(stderr, "HashStat " HS_SUPERVISOR_VERSION " WAITING_HARDWARE; restricted management on 127.0.0.1:%u; no hardware initialized\n",
            (unsigned)ntohs(address.sin_port));
    if (config->listening) config->listening(config->context, ntohs(address.sin_port));
    for (;;) {
        if (interrupted) { result = 0; break; }
        if (!milliseconds(&now) || now < previous) break;
        previous = now;
        struct pollfd watched[HS_SUPERVISOR_MAX_CLIENTS+1];
        watched[0] = (struct pollfd){listener, POLLIN, 0};
        int timeout = 100;
        for (unsigned i = 0; i < config->max_clients; ++i) {
            struct client *c = &clients[i];
            if (c->fd >= 0 && now-c->since >= config->timeout_ms) close_owned(&c->fd);
            if (c->fd >= 0) {
                uint64_t left = config->timeout_ms-(now-c->since);
                if (left < (uint64_t)timeout) timeout = (int)left;
            }
            watched[i+1] = (struct pollfd){c->fd, c->written ? POLLOUT : POLLIN, 0};
        }
        if (exiting != HS_SUPERVISOR_CONTINUE && clients[exiting_client].fd < 0) {
            result = exiting == HS_SUPERVISOR_RECOVER ? HS_SUPERVISOR_EXIT_RECOVERY : 0; break;
        }
        int ready = poll(watched, config->max_clients+1, timeout);
        if (ready < 0) { if (errno == EINTR) continue; break; }
        if (interrupted) { result = 0; break; }
        if (!milliseconds(&now) || now < previous) break;
        previous = now;
        if (watched[0].revents & (POLLERR | POLLHUP | POLLNVAL)) break;

        if (watched[0].revents & POLLIN) for (unsigned n = 0; n < config->max_clients; ++n) {
            int fd = accept(listener, NULL, NULL);
            if (fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
                goto done;
            }
            unsigned slot = 0;
            while (slot < config->max_clients && clients[slot].fd >= 0) ++slot;
            if (slot == config->max_clients || !nonblocking(fd)) { close_owned(&fd); continue; }
            memset(&clients[slot], 0, sizeof(clients[slot]));
            clients[slot].fd = fd; clients[slot].since = now;

            watched[slot+1].revents = 0;
        }
        for (unsigned i = 0; i < config->max_clients; ++i) {
            struct client *c = &clients[i];
            short events = watched[i+1].revents;
            if (c->fd < 0) continue;
            if (!milliseconds(&now) || now < previous) goto done;
            previous = now;
            if (now-c->since >= config->timeout_ms) { close_owned(&c->fd); continue; }
            if (events & (POLLERR | POLLNVAL)) { close_owned(&c->fd); continue; }
            if (!c->written && (events & (POLLIN | POLLHUP))) {
                ssize_t got = recv(c->fd, c->request+c->received, sizeof(c->request)-c->received, 0);
                if (got <= 0) {
                    if (got == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) close_owned(&c->fd);
                    continue;
                }
                c->received += (size_t)got;
                if (!milliseconds(&now) || now < previous) goto done;
                previous = now;
                if (now-c->since >= config->timeout_ms) { close_owned(&c->fd); continue; }
                enum hs_supervisor_action action;
                enum hs_supervisor_response reply = hashstat_supervisor_handle(&state,
                    c->request, c->received, now-started, c->response, sizeof(c->response), &c->written, &action);
                if (reply == HS_SUPERVISOR_ERROR) goto done;
                if (reply == HS_SUPERVISOR_NEED_MORE && c->received >= HS_SUPERVISOR_MAX_REQUEST) close_owned(&c->fd);
                if (action != HS_SUPERVISOR_CONTINUE) {
                    exiting = action; exiting_client = (int)i; close_owned(&listener);
                    for (unsigned other = 0; other < config->max_clients; ++other)
                        if (other != i) close_owned(&clients[other].fd);
                    break;
                }
            }
            if (c->written && (events & POLLOUT)) {
                ssize_t sent = send(c->fd, c->response+c->sent, c->written-c->sent, 0);
                if (sent > 0) {
                    c->sent += (size_t)sent;
                    if (c->sent == c->written) close_owned(&c->fd);
                } else if (sent == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) close_owned(&c->fd);
            }
        }
    }
done:
    close_owned(&listener);
    for (unsigned i = 0; i < HS_SUPERVISOR_MAX_CLIENTS; ++i) close_owned(&clients[i].fd);
    if (pipe_set) (void)sigaction(SIGPIPE, &prior_pipe, NULL);
    if (int_set) (void)sigaction(SIGINT, &prior_int, NULL);
    if (term_set) (void)sigaction(SIGTERM, &prior_term, NULL);
    return result;
}
static bool number(const char *s, unsigned maximum, unsigned *value)
{
    unsigned result = 0;
    if (!s || !*s) return false;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9') return false;
        unsigned digit = (unsigned)(*s-'0');
        if (digit > maximum || result > (maximum-digit)/10) return false;
        result = result*10+digit;
        if (result > maximum) return false;
    }
    if (!result) return false;
    *value = result; return true;
}
int hashstat_supervisor_main(int argc, char **argv)
{
    struct hs_supervisor_config config = HS_SUPERVISOR_CONFIG_INIT;
    unsigned seen = 0;
    if (argc < 2 || !argv || !argv[1] || strcmp(argv[1], "--hashstat-standby")) return 2;
    for (int i = 2; i < argc; i += 2) {
        unsigned bit, maximum, *destination;
        if (i+1 >= argc || !argv[i]) return 2;
        if (!strcmp(argv[i], "--hashstat-standby-port")) { bit = 1; maximum = 65535; destination = &config.port; }
        else if (!strcmp(argv[i], "--hashstat-standby-timeout-ms")) { bit = 2; maximum = 10000; destination = &config.timeout_ms; }
        else if (!strcmp(argv[i], "--hashstat-standby-max-clients")) { bit = 4; maximum = 4; destination = &config.max_clients; }
        else return 2;
        if ((seen & bit) || !number(argv[i+1], maximum, destination)) return 2;
        seen |= bit;
    }
    return hashstat_supervisor_run(&config);
}
