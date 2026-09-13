/* SPDX-License-Identifier: GPL-3.0-only */
#include "hs_miner_owner.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef NDEBUG
#error "Owner fault tests must not be compiled with NDEBUG"
#endif

static unsigned long checks;
#define CHECK(c) do { ++checks; if (!(c)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); \
} } while (0)

struct fake {
    struct hs_miner_owner owner;
    struct hs_miner_operation_request requests[256];
    int events[512];
    size_t count, event_count;
    uint32_t reject_mask;
    int physical_gate;
    int reject_open, reject_close;
    int reenter_begin, reenter_gate;
    int startup_live;
    unsigned workers;
};
static struct hs_miner_owner_result step(struct fake *f, uint64_t now,
    enum hs_miner_command command, uint32_t flags, enum hs_miner_owner_fault fault,
    struct hs_miner_operation_ack ack)
{
    const struct hs_miner_owner_input in = {now, command, flags, fault, ack};
    struct hs_miner_owner_result r = hs_miner_owner_step(&f->owner, &in);
    CHECK(!r.work_permitted || (r.phase == HS_MINER_RUNNING &&
        r.latched_fault == HS_MINER_EXEC_OK && flags == HS_MINER_READY_ALL &&
        !r.cleanup_active && !r.power_off_confirmed));
    CHECK(!r.quiescent_confirmed || (r.power_off_confirmed && !r.cleanup_active));
    return r;
}
static struct hs_miner_owner_result tick(struct fake *f, uint64_t now,
    enum hs_miner_command command, uint32_t flags)
{
    const struct hs_miner_operation_ack no_ack = {0};
    return step(f, now, command, flags, HS_MINER_FAULT_NONE, no_ack);
}
static int fake_begin(void *context, const struct hs_miner_operation_request *p)
{
    struct fake *f = context;
    CHECK(p != NULL && p->generation != 0 && p->token != 0);
    CHECK(p->operation < HS_MINER_OP_COUNT && p->timeout_ms != 0);
    CHECK(f->count < 256 && f->event_count < 512);
    if (f->count != 0) CHECK(p->token > f->requests[f->count - 1].token);
    if (p->operation == HS_MINER_OP_START) {
        CHECK(!f->physical_gate);
        f->startup_live = 1;
    }
    if (p->operation == HS_MINER_OP_DRAIN && f->owner.final_drain)
        CHECK(!f->startup_live);
    f->requests[f->count++] = *p;
    f->events[f->event_count++] = (int)p->operation;
    if (f->reenter_begin) {
        f->reenter_begin = 0;
        const struct hs_miner_owner_input in = {0};
        const struct hs_miner_owner_result r = hs_miner_owner_step(&f->owner, &in);
        CHECK(r.status == HS_MINER_EXEC_REENTRANT && !r.work_permitted);
    }
    return (f->reject_mask & (UINT32_C(1) << (unsigned)p->operation)) == 0;
}
static int fake_gate(void *context, uint64_t generation, int open)
{
    struct fake *f = context;
    CHECK(generation == f->owner.lifecycle.attempt_generation);
    CHECK(open == 0 || open == 1);
    CHECK(f->event_count < 512);
    f->events[f->event_count++] = open ? 101 : 100;
    if ((open && f->reject_open) || (!open && f->reject_close)) return 0;
    f->physical_gate = open;
    if (f->reenter_gate) {
        f->reenter_gate = 0;
        const struct hs_miner_owner_input in = {0};
        const struct hs_miner_owner_result r = hs_miner_owner_step(&f->owner, &in);
        CHECK(r.status == HS_MINER_EXEC_REENTRANT && !r.work_permitted);
    }
    return 1;
}
static const struct hs_miner_owner_ops ops = {fake_begin, fake_gate};

