/* SPDX-License-Identifier: GPL-3.0-only */

#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "hs_hashminer_runtime.h"
#include "hs_aml_readonly.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

static bool valid(const struct hs_hashminer_runtime *r) { return r && r->self == r; }
static bool now_ms(uint64_t *out)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) || ts.tv_sec < 0 || ts.tv_nsec < 0 ||
        ts.tv_nsec >= 1000000000L || (uint64_t)ts.tv_sec > (UINT64_MAX - 999U) / 1000U)
        return false;
    *out = (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
    return true;
}
static void fail(struct hs_hashminer_runtime *r, enum hs_hashminer_runtime_status status)
{
    if (!r->fault_latched) r->status = status;
    r->fault_latched = true;
    r->stop_requested = true;
    r->observation_valid = false;
}
static void close_pipe(struct hs_hashminer_runtime *r)
{
    if (r->read_fd >= 0) {
        int fd = r->read_fd;
        r->read_fd = -1;

        if (close(fd)) { r->descriptor_cleanup_unknown = true; fail(r, HS_HR_SYSTEM_ERROR); }
    }
}
static bool descriptor_flags(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 &&
        fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}
static void encode(const struct hs_aml_observation *o, uint64_t generation,
                   unsigned char wire[HS_HASHMINER_REPORT_BYTES])
{
    memset(wire, 0, HS_HASHMINER_REPORT_BYTES);
    memcpy(wire, "HRO1", 4);
    for (unsigned n = 0; n < 8; ++n) wire[4 + n] = (unsigned char)(generation >> (8U * n));
    wire[12] = (unsigned char)o->bound_profile;
    wire[13] = (unsigned char)o->read_calls;
    for (unsigned n = 0; n < 3; ++n) {
        unsigned offset = 16U + 4U * n;
        wire[offset] = (unsigned char)o->chains[n].gpio;
        wire[offset + 1] = (unsigned char)(o->chains[n].gpio >> 8U);
        wire[offset + 2] = (unsigned char)o->chains[n].plug;
        wire[offset + 3] = (unsigned char)o->chains[n].reason;
    }
}
static bool decode(struct hs_hashminer_runtime *r)
{
    const unsigned char *w = r->report;
    struct hs_aml_observation o = {0};
    uint64_t generation = 0;
    if (r->received != HS_HASHMINER_REPORT_BYTES || memcmp(w, "HRO1", 4) ||
        w[12] > 1 || w[14] || w[15] ||
        w[12] != (unsigned char)r->policy.exact_aml_gpio_binding ||
        w[13] != (w[12] ? 18 : 0)) return false;
    for (unsigned n = 0; n < 8; ++n) generation |= (uint64_t)w[4 + n] << (8U * n);
    if (!generation || generation != r->generation) return false;
    o.bound_profile = w[12] != 0; o.read_calls = w[13];
    for (unsigned n = 0; n < 3; ++n) {
        unsigned offset = 16U + 4U * n;
        o.chains[n].gpio = (uint32_t)w[offset] | ((uint32_t)w[offset + 1] << 8U);
        if (o.chains[n].gpio != 439U + n || w[offset + 2] > HS_AML_PRESENCE_PRESENT ||
            w[offset + 3] > HS_AML_OBSERVATION_VALID_PLUG_LEVEL) return false;
        o.chains[n].plug = (enum hs_aml_presence_state)w[offset + 2];
        o.chains[n].reason = (enum hs_aml_observation_reason)w[offset + 3];
        if ((o.chains[n].reason == HS_AML_OBSERVATION_VALID_PLUG_LEVEL) !=
            (o.chains[n].plug != HS_AML_PRESENCE_UNKNOWN)) return false;
        if (!o.bound_profile && o.chains[n].reason != HS_AML_OBSERVATION_NOT_BOUND) return false;
    }
    r->observation = o;
    return true;
}
static _Noreturn void child_observe(struct hs_hashminer_runtime *r, int out_fd, pid_t parent)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa)); sa.sa_handler = SIG_DFL; sigemptyset(&sa.sa_mask);
    const int reset[] = {SIGTERM, SIGINT, SIGHUP, SIGPIPE};
    for (unsigned n = 0; n < sizeof(reset) / sizeof(reset[0]); ++n)
        if (sigaction(reset[n], &sa, NULL)) _exit(70);
    sigset_t empty; sigemptyset(&empty);
    if (sigprocmask(SIG_SETMASK, &empty, NULL)) _exit(70);
#ifdef __linux__
    if (prctl(PR_SET_PDEATHSIG, (unsigned long)SIGKILL, 0UL, 0UL, 0UL)) _exit(70);
