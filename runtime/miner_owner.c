/* SPDX-License-Identifier: GPL-3.0-only */
/* Asynchronous lifecycle executor. */
#include "hs_miner_owner.h"
#include <stddef.h>

static void latch(struct hs_miner_owner *o, enum hs_miner_owner_status fault)
{
    if (o->fault == HS_MINER_EXEC_OK) o->fault = fault;
}

static struct hs_miner_owner_result snapshot(const struct hs_miner_owner *o,
    enum hs_miner_owner_status status, uint8_t ignored)
{
    struct hs_miner_owner_result r = {0};
    r.status = status;
    r.phase = HS_MINER_FAULTED;
    if (o != NULL) {
        r.latched_fault = o->fault;
        if (o->fault != HS_MINER_EXEC_OK) r.status = o->fault;
        r.phase = o->lifecycle.phase;
        r.reason = o->lifecycle.reason;
        r.generation = o->lifecycle.attempt_generation;
        r.retries_reserved = o->lifecycle.retries_reserved;
        r.work_permitted = (uint8_t)(o->work_permitted && o->fault == HS_MINER_EXEC_OK);
        r.cleanup_active = o->cleanup_active;
        r.power_off_confirmed = o->power_off_confirmed;
        r.quiescent_confirmed = (uint8_t)(o->lifecycle.owner_quiescent &&
            o->power_off_confirmed && !o->cleanup_active);
    }
    r.ack_ignored = ignored;
    return r;
}

static void close_gate(struct hs_miner_owner *o, int force)
{
    int was_open = o->work_permitted;
    o->work_permitted = 0;
    if ((was_open || force) &&
        o->ops.work_gate(o->context, o->lifecycle.attempt_generation, 0) != 1)
        latch(o, HS_MINER_EXEC_GATE_FAILED);
}

static void issue(struct hs_miner_owner *o, enum hs_miner_operation operation,
                  uint64_t now)
{
    struct hs_miner_owned_operation *p = &o->operations[operation];
    if (p->state != HS_MINER_OP_IDLE) return;
    if (o->last_token == UINT64_MAX) {
        p->state = HS_MINER_OP_FAILED;
        latch(o, HS_MINER_EXEC_TOKEN_EXHAUSTED);
        return;
    }
    p->request.operation = operation;
    p->request.generation = o->lifecycle.attempt_generation;
    p->request.token = ++o->last_token;
    p->request.since_ms = now;
    p->request.timeout_ms = operation == HS_MINER_OP_START ?
        o->lifecycle.policy.startup_timeout_ms : o->cleanup_operation_timeout_ms;
    p->state = HS_MINER_OP_PENDING;
    if (o->ops.begin(o->context, &p->request) != 1) {
        p->state = HS_MINER_OP_FAILED;
        if (operation != HS_MINER_OP_START)
            latch(o, HS_MINER_EXEC_OPERATION_REJECTED);
    }
}

static void begin_cleanup(struct hs_miner_owner *o, uint64_t now)
{
    if (o->cleanup_active) return;
    close_gate(o, 1);
    o->cleanup_active = 1;
    o->final_drain = 0;
    o->final_power_off = 0;
    o->power_off_confirmed = 0;
    o->cleanup_since_ms = now;
    o->operations[HS_MINER_OP_START].state = HS_MINER_OP_RETIRED;
    for (unsigned n = HS_MINER_OP_CANCEL_START; n < HS_MINER_OP_COUNT; ++n)
        o->operations[n].state = HS_MINER_OP_IDLE;

    issue(o, HS_MINER_OP_CANCEL_START, now);
    issue(o, HS_MINER_OP_DRAIN, now);
    issue(o, HS_MINER_OP_POWER_OFF, now);
}