static const struct hs_miner_owner_policy policy = {{100,80,10,25,3},30};
static void setup(struct fake *f, uint32_t retries)
{
    struct hs_miner_owner_policy p = policy;
    memset(f, 0, sizeof(*f));
    p.lifecycle.max_retries = retries;
    CHECK(hs_miner_owner_init(&p, &ops, f, HS_MINER_OWNER_INITIAL_ALL,
                             &f->owner) == HS_MINER_EXEC_OK);
    CHECK(f->count == 0 && f->event_count == 0);
}
static struct hs_miner_operation_ack ack_for(struct fake *f,
    enum hs_miner_operation op, enum hs_miner_ack_outcome outcome)
{
    const struct hs_miner_operation_request *p = &f->owner.operations[op].request;
    const struct hs_miner_operation_ack a = {1,op,outcome,p->generation,p->token};
    return a;
}
static struct hs_miner_owner_result acknowledge_flags(struct fake *f, uint64_t now,
    enum hs_miner_operation op, enum hs_miner_ack_outcome outcome, uint32_t flags)
{
    if (outcome == HS_MINER_ACK_SUCCESS) {
        if (op == HS_MINER_OP_CANCEL_START) f->startup_live = 0;
        if (op == HS_MINER_OP_DRAIN) f->workers = 0;
    }
    return step(f, now, HS_MINER_COMMAND_NONE, flags,
                HS_MINER_FAULT_NONE, ack_for(f, op, outcome));
}
static struct hs_miner_owner_result acknowledge(struct fake *f, uint64_t now,
    enum hs_miner_operation op, enum hs_miner_ack_outcome outcome)
{
    return acknowledge_flags(f, now, op, outcome, HS_MINER_READY_ALL);
}
static void starting(struct fake *f, uint64_t now)
{
    struct hs_miner_owner_result r = tick(f, now, HS_MINER_COMMAND_RUN, HS_MINER_READY_ALL);
    CHECK(r.phase == HS_MINER_STARTING && !r.work_permitted && !r.power_off_confirmed);
    CHECK(f->owner.operations[HS_MINER_OP_START].state == HS_MINER_OP_PENDING);
}
static void running(struct fake *f, uint64_t now)
{
    starting(f, now);
    struct hs_miner_owner_result r = acknowledge(f, now + 1, HS_MINER_OP_START, HS_MINER_ACK_SUCCESS);
    CHECK(r.phase == HS_MINER_RUNNING && r.work_permitted && f->physical_gate);
}
static struct hs_miner_owner_result cleanup(struct fake *f, uint64_t now)
{
    struct hs_miner_owner_result r;
    CHECK(f->owner.cleanup_active && !f->owner.work_permitted);
    r = acknowledge(f, now, HS_MINER_OP_CANCEL_START, HS_MINER_ACK_SUCCESS);
    CHECK(r.cleanup_active && !r.quiescent_confirmed);
    r = acknowledge(f, now + 1, HS_MINER_OP_DRAIN, HS_MINER_ACK_SUCCESS);
    CHECK(r.cleanup_active && !r.power_off_confirmed);
    const uint64_t early_token = f->owner.operations[HS_MINER_OP_POWER_OFF].request.token;
    const uint64_t early_drain = f->owner.operations[HS_MINER_OP_DRAIN].request.token;
    r = acknowledge(f, now + 2, HS_MINER_OP_POWER_OFF, HS_MINER_ACK_SUCCESS);
    CHECK(r.cleanup_active && !r.quiescent_confirmed && !r.power_off_confirmed);
    CHECK(f->owner.operations[HS_MINER_OP_DRAIN].request.token > early_drain);
    CHECK(f->owner.operations[HS_MINER_OP_DRAIN].request.since_ms == now + 2);
    CHECK(f->owner.operations[HS_MINER_OP_POWER_OFF].request.token == early_token);
    r = acknowledge(f, now + 3, HS_MINER_OP_DRAIN, HS_MINER_ACK_SUCCESS);
    CHECK(r.cleanup_active && !r.quiescent_confirmed && !r.power_off_confirmed);
    CHECK(f->owner.operations[HS_MINER_OP_POWER_OFF].request.token > early_token);
    CHECK(f->owner.operations[HS_MINER_OP_POWER_OFF].request.since_ms == now + 3);
    r = acknowledge(f, now + 4, HS_MINER_OP_POWER_OFF, HS_MINER_ACK_SUCCESS);
    CHECK(!r.cleanup_active && r.quiescent_confirmed && r.power_off_confirmed);
    return r;
}

static void initialization_tests(void)
{
    struct fake f;
    struct hs_miner_owner before;
    struct hs_miner_owner_policy p;
    struct hs_miner_owner_ops badops;
    memset(&f, 0x5a, sizeof(f)); before = f.owner;
    CHECK(hs_miner_owner_init(NULL, &ops, &f, 7, &f.owner) == HS_MINER_EXEC_INVALID_ARGUMENT);
    CHECK(hs_miner_owner_init(&policy, NULL, &f, 7, &f.owner) == HS_MINER_EXEC_INVALID_ARGUMENT);
    CHECK(hs_miner_owner_init(&policy, &ops, &f, 7, NULL) == HS_MINER_EXEC_INVALID_ARGUMENT);
    CHECK(memcmp(&before, &f.owner, sizeof(before)) == 0);
    for (uint32_t flags = 0; flags < 16; ++flags) {
        if (flags == HS_MINER_OWNER_INITIAL_ALL) continue;
        CHECK(hs_miner_owner_init(&policy, &ops, &f, flags, &f.owner) == HS_MINER_EXEC_INVALID_ARGUMENT);
        CHECK(memcmp(&before, &f.owner, sizeof(before)) == 0);
    }
    for (unsigned n = 0; n < 5; ++n) {
        p = policy;
        if (n == 0) p.cleanup_operation_timeout_ms = 0;
        if (n == 1) p.cleanup_operation_timeout_ms = 81;
        if (n == 2) p.lifecycle.startup_timeout_ms = 0;
        if (n == 3) p.lifecycle.max_retries = 33;
        if (n == 4) p.lifecycle.max_backoff_ms = 9;
        CHECK(hs_miner_owner_init(&p, &ops, &f, 7, &f.owner) == HS_MINER_EXEC_INVALID_ARGUMENT);
        CHECK(memcmp(&before, &f.owner, sizeof(before)) == 0);
    }
    badops = ops; badops.begin = NULL;
    CHECK(hs_miner_owner_init(&policy, &badops, &f, 7, &f.owner) == HS_MINER_EXEC_INVALID_ARGUMENT);
    badops = ops; badops.work_gate = NULL;
    CHECK(hs_miner_owner_init(&policy, &badops, &f, 7, &f.owner) == HS_MINER_EXEC_INVALID_ARGUMENT);
    CHECK(hs_miner_owner_step(NULL, NULL).status == HS_MINER_EXEC_INVALID_ARGUMENT);
    CHECK(hs_miner_owner_step(&f.owner, NULL).status == HS_MINER_EXEC_INVALID_ARGUMENT);
    for (uint32_t flags = 0; flags <= HS_MINER_READY_ALL; ++flags) {
        setup(&f, 3);
        struct hs_miner_owner_result r = tick(&f, 100, HS_MINER_COMMAND_NONE, flags);
        CHECK(r.phase == HS_MINER_STOPPED && r.quiescent_confirmed && f.count == 0);
        if (flags == HS_MINER_READY_ALL) continue;
        r = tick(&f, 101, HS_MINER_COMMAND_RUN, flags);
        CHECK(r.phase == HS_MINER_WAITING_HARDWARE && !r.work_permitted && f.count == 0);
        r = tick(&f, 10000, HS_MINER_COMMAND_NONE, flags);
        CHECK(r.phase == HS_MINER_WAITING_HARDWARE && f.count == 0);
    }
}

