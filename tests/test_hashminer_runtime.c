/* SPDX-License-Identifier: GPL-3.0-only */

#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "hs_hashminer_runtime.h"
#include "hashstat-aml88-bridge.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef NDEBUG
#error "Runtime ownership tests must not define NDEBUG"
#endif

static unsigned long checks;
#define CHECK(c) do { ++checks; if (!(c)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); \
} } while (0)
enum fixture_mode { ABSENT, PRESENT, MIXED, BAD_DIRECTION, BAD_POLARITY, CHANGED,
                    UNAVAILABLE, CHILD_EMPTY, CHILD_ERROR, CHILD_SIGNAL, HANG, IGNORE_TERM };
struct fixture { enum fixture_mode mode; unsigned values[3]; int ready_fd; };
static uint64_t clock_ms(void)
{
    struct timespec ts;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}
static struct hs_aml_text text(const char *s)
{
    struct hs_aml_text t = {0};
    t.status = HS_AML_TEXT_OK; t.length = strlen(s);
    CHECK(t.length <= sizeof(t.bytes)); memcpy(t.bytes, s, t.length);
    return t;
}
static struct hs_aml_text reader(void *opaque, unsigned chain, enum hs_aml_attribute attr)
{
    struct fixture *f = opaque;
    CHECK(chain < 3 && (unsigned)attr < 3);
    if (f->mode == CHILD_EMPTY) _exit(0);
    if (f->mode == CHILD_ERROR) _exit(9);
    if (f->mode == CHILD_SIGNAL) { (void)kill(getpid(), SIGTERM); _exit(8); }
    if (f->mode == HANG || f->mode == IGNORE_TERM) {
        if (f->mode == IGNORE_TERM) CHECK(signal(SIGTERM, SIG_IGN) != SIG_ERR);
        if (f->ready_fd >= 0) CHECK(write(f->ready_fd, "R", 1) == 1);
        for (;;) (void)pause();
    }
    if (f->mode == UNAVAILABLE) { struct hs_aml_text t = {0}; return t; }
    if (attr == HS_AML_ATTRIBUTE_DIRECTION) return text(f->mode == BAD_DIRECTION ? "out" : "in\n");
    if (attr == HS_AML_ATTRIBUTE_ACTIVE_LOW) return text(f->mode == BAD_POLARITY ? "1\n" : "0\n");
    if (f->mode == CHANGED) return text((f->values[chain]++ & 1U) ? "0\n" : "1\n");
    return text(f->mode == PRESENT || (f->mode == MIXED && chain == 0) ? "1\n" : "0\n");
}
static const struct hs_hashminer_runtime_policy policy = {500,10,300,true,true};
static void setup(struct hs_hashminer_runtime *r, struct fixture *f, enum fixture_mode mode, bool bound)
{
    struct hs_hashminer_runtime_policy p = policy; p.exact_aml_gpio_binding = bound;
    memset(r, 0, sizeof(*r)); memset(f, 0, sizeof(*f)); f->mode = mode; f->ready_fd = -1;
    CHECK(hs_hashminer_runtime_init(r, &p, reader, f) == HS_HR_OK);
    CHECK(r->read_fd == -1 && !r->child && r->process_quiescent && r->work_gate_closed);
}
static struct hs_hashminer_runtime_result finish(struct hs_hashminer_runtime *r)
{
    uint64_t began = clock_ms();
    for (;;) {
        struct hs_hashminer_runtime_result out = hs_hashminer_runtime_poll(r);
        CHECK(!out.mining_ready && !out.physical_power_off_confirmed && out.work_gate_closed);
        if ((out.phase == HS_HR_FINISHED || out.phase == HS_HR_STOPPED || out.phase == HS_HR_FAULTED) &&
            (out.process_quiescent || out.status == HS_HR_REAP_UNKNOWN)) return out;
        CHECK(clock_ms() - began < 3000);
        (void)poll(NULL, 0, 1);
    }
}
static void reaped(pid_t pid, int fd)
{
    int status;
    errno = 0; CHECK(waitpid(pid, &status, WNOHANG) == -1 && errno == ECHILD);
    errno = 0; CHECK(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
}
static void wait_fixture_ready(int fd)
{
    struct pollfd p = {fd, POLLIN, 0};
    CHECK(poll(&p, 1, 1000) == 1 && (p.revents & POLLIN));
    char byte = 0; CHECK(read(fd, &byte, 1) == 1 && byte == 'R');
}

static void observation_tests(void)
{
    for (unsigned mode = ABSENT; mode <= CHILD_SIGNAL; ++mode) {
        struct hs_hashminer_runtime r; struct fixture f;
        setup(&r, &f, (enum fixture_mode)mode, true);
        CHECK(hs_hashminer_runtime_observe(&r) == HS_HR_OK);
        pid_t pid = r.child; int fd = r.read_fd;
        CHECK(pid > 1 && fd >= 0 && !r.process_quiescent);
        CHECK(hs_hashminer_runtime_observe(&r) == HS_HR_BUSY);
        struct hs_hashminer_runtime_result out = finish(&r);
        CHECK(out.process_quiescent && !out.owned_pid);
        reaped(pid, fd);
        if (mode >= CHILD_EMPTY) {
            CHECK(out.status == HS_HR_PROTOCOL_ERROR && !out.observation_valid);
            CHECK(hs_hashminer_runtime_observe(&r) == HS_HR_PROTOCOL_ERROR);
        } else {
            CHECK(out.observation_valid && out.observation.read_calls == 18);
            CHECK(out.status == (mode == ABSENT ? HS_HR_NO_HASHBOARDS : HS_HR_UNQUALIFIED));
            for (unsigned chain = 0; chain < 3; ++chain) {
                CHECK(out.observation.chains[chain].gpio == 439U + chain);
                if (mode == ABSENT) CHECK(out.observation.chains[chain].plug == HS_AML_PRESENCE_ABSENT);
                if (mode == PRESENT) CHECK(out.observation.chains[chain].plug == HS_AML_PRESENCE_PRESENT);
                if (mode >= BAD_DIRECTION) CHECK(out.observation.chains[chain].plug == HS_AML_PRESENCE_UNKNOWN);
            }
        }

        CHECK(f.values[0] == 0 && f.values[1] == 0 && f.values[2] == 0);
    }
    struct hs_hashminer_runtime r; struct fixture f;
    setup(&r, &f, CHILD_ERROR, false);
    CHECK(hs_hashminer_runtime_observe(&r) == HS_HR_OK);
    struct hs_hashminer_runtime_result out = finish(&r);
    CHECK(out.status == HS_HR_UNQUALIFIED && out.observation_valid);
    CHECK(!out.observation.bound_profile && out.observation.read_calls == 0);
    CHECK(r.generation == 1);
    CHECK(hs_hashminer_runtime_observe(&r) == HS_HR_OK);
    out = finish(&r); CHECK(out.observation_generation == 2 && out.process_quiescent);
    hs_hashminer_runtime_cancel(&r); out = hs_hashminer_runtime_poll(&r);
    CHECK(out.phase == HS_HR_STOPPED && !out.observation_valid);
    CHECK(hs_hashminer_runtime_observe(&r) == HS_HR_CANCELLED);
}

static void cancellation_tests(void)
{
    {
        struct hs_hashminer_runtime r; struct fixture f;
        setup(&r, &f, ABSENT, true); r.policy.observation_timeout_ms = 10;
        CHECK(hs_hashminer_runtime_observe(&r) == HS_HR_OK);
        (void)poll(NULL,0,30);
        struct hs_hashminer_runtime_result out = finish(&r);
        CHECK(out.status == HS_HR_TIMEOUT && !out.observation_valid && out.process_quiescent);
    }
    for (unsigned mode = HANG; mode <= IGNORE_TERM; ++mode) {
        for (unsigned timeout = 0; timeout < 2; ++timeout) {
            struct hs_hashminer_runtime r; struct fixture f; int notify[2];
            setup(&r, &f, (enum fixture_mode)mode, true);
            r.policy.observation_timeout_ms = 30;
            CHECK(pipe(notify) == 0); f.ready_fd = notify[1];
            CHECK(hs_hashminer_runtime_observe(&r) == HS_HR_OK);
            pid_t pid = r.child; int fd = r.read_fd;
            wait_fixture_ready(notify[0]);
            CHECK(close(notify[0]) == 0 && close(notify[1]) == 0);
            if (!timeout) hs_hashminer_runtime_cancel(&r);
            struct hs_hashminer_runtime_result out = finish(&r);
            CHECK(out.status == (timeout ? HS_HR_TIMEOUT : HS_HR_CANCELLED));
            CHECK(out.process_quiescent && !out.observation_valid && out.term_sent);

            CHECK(mode != IGNORE_TERM || out.kill_sent);
            CHECK(WIFSIGNALED(out.child_wait_status));
            reaped(pid, fd);
            for (unsigned n = 0; n < 3; ++n) {
                out = hs_hashminer_runtime_poll(&r);
                CHECK(out.owned_pid == 0 && out.process_quiescent);
            }
        }
    }

    struct hs_hashminer_runtime r; struct fixture f;
    setup(&r, &f, ABSENT, true);
    CHECK(hs_hashminer_runtime_observe(&r) == HS_HR_OK);
    int status; pid_t child = r.child;
    CHECK(waitpid(child, &status, 0) == child);
    hs_hashminer_runtime_cancel(&r);
    struct hs_hashminer_runtime_result out = hs_hashminer_runtime_poll(&r);
    CHECK(out.status == HS_HR_REAP_UNKNOWN && !out.process_quiescent && out.owned_pid == 0);
    CHECK(!out.term_sent && !out.kill_sent && !out.observation_valid);
    CHECK(hs_hashminer_runtime_observe(&r) == HS_HR_BUSY);
}

static void malformed_ipc_tests(void)
{
    const unsigned char literal[28] = {
        'H','R','O','1', 1,0,0,0,0,0,0,0, 1,18,0,0,
        0xb7,1,1,6, 0xb8,1,1,6, 0xb9,1,1,6
    };
    for (unsigned variant = 0; variant < 42; ++variant) {
        struct hs_hashminer_runtime r; struct fixture f; int pair[2];
        setup(&r, &f, ABSENT, true);
        unsigned char wire[29]; memcpy(wire, literal, 28); wire[28] = 0;
        size_t length = 28;
        if (variant < 28) length = variant;
        if (variant == 28) length = 29;
        if (variant == 29) wire[0] = 'X';
        if (variant == 30) wire[4] = 2;
        if (variant == 31) wire[12] = 2;
        if (variant == 32) wire[13] = 17;
        if (variant == 33) wire[14] = 1;
        if (variant == 34) wire[15] = 1;
        if (variant == 35) wire[16]++;
        if (variant == 36) wire[18] = 3;
        if (variant == 37) wire[19] = 7;
        if (variant == 38) wire[19] = 1;
        if (variant == 39) wire[18] = 0;
        if (variant == 40) wire[12] = 0;
        CHECK(pipe(pair) == 0);
        CHECK(write(pair[1], wire, length) == (ssize_t)length);
        CHECK(close(pair[1]) == 0);
        r.read_fd = pair[0]; r.generation = 1;
        r.began_ms = clock_ms();
        struct hs_hashminer_runtime_result out = hs_hashminer_runtime_poll(&r);
        if (variant == 41) CHECK(out.status == HS_HR_NO_HASHBOARDS && out.observation_valid);
        else CHECK(out.status == HS_HR_PROTOCOL_ERROR && !out.observation_valid);
        CHECK(out.process_quiescent && !out.mining_ready && r.read_fd == -1);
    }
}

struct work { unsigned identifier; };
static unsigned released;
static struct work *clone(void *p, const struct work *w) { (void)p; (void)w; return NULL; }
static void discard(void *p, struct work *w) { (void)p; CHECK(w && w->identifier == 42); ++released; }
static bool export_work(void *p, struct work *w, struct hs_ab_export *e) { (void)p;(void)w;(void)e;return false; }
static bool current(void *p, struct work *w) { (void)p;(void)w;return false; }
static bool submit(void *p, const struct work *w, const struct hs_checked_share *s) { (void)p;(void)w;(void)s;return false; }
static const struct hs_ab_ops bridge_ops = {clone,discard,export_work,current,submit};
static const struct hs_miner_owner_policy owner_policy = {{1000,1000,10,20,0},500};
static void owner_bridge_tests(void)
{
    struct hs_hashminer_runtime r; struct fixture f;
    struct hs_miner_owner owner;
    struct hs_aml88_bridge bridge = {0};
    setup(&r, &f, ABSENT, true);

    CHECK(hs_miner_owner_init(&owner_policy, hs_hashminer_runtime_owner_ops(), &r,
        HS_MINER_OWNER_INITIAL_ALL, &owner) == HS_MINER_EXEC_OK);
    const struct hs_aml88_profile *profile = hs_aml88_profile_select("42801",5,"aml",3).profile;
    CHECK(profile != NULL);
    CHECK(hs_aml88_bridge_init(&bridge,profile,&owner.lifecycle,&bridge_ops,NULL) == HS_AB_OK);
    CHECK(hs_hashminer_runtime_bind_owner(&r,&owner,&bridge) == HS_HR_OK);
    struct work held = {42};
    bridge.chains[0].slots[0].work = &held;
    released = 0;
    CHECK(hs_hashminer_runtime_observe(&r) == HS_HR_OK);
    (void)finish(&r);
    struct hs_miner_owner_result life = hs_hashminer_runtime_owner_step(&r,HS_MINER_COMMAND_RUN,HS_MINER_FAULT_NONE);
    CHECK(life.phase == HS_MINER_WAITING_HARDWARE && !life.work_permitted);
    life = hs_hashminer_runtime_owner_step(&r,HS_MINER_COMMAND_STOP,HS_MINER_FAULT_NONE);
    CHECK(life.phase == HS_MINER_STOPPED && !life.work_permitted && released == 1);
    CHECK(bridge.chains[0].slots[0].work == NULL);
    (void)hs_hashminer_runtime_owner_step(&r,HS_MINER_COMMAND_STOP,HS_MINER_FAULT_NONE);
    CHECK(released == 1);
    bridge.cleanup_errors = 1;
    CHECK(!hs_hashminer_runtime_close_bridge(&r) && r.bridge_failed);

    int notify[2]; setup(&r, &f, IGNORE_TERM, true);
    CHECK(hs_miner_owner_init(&owner_policy, hs_hashminer_runtime_owner_ops(), &r,
        HS_MINER_OWNER_INITIAL_ALL, &owner) == HS_MINER_EXEC_OK);
    CHECK(hs_hashminer_runtime_bind_owner(&r,&owner,NULL) == HS_HR_OK);
    CHECK(pipe(notify) == 0); f.ready_fd = notify[1];
    CHECK(hs_hashminer_runtime_observe(&r) == HS_HR_OK);
    wait_fixture_ready(notify[0]); CHECK(close(notify[0]) == 0 && close(notify[1]) == 0);
    struct hs_miner_owner_input in = {0};
    in.now_ms = clock_ms(); in.command = HS_MINER_COMMAND_RUN; in.ready_flags = HS_MINER_READY_ALL;
    life = hs_miner_owner_step(&owner,&in);
    CHECK(life.latched_fault == HS_MINER_EXEC_OPERATION_REJECTED && !life.work_permitted);
    CHECK(owner.operations[HS_MINER_OP_START].state == HS_MINER_OP_RETIRED);
    CHECK(owner.operations[HS_MINER_OP_POWER_OFF].state == HS_MINER_OP_FAILED);
    uint64_t began = clock_ms();
    do {
        life = hs_hashminer_runtime_owner_step(&r,HS_MINER_COMMAND_NONE,HS_MINER_FAULT_NONE);
        CHECK(!life.work_permitted && !life.power_off_confirmed && !life.quiescent_confirmed);
        CHECK(clock_ms() - began < 2000);
        (void)poll(NULL,0,1);
    } while (!r.process_quiescent || r.acknowledgments[HS_MINER_OP_CANCEL_START].pending ||
             r.acknowledgments[HS_MINER_OP_DRAIN].pending);
    CHECK(r.child == 0 && r.read_fd == -1 && r.kill_sent);
    CHECK(owner.operations[HS_MINER_OP_CANCEL_START].state == HS_MINER_OP_SUCCEEDED);
    CHECK(owner.operations[HS_MINER_OP_DRAIN].state == HS_MINER_OP_SUCCEEDED);
    CHECK(!hs_hashminer_runtime_poll(&r).physical_power_off_confirmed);
}

static void argument_tests(void)
{
    struct hs_hashminer_runtime r = {0}; struct fixture f = {ABSENT,{0},-1};
    struct hs_hashminer_runtime_policy p = policy;
    CHECK(hs_hashminer_runtime_init(NULL,&p,reader,&f) == HS_HR_ARGUMENT);
    CHECK(hs_hashminer_runtime_init(&r,NULL,reader,&f) == HS_HR_ARGUMENT);
    p.isolated_single_thread_entry = false;
    CHECK(hs_hashminer_runtime_init(&r,&p,reader,&f) == HS_HR_ARGUMENT);
    p = policy; p.observation_timeout_ms = 0;
    CHECK(hs_hashminer_runtime_init(&r,&p,reader,&f) == HS_HR_ARGUMENT);
    p = policy; p.reap_grace_ms = 10001;
    CHECK(hs_hashminer_runtime_init(&r,&p,reader,&f) == HS_HR_ARGUMENT);
    CHECK(hs_hashminer_runtime_poll(NULL).status == HS_HR_ARGUMENT);
    CHECK(hs_hashminer_runtime_observe(NULL) == HS_HR_ARGUMENT);
    struct sigaction prior, ignored;
    memset(&ignored,0,sizeof(ignored)); ignored.sa_handler = SIG_IGN; sigemptyset(&ignored.sa_mask);
    CHECK(sigaction(SIGCHLD,&ignored,&prior) == 0);
    CHECK(hs_hashminer_runtime_init(&r,&policy,reader,&f) == HS_HR_SYSTEM_ERROR);
    CHECK(sigaction(SIGCHLD,&prior,NULL) == 0);
    setup(&r,&f,ABSENT,true);
    CHECK(hs_hashminer_runtime_init(&r,&policy,reader,&f) == HS_HR_ARGUMENT);
    struct hs_hashminer_runtime copy = r;
    CHECK(hs_hashminer_runtime_poll(&copy).status == HS_HR_ARGUMENT);
    CHECK(hs_hashminer_runtime_owner_step(&r,HS_MINER_COMMAND_RUN,HS_MINER_FAULT_NONE).status == HS_MINER_EXEC_INVALID_ARGUMENT);
    r.generation = UINT64_MAX;
    CHECK(hs_hashminer_runtime_observe(&r) == HS_HR_EXHAUSTED && r.child == 0);

    setup(&r,&f,ABSENT,true);
    int pair[2]; CHECK(pipe(pair) == 0);
    CHECK(close(pair[0]) == 0 && close(pair[1]) == 0);
    r.read_fd = pair[0]; r.began_ms = clock_ms();
    struct hs_hashminer_runtime_result out = hs_hashminer_runtime_poll(&r);
    CHECK(out.status == HS_HR_SYSTEM_ERROR && out.descriptor_cleanup_unknown);
    CHECK(!out.process_quiescent && !out.physical_power_off_confirmed && !out.mining_ready);
}
int main(void)
{
    argument_tests(); observation_tests(); cancellation_tests();
    malformed_ipc_tests(); owner_bridge_tests();
    printf("hashminer runtime: %lu parent checks PASS (real local children; synthetic attributes; no hardware)\n",checks);
    return 0;
}