static struct hs_miner_lifecycle_result transition(struct hs_miner_owner *o,
    const struct hs_miner_owner_input *in, enum hs_miner_owner_event event)
{
    struct hs_miner_lifecycle_input li = {0};
    li.now_ms = in->now_ms;
    li.command = o->fault == HS_MINER_EXEC_OK ? in->command : HS_MINER_COMMAND_SHUTDOWN;
    li.ready_flags = o->fault == HS_MINER_EXEC_OK ? in->ready_flags : 0;
    li.owner_event = event;
    li.owner_generation = o->lifecycle.attempt_generation;
    struct hs_miner_lifecycle_result lr = hs_miner_lifecycle_step(&o->lifecycle, &li);
    if (lr.status != HS_MINER_LIFECYCLE_OK &&
        lr.status != HS_MINER_LIFECYCLE_COMMAND_REJECTED)
        latch(o, HS_MINER_EXEC_LIFECYCLE_ERROR);
    return lr;
}

static void record_attempt_failure(struct hs_miner_owner *o,
    const struct hs_miner_owner_input *in)
{
    if (o->attempt_failed && o->lifecycle.phase == HS_MINER_DRAINING &&
        o->lifecycle.reason != HS_MINER_REASON_START_FAILED &&
        o->lifecycle.reason != HS_MINER_REASON_START_TIMEOUT &&
        o->lifecycle.reason != HS_MINER_REASON_RUN_FAILED) {
        struct hs_miner_owner_input next = *in;
        next.command = HS_MINER_COMMAND_NONE;
        (void)transition(o, &next, HS_MINER_OWNER_FAILED);
    }
}

enum hs_miner_owner_status hs_miner_owner_init(
    const struct hs_miner_owner_policy *policy,
    const struct hs_miner_owner_ops *ops, void *context,
    uint32_t initial_confirmations, struct hs_miner_owner *destination)
{
    struct hs_miner_owner o = {0};
    if (policy == NULL || ops == NULL || destination == NULL ||
        ops->begin == NULL || ops->work_gate == NULL ||
        initial_confirmations != HS_MINER_OWNER_INITIAL_ALL ||
        policy->cleanup_operation_timeout_ms == 0 ||
        policy->cleanup_operation_timeout_ms > policy->lifecycle.drain_timeout_ms)
        return HS_MINER_EXEC_INVALID_ARGUMENT;
    if (hs_miner_lifecycle_init(&policy->lifecycle, &o.lifecycle) != HS_MINER_LIFECYCLE_OK)
        return HS_MINER_EXEC_INVALID_ARGUMENT;
    o.abi = HS_MINER_OWNER_ABI;
    o.ops = *ops;
    o.context = context;
    o.cleanup_operation_timeout_ms = policy->cleanup_operation_timeout_ms;
    o.power_off_confirmed = 1;
    *destination = o;
    return HS_MINER_EXEC_OK;
}

struct hs_miner_owner_result hs_miner_owner_step(
    struct hs_miner_owner *o, const struct hs_miner_owner_input *input)
{
    struct hs_miner_owner_input in = {0};
    enum hs_miner_owner_event event = HS_MINER_OWNER_NONE;
    enum hs_miner_owner_status status = HS_MINER_EXEC_OK;
    uint8_t ignored = 0;
    int completed = 0;
    if (o == NULL || o->abi != HS_MINER_OWNER_ABI || o->ops.begin == NULL ||
        o->ops.work_gate == NULL)
        return snapshot(NULL, HS_MINER_EXEC_INVALID_ARGUMENT, 0);
    if (o->busy) {
        latch(o, HS_MINER_EXEC_REENTRANT);
        o->work_permitted = 0;
        return snapshot(o, HS_MINER_EXEC_REENTRANT, 0);
    }
    o->busy = 1;
    if (input == NULL) {
        latch(o, HS_MINER_EXEC_INVALID_ARGUMENT);
        in.now_ms = o->last_now_ms;
    } else {
        in = *input;
        if ((unsigned)in.command > HS_MINER_COMMAND_SHUTDOWN ||
            (in.ready_flags & ~HS_MINER_READY_ALL) != 0 ||
            (unsigned)in.fault > HS_MINER_FAULT_FAN || in.ack.present > 1 ||
            (in.ack.present && ((unsigned)in.ack.operation >= HS_MINER_OP_COUNT ||
                                (unsigned)in.ack.outcome > HS_MINER_ACK_FAILURE))) {
            latch(o, HS_MINER_EXEC_INVALID_ARGUMENT);
            in.command = HS_MINER_COMMAND_NONE;
            in.ready_flags = 0;
            in.fault = HS_MINER_FAULT_NONE;
            in.ack.present = 0;
        }
        if (o->clock_started && in.now_ms < o->last_now_ms) {
            latch(o, HS_MINER_EXEC_TIME_REVERSED);
            in.now_ms = o->last_now_ms;
            in.ack.present = 0;
        }
    }
    o->clock_started = 1;
    o->last_now_ms = in.now_ms;
    if (in.fault == HS_MINER_FAULT_THERMAL || in.fault == HS_MINER_FAULT_FAN)
        latch(o, HS_MINER_EXEC_SAFETY_FAULT);
    else if (in.fault == HS_MINER_FAULT_RUNTIME)
        event = HS_MINER_OWNER_FAILED;