static void normal_and_stale_tests(void)
{
    struct fake f;
    struct hs_miner_owner_result r;
    setup(&f, 3); starting(&f, 100);
    struct hs_miner_operation_ack a = ack_for(&f, HS_MINER_OP_START, HS_MINER_ACK_SUCCESS);
    for (unsigned mismatch = 0; mismatch < 3; ++mismatch) {
        struct hs_miner_operation_ack wrong = a;
        if (mismatch == 0) wrong.generation++;
        if (mismatch == 1) wrong.token++;
        if (mismatch == 2) wrong.operation = HS_MINER_OP_DRAIN;
        r = step(&f, 101, HS_MINER_COMMAND_NONE, 31, HS_MINER_FAULT_NONE, wrong);
        CHECK(r.ack_ignored && r.phase == HS_MINER_STARTING && !r.work_permitted);
    }
    r = step(&f, 102, HS_MINER_COMMAND_NONE, 31, HS_MINER_FAULT_NONE, a);
    CHECK(r.work_permitted);
    r = step(&f, 103, HS_MINER_COMMAND_NONE, 31, HS_MINER_FAULT_NONE, a);
    CHECK(r.ack_ignored && r.work_permitted);
    size_t event_before = f.event_count;
    r = tick(&f, 104, HS_MINER_COMMAND_STOP, 31);
    CHECK(r.phase == HS_MINER_DRAINING && !r.work_permitted && !f.physical_gate);
    CHECK(f.events[event_before] == 100);
    CHECK(f.count == 4 && f.requests[1].operation == HS_MINER_OP_CANCEL_START &&
          f.requests[2].operation == HS_MINER_OP_DRAIN &&
          f.requests[3].operation == HS_MINER_OP_POWER_OFF);
    r = step(&f, 105, HS_MINER_COMMAND_NONE, 31, HS_MINER_FAULT_NONE, a);
    CHECK(r.ack_ignored && !r.work_permitted);
    r = cleanup(&f, 106);
    CHECK(r.phase == HS_MINER_STOPPED && f.count == 6);
    for (uint64_t n = 110; n < 120; ++n) {
        r = tick(&f, n, HS_MINER_COMMAND_NONE, 31);
        CHECK(r.phase == HS_MINER_STOPPED && !r.work_permitted && f.count == 6);
    }
    starting(&f, 120);
    CHECK(f.owner.lifecycle.attempt_generation == 2 && f.requests[6].token > a.token);
    r = step(&f, 121, HS_MINER_COMMAND_NONE, 31, HS_MINER_FAULT_NONE, a);
    CHECK(r.ack_ignored && !r.work_permitted);

    setup(&f, 3); starting(&f, 100);
    a = ack_for(&f, HS_MINER_OP_START, HS_MINER_ACK_SUCCESS);
    r = step(&f, 101, HS_MINER_COMMAND_STOP, 31, HS_MINER_FAULT_NONE, a);
    CHECK(r.ack_ignored && r.cleanup_active && !r.work_permitted);
    r = cleanup(&f, 102); CHECK(r.phase == HS_MINER_STOPPED);
    setup(&f, 3); starting(&f, 100);
    a = ack_for(&f, HS_MINER_OP_START, HS_MINER_ACK_SUCCESS);
    r = step(&f, 200, HS_MINER_COMMAND_NONE, 31, HS_MINER_FAULT_NONE, a);
    CHECK(r.ack_ignored && r.cleanup_active && r.reason == HS_MINER_REASON_START_TIMEOUT);
    CHECK(!r.work_permitted && r.latched_fault == HS_MINER_EXEC_OK);
}