#endif
    if (getppid() != parent) _exit(70);
    struct hs_aml_observation o = hs_aml_observe_presence(r->policy.exact_aml_gpio_binding,
                                                        r->reader, r->reader_context);
    unsigned char wire[HS_HASHMINER_REPORT_BYTES];
    encode(&o, r->generation, wire);
    for (unsigned attempt = 0; attempt < 9; ++attempt) {
        ssize_t count = write(out_fd, wire, sizeof(wire));
        if (count == (ssize_t)sizeof(wire)) _exit(0);
        if (count < 0 && errno == EINTR) continue;
        _exit(74);
    }
    _exit(74);
}

enum hs_hashminer_runtime_status hs_hashminer_runtime_init(
    struct hs_hashminer_runtime *r, const struct hs_hashminer_runtime_policy *p,
    hs_aml_attribute_reader reader, void *context)
{
    struct sigaction action;
    if (!r || r->self || !p || !p->isolated_single_thread_entry ||
        !p->observation_timeout_ms || p->observation_timeout_ms > 10000 ||
        !p->cancel_grace_ms || p->cancel_grace_ms > 10000 ||
        !p->reap_grace_ms || p->reap_grace_ms > 10000) return HS_HR_ARGUMENT;

    if (sigaction(SIGCHLD, NULL, &action) || action.sa_handler != SIG_DFL ||
        (action.sa_flags & SA_NOCLDWAIT) != 0) return HS_HR_SYSTEM_ERROR;
    const struct hs_hashminer_runtime_policy saved_policy = *p;
    memset(r, 0, sizeof(*r));
    r->self = r; r->policy = saved_policy; r->read_fd = -1;
    r->reader = reader ? reader : hs_aml_readonly_attribute;
    r->reader_context = context; r->process_quiescent = true;
    r->work_gate_closed = true;
    return HS_HR_OK;
}
enum hs_hashminer_runtime_status hs_hashminer_runtime_observe(struct hs_hashminer_runtime *r)
{
    uint64_t now;
    int pair[2];
    if (!valid(r)) return HS_HR_ARGUMENT;
    if (!r->process_quiescent || r->child || r->read_fd >= 0) return HS_HR_BUSY;
    if (r->fault_latched || r->stop_requested) return r->fault_latched ? r->status : HS_HR_CANCELLED;
    if (r->generation == UINT64_MAX) { fail(r, HS_HR_EXHAUSTED); return r->status; }
    if (!now_ms(&now) || (r->clock_started && now < r->last_now_ms)) {
        fail(r, HS_HR_SYSTEM_ERROR); return r->status;
    }
    if (pipe(pair)) { fail(r, HS_HR_SYSTEM_ERROR); return r->status; }
    if (!descriptor_flags(pair[0]) || !descriptor_flags(pair[1])) {
        if (close(pair[0])) r->descriptor_cleanup_unknown = true;
        if (close(pair[1])) r->descriptor_cleanup_unknown = true;
        fail(r, HS_HR_SYSTEM_ERROR); return r->status;
    }
    ++r->generation;
    r->observation_valid = false; r->received = 0; r->eof = false;
    r->term_sent = false; r->kill_sent = false; r->wait_status = 0;
    r->began_ms = now; r->last_now_ms = now; r->clock_started = true;
    pid_t parent = getpid();
    pid_t child = fork();
    if (child == 0) {
        (void)close(pair[0]);
        child_observe(r, pair[1], parent);
    }
    if (close(pair[1])) r->descriptor_cleanup_unknown = true;
    if (child < 0) {
        if (close(pair[0])) r->descriptor_cleanup_unknown = true;
        fail(r, HS_HR_SYSTEM_ERROR); return r->status;
    }
    r->read_fd = pair[0]; r->child = child; r->process_quiescent = false;
    r->phase = HS_HR_OBSERVING; r->status = HS_HR_OK;
    if (r->descriptor_cleanup_unknown) { fail(r, HS_HR_SYSTEM_ERROR); return r->status; }
    return HS_HR_OK;
}
void hs_hashminer_runtime_cancel(struct hs_hashminer_runtime *r)
{
    if (!valid(r)) return;
    r->stop_requested = true; r->work_gate_closed = true;
    r->observation_valid = false;
    if (!r->fault_latched) r->status = HS_HR_CANCELLED;
}
static struct hs_hashminer_runtime_result snapshot(struct hs_hashminer_runtime *r)
{
    struct hs_hashminer_runtime_result out = {0};
    out.status = HS_HR_ARGUMENT; out.phase = HS_HR_FAULTED;
    if (!valid(r)) return out;
    out.status = r->status; out.phase = r->phase; out.observation = r->observation;
    out.observation_generation = r->generation; out.owned_pid = r->child;
    out.observation_valid = r->observation_valid;
    out.process_quiescent = r->process_quiescent && r->read_fd < 0 && !r->descriptor_cleanup_unknown;
    out.work_gate_closed = r->work_gate_closed;
    out.term_sent = r->term_sent; out.kill_sent = r->kill_sent;
    out.descriptor_cleanup_unknown = r->descriptor_cleanup_unknown;
    out.child_wait_status = r->wait_status;