    if (o->fault == HS_MINER_EXEC_OK && o->lifecycle.owner_quiescent &&
        o->power_off_confirmed && !o->cleanup_active &&
        (o->lifecycle.phase == HS_MINER_STOPPED ||
         o->lifecycle.phase == HS_MINER_WAITING_HARDWARE ||
         o->lifecycle.phase == HS_MINER_BACKOFF) &&
        (in.command == HS_MINER_COMMAND_RUN ||
         (in.command == HS_MINER_COMMAND_NONE && o->lifecycle.intent == HS_MINER_COMMAND_RUN)) &&
        o->last_token > UINT64_MAX - HS_MINER_OWNER_ATTEMPT_TOKENS)
        latch(o, HS_MINER_EXEC_TOKEN_EXHAUSTED);

    for (unsigned n = 0; n < HS_MINER_OP_COUNT; ++n) {
        struct hs_miner_owned_operation *p = &o->operations[n];
        if (p->state == HS_MINER_OP_PENDING &&
            in.now_ms - p->request.since_ms >= p->request.timeout_ms) {
            p->state = HS_MINER_OP_TIMED_OUT;
            if (n == HS_MINER_OP_START) event = HS_MINER_OWNER_FAILED;
            else latch(o, HS_MINER_EXEC_OPERATION_TIMEOUT);
        }
    }
    if (o->cleanup_active &&
        in.now_ms - o->cleanup_since_ms >= o->lifecycle.policy.drain_timeout_ms)
        latch(o, HS_MINER_EXEC_CLEANUP_TIMEOUT);
    if (in.ack.present) {
        struct hs_miner_owned_operation *p = &o->operations[in.ack.operation];
        if (p->state != HS_MINER_OP_PENDING ||
            p->request.generation != in.ack.generation ||
            p->request.token != in.ack.token) {
            ignored = 1;
        } else {
            p->state = in.ack.outcome == HS_MINER_ACK_SUCCESS ?
                HS_MINER_OP_SUCCEEDED : HS_MINER_OP_FAILED;
            if (in.ack.operation == HS_MINER_OP_START) {
                if (event == HS_MINER_OWNER_NONE)
                    event = in.ack.outcome == HS_MINER_ACK_SUCCESS ?
                        HS_MINER_OWNER_READY : HS_MINER_OWNER_FAILED;
            } else if (in.ack.outcome == HS_MINER_ACK_FAILURE) {
                latch(o, HS_MINER_EXEC_OPERATION_FAILED);
            }
        }
    }

    if (event == HS_MINER_OWNER_FAILED && !o->lifecycle.owner_quiescent)
        o->attempt_failed = 1;
    record_attempt_failure(o, &in);

    if (o->cleanup_active &&
        o->operations[HS_MINER_OP_CANCEL_START].state == HS_MINER_OP_SUCCEEDED &&
        o->operations[HS_MINER_OP_DRAIN].state == HS_MINER_OP_SUCCEEDED &&
        o->operations[HS_MINER_OP_POWER_OFF].state == HS_MINER_OP_SUCCEEDED) {
        if (!o->final_drain) {
            o->final_drain = 1;
            o->operations[HS_MINER_OP_DRAIN].state = HS_MINER_OP_IDLE;
        } else if (!o->final_power_off) {
            o->final_power_off = 1;
            o->operations[HS_MINER_OP_POWER_OFF].state = HS_MINER_OP_IDLE;
        } else {
            completed = 1;
            event = HS_MINER_OWNER_DRAINED;
        }
    }