static void cleanup_barrier_tests(void)
{
    static const unsigned permutations[6][3] = {
        {1,2,3},{1,3,2},{2,1,3},{2,3,1},{3,1,2},{3,2,1}
    };
    for (unsigned n = 0; n < 6; ++n) {
        struct fake f; setup(&f, 3); running(&f, 100);
        struct hs_miner_owner_result r = tick(&f, 102, HS_MINER_COMMAND_STOP, 31);
        const struct hs_miner_operation_ack early = ack_for(&f, HS_MINER_OP_POWER_OFF, HS_MINER_ACK_SUCCESS);
        for (unsigned i = 0; i < 3; ++i) {
            r = acknowledge(&f, 103 + i, (enum hs_miner_operation)permutations[n][i], HS_MINER_ACK_SUCCESS);
            CHECK(!r.work_permitted && !r.quiescent_confirmed && !r.power_off_confirmed);
            CHECK(f.count == (i == 2 ? 5U : 4U));
        }
        r = step(&f, 106, HS_MINER_COMMAND_NONE, 31, HS_MINER_FAULT_NONE, early);
        CHECK(r.ack_ignored && !r.quiescent_confirmed && f.count == 5);
        r = acknowledge(&f, 107, HS_MINER_OP_DRAIN, HS_MINER_ACK_SUCCESS);
        CHECK(!r.quiescent_confirmed && f.count == 6);
        r = acknowledge(&f, 108, HS_MINER_OP_POWER_OFF, HS_MINER_ACK_SUCCESS);
        CHECK(r.phase == HS_MINER_STOPPED && r.quiescent_confirmed && f.count == 6);
    }

    struct fake f; setup(&f, 3); running(&f, 100);
    (void)tick(&f, 102, HS_MINER_COMMAND_SHUTDOWN, 31);
    (void)acknowledge(&f, 103, HS_MINER_OP_CANCEL_START, HS_MINER_ACK_SUCCESS);
    (void)acknowledge(&f, 104, HS_MINER_OP_DRAIN, HS_MINER_ACK_SUCCESS);
    (void)acknowledge(&f, 105, HS_MINER_OP_POWER_OFF, HS_MINER_ACK_SUCCESS);
    (void)acknowledge(&f, 106, HS_MINER_OP_DRAIN, HS_MINER_ACK_SUCCESS);
    struct hs_miner_owner_result r = step(&f, 107, HS_MINER_COMMAND_RUN, 31,
        HS_MINER_FAULT_NONE, ack_for(&f, HS_MINER_OP_POWER_OFF, HS_MINER_ACK_SUCCESS));
    CHECK(r.status == HS_MINER_EXEC_COMMAND_REJECTED && r.phase == HS_MINER_SHUTDOWN);
    CHECK(r.quiescent_confirmed && f.count == 6);
}

static void retry_tests(void)
{
    for (uint32_t budget = 0; budget <= 3; ++budget) {
        struct fake f; setup(&f, budget);
        uint64_t now = 100;
        starting(&f, now);
        for (uint32_t attempt = 0; attempt <= budget; ++attempt) {
            struct hs_miner_owner_result r = acknowledge(&f, ++now, HS_MINER_OP_START, HS_MINER_ACK_FAILURE);
            CHECK(r.cleanup_active && r.latched_fault == HS_MINER_EXEC_OK);
            r = cleanup(&f, ++now); now += 4;
            CHECK(r.retries_reserved == (attempt < budget ? attempt + 1 : budget));
            if (attempt == budget) {
                CHECK(r.phase == HS_MINER_FAULTED && r.reason == HS_MINER_REASON_RETRY_LIMIT);
                r = tick(&f, ++now, HS_MINER_COMMAND_RUN, 31);
                CHECK(r.status == HS_MINER_EXEC_COMMAND_REJECTED && !r.work_permitted);
                r = tick(&f, ++now, HS_MINER_COMMAND_STOP, 31);
                CHECK(r.phase == HS_MINER_STOPPED);
                r = tick(&f, ++now, HS_MINER_COMMAND_NONE, 31);
                CHECK(r.phase == HS_MINER_STOPPED);
                starting(&f, ++now);
                CHECK(f.owner.lifecycle.retries_reserved == 0);
                break;
            }
            CHECK(r.phase == HS_MINER_BACKOFF);
            uint64_t delay = attempt == 0 ? 10 : attempt == 1 ? 20 : 25;
            size_t count = f.count;
            r = tick(&f, now + delay - 1, HS_MINER_COMMAND_RUN, 31);
            CHECK(r.phase == HS_MINER_BACKOFF && f.count == count);
            now += delay;
            r = tick(&f, now, HS_MINER_COMMAND_NONE, 31);
            CHECK(r.phase == HS_MINER_STARTING && f.count == count + 1);
        }
    }

    struct fake f; setup(&f, 3);
    f.reject_mask = UINT32_C(1) << HS_MINER_OP_START;
    struct hs_miner_owner_result r = tick(&f, 100, HS_MINER_COMMAND_RUN, 31);
    CHECK(r.cleanup_active && r.phase == HS_MINER_DRAINING && f.count == 4);
    CHECK(r.latched_fault == HS_MINER_EXEC_OK);
    r = cleanup(&f, 101); CHECK(r.phase == HS_MINER_BACKOFF);

    setup(&f, 1); running(&f, 100);
    const struct hs_miner_operation_ack no_ack = {0};
    r = step(&f, 102, HS_MINER_COMMAND_NONE, 31, HS_MINER_FAULT_RUNTIME, no_ack);
    CHECK(r.cleanup_active && !r.work_permitted);
    r = cleanup(&f, 103); CHECK(r.phase == HS_MINER_BACKOFF && r.retries_reserved == 1);

    setup(&f, 1); running(&f, 100);
    (void)tick(&f, 102, HS_MINER_COMMAND_NONE, 0);
    (void)acknowledge(&f, 103, HS_MINER_OP_CANCEL_START, HS_MINER_ACK_SUCCESS);
    (void)acknowledge(&f, 104, HS_MINER_OP_DRAIN, HS_MINER_ACK_SUCCESS);
    (void)acknowledge(&f, 105, HS_MINER_OP_POWER_OFF, HS_MINER_ACK_SUCCESS);
    (void)acknowledge(&f, 106, HS_MINER_OP_DRAIN, HS_MINER_ACK_SUCCESS);
    r = step(&f, 107, HS_MINER_COMMAND_NONE, 31, HS_MINER_FAULT_RUNTIME,
             ack_for(&f, HS_MINER_OP_POWER_OFF, HS_MINER_ACK_SUCCESS));
    CHECK(r.phase == HS_MINER_BACKOFF && r.retries_reserved == 1);
}