    return out;
}
struct hs_hashminer_runtime_result hs_hashminer_runtime_poll(struct hs_hashminer_runtime *r)
{
    uint64_t now = 0;
    bool may_signal = false;
    if (!valid(r)) return snapshot(NULL);
    bool clock_ok = now_ms(&now) && (!r->clock_started || now >= r->last_now_ms);
    if (!clock_ok) { fail(r, HS_HR_SYSTEM_ERROR); now = r->last_now_ms; }
    r->last_now_ms = now; r->clock_started = true;

    if ((r->child > 1 || r->read_fd >= 0) && !r->stop_requested &&
        now - r->began_ms >= r->policy.observation_timeout_ms)
        fail(r, HS_HR_TIMEOUT);
    if (r->read_fd >= 0) {
        for (unsigned attempt = 0; attempt < 2; ++attempt) {
            size_t room = sizeof(r->report) - r->received;
            if (!room) { fail(r, HS_HR_PROTOCOL_ERROR); break; }
            ssize_t got = read(r->read_fd, r->report + r->received, room);
            if (got == 0) { r->eof = true; break; }
            if (got < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                    fail(r, HS_HR_SYSTEM_ERROR);
                break;
            }
            r->received += (size_t)got;
            if (r->received > HS_HASHMINER_REPORT_BYTES) { fail(r, HS_HR_PROTOCOL_ERROR); break; }
        }
    }
    if (r->child > 1) {
        int status = 0;
        pid_t got = waitpid(r->child, &status, WNOHANG);
        if (got == r->child) {
            r->child = 0; r->wait_status = status; r->process_quiescent = true;
            if (!r->stop_requested && (!WIFEXITED(status) || WEXITSTATUS(status) != 0))
                fail(r, HS_HR_PROTOCOL_ERROR);
        } else if (got == 0) {
            may_signal = true;
        } else if (got < 0 && errno == ECHILD) {
            r->child = 0; r->process_quiescent = false;
            fail(r, HS_HR_REAP_UNKNOWN);
        } else if (got < 0 && errno != EINTR) {
            fail(r, HS_HR_SYSTEM_ERROR);
        }
    }
    if (r->child > 1 && r->stop_requested && may_signal) {
        if (!r->term_sent) {
            r->term_sent = true; r->cancel_ms = now; r->phase = HS_HR_CANCELLING;
            if (kill(r->child, SIGTERM) && errno != ESRCH) fail(r, HS_HR_SYSTEM_ERROR);
        }
        if (!r->kill_sent && (!clock_ok || now - r->cancel_ms >= r->policy.cancel_grace_ms)) {
            r->kill_sent = true; r->kill_ms = now; r->phase = HS_HR_KILLING;
            if (kill(r->child, SIGKILL) && errno != ESRCH) fail(r, HS_HR_SYSTEM_ERROR);
        }
        if (r->kill_sent && now - r->kill_ms >= r->policy.reap_grace_ms) {
            fail(r, HS_HR_REAP_UNKNOWN); r->phase = HS_HR_FAULTED;
        }
    }
    if (r->process_quiescent && r->child == 0) {
        if (r->stop_requested) {
            close_pipe(r);
            if (!hs_hashminer_runtime_close_bridge(r)) fail(r, HS_HR_BRIDGE_ERROR);
            r->phase = r->fault_latched ? HS_HR_FAULTED : HS_HR_STOPPED;
        } else if (r->read_fd >= 0 && r->eof) {
            if (!decode(r)) fail(r, HS_HR_PROTOCOL_ERROR);
            else {
                r->observation_valid = true;
                r->status = HS_HR_NO_HASHBOARDS;
                for (unsigned n = 0; n < 3; ++n)
                    if (r->observation.chains[n].plug != HS_AML_PRESENCE_ABSENT)
                        r->status = HS_HR_UNQUALIFIED;
            }
            close_pipe(r);
            r->phase = r->fault_latched ? HS_HR_FAULTED : HS_HR_FINISHED;
        }
    } else if (r->child == 0 && r->fault_latched) {
        close_pipe(r);
        if (!hs_hashminer_runtime_close_bridge(r)) fail(r, HS_HR_BRIDGE_ERROR);
        r->phase = HS_HR_FAULTED;
    }
    return snapshot(r);
}