    struct hs_miner_lifecycle_result lr = transition(o, &in, event);
    if (lr.status == HS_MINER_LIFECYCLE_COMMAND_REJECTED) {
        status = HS_MINER_EXEC_COMMAND_REJECTED;

        struct hs_miner_owner_input next = in;
        next.command = HS_MINER_COMMAND_NONE;
        lr = transition(o, &next, event);
    }
    if (in.ack.present && in.ack.operation == HS_MINER_OP_START && lr.owner_event_ignored)
        ignored = 1;
    if (!lr.work_permitted || o->fault != HS_MINER_EXEC_OK) close_gate(o, 0);
    if (completed && o->lifecycle.owner_quiescent) {
        o->cleanup_active = 0;
        o->power_off_confirmed = 1;
    } else if (completed) {
        latch(o, HS_MINER_EXEC_LIFECYCLE_ERROR);
    }

    if ((lr.actions & HS_MINER_ACTION_REQUEST_START) != 0) {
        if (o->fault != HS_MINER_EXEC_OK || o->cleanup_active || !o->power_off_confirmed) {
            latch(o, HS_MINER_EXEC_LIFECYCLE_ERROR);
        } else {
            o->power_off_confirmed = 0;
            o->attempt_failed = 0;
            o->operations[HS_MINER_OP_START].state = HS_MINER_OP_IDLE;
            issue(o, HS_MINER_OP_START, in.now_ms);
            if (o->operations[HS_MINER_OP_START].state == HS_MINER_OP_FAILED) {
                o->attempt_failed = 1;

                struct hs_miner_owner_input next = in;
                next.command = HS_MINER_COMMAND_NONE;
                lr = transition(o, &next, HS_MINER_OWNER_FAILED);
            }
        }
    }
    record_attempt_failure(o, &in);
    if (o->fault != HS_MINER_EXEC_OK) {
        close_gate(o, 1);
        lr = transition(o, &in, HS_MINER_OWNER_NONE);
    }
    if (!o->lifecycle.owner_quiescent &&
        ((lr.actions & (HS_MINER_ACTION_REQUEST_DRAIN |
                        HS_MINER_ACTION_REQUEST_CANCEL_START)) != 0 ||
         o->fault != HS_MINER_EXEC_OK))
        begin_cleanup(o, in.now_ms);
    if (o->cleanup_active && o->final_drain &&
        o->operations[HS_MINER_OP_DRAIN].state == HS_MINER_OP_IDLE)
        issue(o, HS_MINER_OP_DRAIN, in.now_ms);
    if (o->cleanup_active && o->final_power_off &&
        o->operations[HS_MINER_OP_POWER_OFF].state == HS_MINER_OP_IDLE)
        issue(o, HS_MINER_OP_POWER_OFF, in.now_ms);

    if (lr.work_permitted && o->fault == HS_MINER_EXEC_OK && !o->cleanup_active &&
        o->operations[HS_MINER_OP_START].state == HS_MINER_OP_SUCCEEDED) {
        if (!o->work_permitted) {
            if (o->ops.work_gate(o->context, o->lifecycle.attempt_generation, 1) == 1 &&
                o->fault == HS_MINER_EXEC_OK)
                o->work_permitted = 1;
            else latch(o, HS_MINER_EXEC_GATE_FAILED);
        }
    } else {
        close_gate(o, 0);
    }

    if (o->fault != HS_MINER_EXEC_OK) {
        close_gate(o, 1);
        (void)transition(o, &in, HS_MINER_OWNER_NONE);
        if (!o->lifecycle.owner_quiescent) begin_cleanup(o, in.now_ms);
    }
    o->busy = 0;
    return snapshot(o, status, ignored);
}