static void late_startup_worker_tests(void)
{
    for (unsigned fail_start = 0; fail_start < 2; ++fail_start) {
        struct fake f; setup(&f, 1); starting(&f, 100);
        struct hs_miner_owner_result r;
        if (fail_start)
            r = acknowledge(&f, 101, HS_MINER_OP_START, HS_MINER_ACK_FAILURE);
        else
            r = tick(&f, 101, HS_MINER_COMMAND_STOP, 31);
        CHECK(r.cleanup_active && f.startup_live && f.count == 4);
        const struct hs_miner_operation_ack early_drain = ack_for(&f, HS_MINER_OP_DRAIN, HS_MINER_ACK_SUCCESS);
        const struct hs_miner_operation_ack early_off = ack_for(&f, HS_MINER_OP_POWER_OFF, HS_MINER_ACK_SUCCESS);
        f.workers = 1;
        r = acknowledge(&f, 102, HS_MINER_OP_DRAIN, HS_MINER_ACK_SUCCESS);
        CHECK(f.workers == 0 && f.startup_live && !r.quiescent_confirmed);

        f.workers++;
        r = acknowledge(&f, 103, HS_MINER_OP_POWER_OFF, HS_MINER_ACK_SUCCESS);
        CHECK(f.workers == 1 && !r.quiescent_confirmed && f.count == 4);
        r = acknowledge(&f, 104, HS_MINER_OP_CANCEL_START, HS_MINER_ACK_SUCCESS);
        CHECK(!f.startup_live && f.workers == 1 && !r.quiescent_confirmed);
        CHECK(f.count == 5 && f.requests[4].operation == HS_MINER_OP_DRAIN);
        CHECK(f.requests[4].token != early_drain.token && f.requests[4].since_ms == 104);
        CHECK(f.owner.operations[HS_MINER_OP_POWER_OFF].request.token == early_off.token);
        r = step(&f, 105, HS_MINER_COMMAND_NONE, 31, HS_MINER_FAULT_NONE, early_drain);
        CHECK(r.ack_ignored && !r.quiescent_confirmed && f.workers == 1 && f.count == 5);
        r = step(&f, 106, HS_MINER_COMMAND_NONE, 31, HS_MINER_FAULT_NONE, early_off);
        CHECK(r.ack_ignored && !r.quiescent_confirmed && r.retries_reserved == 0);
        r = acknowledge(&f, 107, HS_MINER_OP_DRAIN, HS_MINER_ACK_SUCCESS);
        CHECK(f.workers == 0 && !r.quiescent_confirmed && f.count == 6);
        CHECK(f.requests[5].operation == HS_MINER_OP_POWER_OFF && f.requests[5].since_ms == 107);
        r = step(&f, 108, HS_MINER_COMMAND_NONE, 31, HS_MINER_FAULT_NONE, early_off);
        CHECK(r.ack_ignored && !r.quiescent_confirmed && !r.work_permitted);
        r = acknowledge(&f, 109, HS_MINER_OP_POWER_OFF, HS_MINER_ACK_SUCCESS);
        CHECK(r.quiescent_confirmed && !r.work_permitted && f.workers == 0);
        CHECK(r.phase == (fail_start ? HS_MINER_BACKOFF : HS_MINER_STOPPED));
        CHECK(r.retries_reserved == fail_start && f.count == 6);
    }
}

static void concurrent_readiness_failure_tests(void)
{
    for (unsigned failure = 0; failure < 3; ++failure) {
        for (uint32_t budget = 0; budget <= 2; ++budget) {
            struct fake f; setup(&f, budget);
            uint64_t now = 100;
            starting(&f, now);
            for (uint32_t attempt = 0; attempt <= budget; ++attempt) {
                struct hs_miner_owner_result r;
                if (failure == 0) {
                    (void)acknowledge(&f, ++now, HS_MINER_OP_START, HS_MINER_ACK_SUCCESS);
                    const struct hs_miner_operation_ack no_ack = {0};
                    r = step(&f, ++now, HS_MINER_COMMAND_NONE, 0, HS_MINER_FAULT_RUNTIME, no_ack);
                } else if (failure == 1) {
                    r = acknowledge_flags(&f, ++now, HS_MINER_OP_START, HS_MINER_ACK_FAILURE, 0);
                } else {
                    now += 100;
                    r = tick(&f, now, HS_MINER_COMMAND_NONE, 0);
                }
                CHECK(r.cleanup_active && !r.work_permitted && r.latched_fault == HS_MINER_EXEC_OK);
                CHECK(f.owner.attempt_failed && r.reason == HS_MINER_REASON_RUN_FAILED);
                (void)acknowledge_flags(&f, ++now, HS_MINER_OP_CANCEL_START, HS_MINER_ACK_SUCCESS, 0);
                (void)acknowledge_flags(&f, ++now, HS_MINER_OP_DRAIN, HS_MINER_ACK_SUCCESS, 0);
                (void)acknowledge_flags(&f, ++now, HS_MINER_OP_POWER_OFF, HS_MINER_ACK_SUCCESS, 0);
                r = acknowledge_flags(&f, ++now, HS_MINER_OP_DRAIN, HS_MINER_ACK_SUCCESS, 0);
                CHECK(!r.quiescent_confirmed && !r.work_permitted && r.retries_reserved == attempt);
                r = acknowledge_flags(&f, ++now, HS_MINER_OP_POWER_OFF, HS_MINER_ACK_SUCCESS, 0);
                CHECK(r.quiescent_confirmed && !r.work_permitted);
                const size_t requests = f.count;
                if (attempt == budget) {
                    CHECK(r.phase == HS_MINER_FAULTED && r.reason == HS_MINER_REASON_RETRY_LIMIT);
                    CHECK(r.retries_reserved == budget);
                    r = tick(&f, now + 1000, HS_MINER_COMMAND_NONE, 31);
                    CHECK(r.phase == HS_MINER_FAULTED && f.count == requests && !r.work_permitted);
                } else {
                    CHECK(r.phase == HS_MINER_WAITING_HARDWARE && r.retries_reserved == attempt + 1);
                    r = tick(&f, now + 1, HS_MINER_COMMAND_NONE, 31);
                    CHECK(r.phase == HS_MINER_BACKOFF && f.count == requests);
                    now += attempt == 0 ? 10 : 20;
                    r = tick(&f, now, HS_MINER_COMMAND_NONE, 31);
                    CHECK(r.phase == HS_MINER_STARTING && f.count == requests + 1);
                    CHECK(!f.owner.attempt_failed);
                }
            }
        }
    }
}

static void token_reservation_tests(void)
{
    for (uint64_t remaining = 0; remaining < 6; ++remaining) {
        struct fake f; setup(&f, 3);
        f.owner.last_token = UINT64_MAX - remaining;
        struct hs_miner_owner_result r = tick(&f, 100, HS_MINER_COMMAND_RUN, 31);
        CHECK(r.latched_fault == HS_MINER_EXEC_TOKEN_EXHAUSTED);
        CHECK(f.count == 0 && r.generation == 0 && r.quiescent_confirmed);
        CHECK(r.power_off_confirmed && !r.cleanup_active && !r.work_permitted);
        CHECK(f.owner.last_token == UINT64_MAX - remaining);
    }
    for (uint64_t remaining = 6; remaining <= 7; ++remaining) {
        struct fake f; setup(&f, 3);
        f.owner.last_token = UINT64_MAX - remaining;
        starting(&f, 100);
        struct hs_miner_owner_result r = tick(&f, 101, HS_MINER_COMMAND_STOP, 31);
        CHECK(r.latched_fault == HS_MINER_EXEC_OK && f.count == 4);
        r = cleanup(&f, 102);
        CHECK(r.latched_fault == HS_MINER_EXEC_OK && r.quiescent_confirmed && f.count == 6);
        CHECK(f.owner.last_token == UINT64_MAX - remaining + 6);
        r = tick(&f, 110, HS_MINER_COMMAND_RUN, 31);
        CHECK(r.latched_fault == HS_MINER_EXEC_TOKEN_EXHAUSTED && f.count == 6);
        CHECK(r.quiescent_confirmed && !r.cleanup_active && r.generation == 1);
    }
}

static void failure_tests(void)
{
    for (unsigned mode = 0; mode < 3; ++mode) {
        for (unsigned failed_op = 1; failed_op < HS_MINER_OP_COUNT; ++failed_op) {
            struct fake f; setup(&f, 3); running(&f, 100);
            if (mode == 0) f.reject_mask = UINT32_C(1) << failed_op;
            struct hs_miner_owner_result r = tick(&f, 102, HS_MINER_COMMAND_STOP, 31);
            CHECK(f.count == 4 && !r.work_permitted);
            if (mode == 1) r = acknowledge(&f, 103, (enum hs_miner_operation)failed_op, HS_MINER_ACK_FAILURE);
            if (mode == 2) {
                for (unsigned n = 1; n < HS_MINER_OP_COUNT; ++n)
                    if (n != failed_op) (void)acknowledge(&f, 103, (enum hs_miner_operation)n, HS_MINER_ACK_SUCCESS);
                r = acknowledge(&f, 132, (enum hs_miner_operation)failed_op, HS_MINER_ACK_SUCCESS);
                CHECK(r.ack_ignored);
            }
            CHECK(r.latched_fault == (mode == 0 ? HS_MINER_EXEC_OPERATION_REJECTED :
                  mode == 1 ? HS_MINER_EXEC_OPERATION_FAILED : HS_MINER_EXEC_OPERATION_TIMEOUT));
            CHECK(!r.quiescent_confirmed && !r.work_permitted && r.cleanup_active);
            size_t count = f.count;
            r = tick(&f, 10000, HS_MINER_COMMAND_RUN, 31);
            CHECK(r.latched_fault != HS_MINER_EXEC_OK && !r.work_permitted && f.count == count);
        }
    }

    for (unsigned final_op = HS_MINER_OP_DRAIN; final_op <= HS_MINER_OP_POWER_OFF; ++final_op)
    for (unsigned fail = 0; fail < 3; ++fail) {
        struct fake f; setup(&f, 3); running(&f, 100);
        (void)tick(&f, 102, HS_MINER_COMMAND_STOP, 31);
        (void)acknowledge(&f, 103, HS_MINER_OP_CANCEL_START, HS_MINER_ACK_SUCCESS);
        (void)acknowledge(&f, 104, HS_MINER_OP_DRAIN, HS_MINER_ACK_SUCCESS);
        if (fail == 0) f.reject_mask = UINT32_C(1) << final_op;
        struct hs_miner_owner_result r = acknowledge(&f, 105, HS_MINER_OP_POWER_OFF, HS_MINER_ACK_SUCCESS);
        uint64_t now = 105;
        if (final_op == HS_MINER_OP_POWER_OFF)
            r = acknowledge(&f, ++now, HS_MINER_OP_DRAIN, HS_MINER_ACK_SUCCESS);
        if (fail == 1) r = acknowledge(&f, now + 1, (enum hs_miner_operation)final_op, HS_MINER_ACK_FAILURE);
        if (fail == 2) r = acknowledge(&f, now + 30, (enum hs_miner_operation)final_op, HS_MINER_ACK_SUCCESS);
        CHECK(r.latched_fault != HS_MINER_EXEC_OK && !r.power_off_confirmed && !r.quiescent_confirmed);
    }
    for (unsigned safety = HS_MINER_FAULT_THERMAL; safety <= HS_MINER_FAULT_FAN; ++safety) {
        for (unsigned active = 0; active < 2; ++active) {
            struct fake f; setup(&f, 3);
            if (active) running(&f, 100); else starting(&f, 100);
            const struct hs_miner_operation_ack no_ack = {0};
            struct hs_miner_owner_result r = step(&f, 102, HS_MINER_COMMAND_NONE, 31,
                (enum hs_miner_owner_fault)safety, no_ack);
            CHECK(r.latched_fault == HS_MINER_EXEC_SAFETY_FAULT && r.cleanup_active && !r.work_permitted);
            r = cleanup(&f, 103);
            CHECK(r.phase == HS_MINER_SHUTDOWN && r.latched_fault == HS_MINER_EXEC_SAFETY_FAULT);
            r = tick(&f, 200, HS_MINER_COMMAND_RUN, 31);
            CHECK(!r.work_permitted && f.count == 6);
        }
    }
    for (uint32_t flags = 0; flags < HS_MINER_READY_ALL; ++flags) {
        struct fake f; setup(&f, 3); running(&f, 100);
        struct hs_miner_owner_result r = tick(&f, 102, HS_MINER_COMMAND_NONE, flags);
        CHECK(r.cleanup_active && !r.work_permitted && r.latched_fault == HS_MINER_EXEC_OK);
        r = tick(&f, 103, HS_MINER_COMMAND_NONE, 31);
        CHECK(r.cleanup_active && !r.work_permitted && f.count == 4);
        r = cleanup(&f, 104);
        CHECK(r.phase == HS_MINER_WAITING_HARDWARE && r.retries_reserved == 0);
    }
}

static void contract_and_boundary_tests(void)
{
    struct fake f;
    struct hs_miner_owner_result r;
    for (unsigned mode = 0; mode < 4; ++mode) {
        setup(&f, 3);
        if (mode == 0) f.reenter_begin = 1;
        if (mode == 1) f.reject_open = 1;
        if (mode == 2) f.reenter_gate = 1;
        r = tick(&f, 100, HS_MINER_COMMAND_RUN, 31);
        if (mode != 0) r = acknowledge(&f, 101, HS_MINER_OP_START, HS_MINER_ACK_SUCCESS);
        if (mode == 3) {
            CHECK(r.work_permitted); f.reject_close = 1;
            r = tick(&f, 102, HS_MINER_COMMAND_STOP, 31);
        }
        CHECK(r.latched_fault == ((mode == 0 || mode == 2) ? HS_MINER_EXEC_REENTRANT : HS_MINER_EXEC_GATE_FAILED));
        CHECK(!r.work_permitted && r.cleanup_active && f.count == 4);
        if (mode != 3) CHECK(!f.physical_gate);
    }
    for (unsigned invalid = 0; invalid < 6; ++invalid) {
        setup(&f, 3); running(&f, 100);
        struct hs_miner_owner_input in = {102,HS_MINER_COMMAND_NONE,31,HS_MINER_FAULT_NONE,{0}};
        if (invalid == 0) in.command = (enum hs_miner_command)99;
        if (invalid == 1) in.ready_flags = 32;
        if (invalid == 2) in.fault = (enum hs_miner_owner_fault)99;
        if (invalid == 3) in.ack.present = 2;
        if (invalid == 4) { in.ack.present = 1; in.ack.operation = HS_MINER_OP_COUNT; }
        if (invalid == 5) { in.ack.present = 1; in.ack.outcome = (enum hs_miner_ack_outcome)99; }
        r = hs_miner_owner_step(&f.owner, &in);
        CHECK(r.latched_fault == HS_MINER_EXEC_INVALID_ARGUMENT && r.cleanup_active && !r.work_permitted);
        CHECK(!f.physical_gate);
    }
    setup(&f, 3); running(&f, 100);
    r = hs_miner_owner_step(&f.owner, NULL);
    CHECK(r.latched_fault == HS_MINER_EXEC_INVALID_ARGUMENT && r.cleanup_active && !r.work_permitted);
    setup(&f, 3); running(&f, 100);
    r = tick(&f, 99, HS_MINER_COMMAND_NONE, 31);
    CHECK(r.latched_fault == HS_MINER_EXEC_TIME_REVERSED && f.owner.last_now_ms == 101);
    CHECK(!r.work_permitted && r.cleanup_active);

    setup(&f, 3); f.owner.last_token = UINT64_MAX;
    r = tick(&f, 100, HS_MINER_COMMAND_RUN, 31);
    CHECK(r.latched_fault == HS_MINER_EXEC_TOKEN_EXHAUSTED && f.count == 0 && !r.work_permitted);
    setup(&f, 3); f.owner.lifecycle.attempt_generation = UINT64_MAX;
    r = tick(&f, 100, HS_MINER_COMMAND_RUN, 31);
    CHECK(r.phase == HS_MINER_FAULTED && f.count == 0 && !r.work_permitted);
    CHECK(r.reason == HS_MINER_REASON_GENERATION_EXHAUSTED);
    setup(&f, 3); starting(&f, UINT64_MAX - 101);
    r = acknowledge(&f, UINT64_MAX - 2, HS_MINER_OP_START, HS_MINER_ACK_SUCCESS);
    CHECK(r.work_permitted);
    r = tick(&f, UINT64_MAX, HS_MINER_COMMAND_STOP, 31);
    CHECK(r.cleanup_active);
    r = tick(&f, 0, HS_MINER_COMMAND_NONE, 31);
    CHECK(r.latched_fault == HS_MINER_EXEC_TIME_REVERSED && !r.work_permitted);

    setup(&f, 3); f.owner.cleanup_operation_timeout_ms = 80;
    running(&f, 100); (void)tick(&f, 102, HS_MINER_COMMAND_STOP, 31);
    (void)acknowledge(&f, 130, HS_MINER_OP_CANCEL_START, HS_MINER_ACK_SUCCESS);
    (void)acknowledge(&f, 150, HS_MINER_OP_DRAIN, HS_MINER_ACK_SUCCESS);
    (void)acknowledge(&f, 170, HS_MINER_OP_POWER_OFF, HS_MINER_ACK_SUCCESS);
    (void)acknowledge(&f, 175, HS_MINER_OP_DRAIN, HS_MINER_ACK_SUCCESS);
    r = acknowledge(&f, 182, HS_MINER_OP_POWER_OFF, HS_MINER_ACK_SUCCESS);
    CHECK(r.latched_fault == HS_MINER_EXEC_CLEANUP_TIMEOUT && !r.work_permitted);

    CHECK(r.quiescent_confirmed && r.phase == HS_MINER_SHUTDOWN);
}

static void make_phase(struct fake *f, unsigned phase)
{
    setup(f, phase == HS_MINER_FAULTED ? 0 : 3);
    if (phase == HS_MINER_STOPPED) return;
    if (phase == HS_MINER_SHUTDOWN) { (void)tick(f,100,HS_MINER_COMMAND_SHUTDOWN,31); return; }
    if (phase == HS_MINER_WAITING_HARDWARE) { (void)tick(f,100,HS_MINER_COMMAND_RUN,0); return; }
    starting(f,100);
    if (phase == HS_MINER_STARTING) return;
    if (phase == HS_MINER_RUNNING || phase == HS_MINER_DRAINING) {
        (void)acknowledge(f,101,HS_MINER_OP_START,HS_MINER_ACK_SUCCESS);
        if (phase == HS_MINER_DRAINING) (void)tick(f,102,HS_MINER_COMMAND_STOP,31);
        return;
    }
    (void)acknowledge(f,101,HS_MINER_OP_START,HS_MINER_ACK_FAILURE);
    (void)cleanup(f,102);
    CHECK((unsigned)f->owner.lifecycle.phase == phase);
}
static void transition_matrix(void)
{
    for (unsigned phase = HS_MINER_STOPPED; phase <= HS_MINER_SHUTDOWN; ++phase)
        for (unsigned command = HS_MINER_COMMAND_NONE; command <= HS_MINER_COMMAND_SHUTDOWN; ++command)
            for (uint32_t flags = 0; flags <= HS_MINER_READY_ALL; ++flags)
                for (unsigned event = 0; event < 4; ++event) {
                    struct fake f; make_phase(&f, phase);
                    struct hs_miner_operation_ack a = {0};
                    if (event == 1) a = ack_for(&f,HS_MINER_OP_START,HS_MINER_ACK_SUCCESS);
                    if (event == 2) { a = ack_for(&f,HS_MINER_OP_START,HS_MINER_ACK_SUCCESS); a.generation++; }
                    if (event == 3) a = ack_for(&f,HS_MINER_OP_DRAIN,HS_MINER_ACK_SUCCESS);
                    const uint64_t now = f.owner.last_now_ms + 1;
                    size_t before = f.count;
                    struct hs_miner_owner_result r = step(&f,now,(enum hs_miner_command)command,
                        flags,HS_MINER_FAULT_NONE,a);
                    CHECK(f.count - before <= 4 && f.physical_gate == r.work_permitted);
                    CHECK(r.status == HS_MINER_EXEC_OK || r.status == HS_MINER_EXEC_COMMAND_REJECTED);
                    if (command == HS_MINER_COMMAND_STOP || command == HS_MINER_COMMAND_SHUTDOWN || flags != 31)
                        CHECK(!r.work_permitted);
                    r = tick(&f,now + 1,HS_MINER_COMMAND_NONE,flags);
                    CHECK(f.physical_gate == r.work_permitted && r.latched_fault == HS_MINER_EXEC_OK);
                }
}
int main(void)
{
    initialization_tests();
    normal_and_stale_tests();
    cleanup_barrier_tests();
    retry_tests();
    late_startup_worker_tests();
    concurrent_readiness_failure_tests();
    token_reservation_tests();
    failure_tests();
    contract_and_boundary_tests();
    transition_matrix();
    printf("miner owner: %lu checks PASS (injected callbacks; no hardware)\n", checks);
    return 0;
}
