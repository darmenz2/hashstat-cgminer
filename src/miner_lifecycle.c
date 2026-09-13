/* SPDX-License-Identifier: GPL-3.0-only */
#include "hs_miner_lifecycle.h"
#include "hs_span.h"
#include <stddef.h>

static int policy_valid(const struct hs_miner_lifecycle_policy *p)
{
    return p->startup_timeout_ms != 0 && p->drain_timeout_ms != 0
        && p->first_backoff_ms != 0 && p->max_backoff_ms >= p->first_backoff_ms
        && p->max_retries <= HS_MINER_LIFECYCLE_MAX_RETRIES;
}
static int state_valid(const struct hs_miner_lifecycle *s)
{
    if (s->abi != HS_MINER_LIFECYCLE_ABI || !policy_valid(&s->policy)
        || (unsigned)s->phase > HS_MINER_SHUTDOWN
        || s->intent < HS_MINER_COMMAND_RUN || s->intent > HS_MINER_COMMAND_SHUTDOWN
        || (unsigned)s->reason > HS_MINER_REASON_SUPERVISOR_ERROR
        || (unsigned)s->last_failure > HS_MINER_REASON_SUPERVISOR_ERROR
        || s->retries_reserved > s->policy.max_retries || s->clock_started > 1
        || s->owner_quiescent > 1 || s->phase_since_ms > s->last_now_ms
        || s->retry_since_ms > s->last_now_ms || s->retry_delay_ms > s->policy.max_backoff_ms)
        return 0;
    if (!s->clock_started && (s->last_now_ms != 0 || s->phase_since_ms != 0
                              || s->retry_since_ms != 0 || s->phase != HS_MINER_STOPPED))
        return 0;
    if ((s->phase == HS_MINER_STOPPED && s->intent != HS_MINER_COMMAND_STOP)
        || (s->phase == HS_MINER_SHUTDOWN && s->intent != HS_MINER_COMMAND_SHUTDOWN))
        return 0;
    if ((s->phase == HS_MINER_WAITING_HARDWARE || s->phase == HS_MINER_STARTING
         || s->phase == HS_MINER_RUNNING || s->phase == HS_MINER_BACKOFF)
        && s->intent != HS_MINER_COMMAND_RUN)
        return 0;
    if (s->phase == HS_MINER_STOPPED || s->phase == HS_MINER_WAITING_HARDWARE
        || s->phase == HS_MINER_BACKOFF || s->phase == HS_MINER_SHUTDOWN) {
        if (!s->owner_quiescent) return 0;
    }
    if (s->phase == HS_MINER_STARTING || s->phase == HS_MINER_RUNNING
        || s->phase == HS_MINER_DRAINING) {
        if (s->owner_quiescent || s->attempt_generation == 0) return 0;
    }
    if (s->retry_delay_ms != 0
        && (s->retries_reserved == 0 || (s->phase != HS_MINER_WAITING_HARDWARE
                                       && s->phase != HS_MINER_BACKOFF)))
        return 0;
    if (s->phase == HS_MINER_BACKOFF && s->retry_delay_ms == 0) return 0;
    return 1;
}
static struct hs_miner_lifecycle_result error(enum hs_miner_lifecycle_status status)
{
    struct hs_miner_lifecycle_result out = {0};
    out.status = status;
    out.phase = HS_MINER_FAULTED;
    out.actions = HS_MINER_ACTION_CLOSE_WORK | HS_MINER_ACTION_REQUEST_POWER_OFF;
    out.missing_ready_flags = HS_MINER_READY_ALL;
    return out;
}
static void enter(struct hs_miner_lifecycle *s, enum hs_miner_phase phase,
                  enum hs_miner_reason reason, uint64_t now)
{
    s->phase = phase; s->reason = reason; s->phase_since_ms = now;
}
static struct hs_miner_lifecycle_result known_error(struct hs_miner_lifecycle *state,
    struct hs_miner_lifecycle s, enum hs_miner_lifecycle_status status, int latch)
{
    struct hs_miner_lifecycle_result out = error(status);
    if (latch && s.phase != HS_MINER_SHUTDOWN) {
        s.clock_started = 1;
        s.retry_delay_ms = 0;
        s.last_failure = HS_MINER_REASON_SUPERVISOR_ERROR;
        enter(&s, HS_MINER_FAULTED, HS_MINER_REASON_SUPERVISOR_ERROR, s.last_now_ms);
        *state = s;
    }
    out.phase = s.phase; out.reason = s.reason;
    out.action_generation = s.attempt_generation;
    if (!s.owner_quiescent)
        out.actions |= HS_MINER_ACTION_REQUEST_DRAIN | HS_MINER_ACTION_REQUEST_CANCEL_START;
    return out;
}
static int failure_reason(enum hs_miner_reason reason)
{
    return reason == HS_MINER_REASON_START_FAILED
        || reason == HS_MINER_REASON_START_TIMEOUT || reason == HS_MINER_REASON_RUN_FAILED;
}
static void drain(struct hs_miner_lifecycle *s, enum hs_miner_reason reason, uint64_t now)
{
    if (failure_reason(reason)) s->last_failure = reason;
    s->retry_delay_ms = 0;
    enter(s, HS_MINER_DRAINING, reason, now);
}
static uint64_t retry_remaining(const struct hs_miner_lifecycle *s, uint64_t now)
{
    const uint64_t elapsed = now - s->retry_since_ms;
    return elapsed >= s->retry_delay_ms ? 0 : s->retry_delay_ms - elapsed;
}
static uint64_t backoff(const struct hs_miner_lifecycle *s)
{
    uint64_t delay = s->policy.first_backoff_ms;
    for (uint32_t n = 1; n < s->retries_reserved && delay < s->policy.max_backoff_ms; ++n) {
        if (delay > s->policy.max_backoff_ms - delay)
            delay = s->policy.max_backoff_ms;
        else
            delay *= 2U;
    }
    return delay;
}
static uint32_t start(struct hs_miner_lifecycle *s, uint64_t now)
{
    s->retry_delay_ms = 0;
    if (s->attempt_generation == UINT64_MAX) {
        s->last_failure = HS_MINER_REASON_GENERATION_EXHAUSTED;
        enter(s, HS_MINER_FAULTED, HS_MINER_REASON_GENERATION_EXHAUSTED, now);
        return 0;
    }
    ++s->attempt_generation;
    s->owner_quiescent = 0;
    enter(s, HS_MINER_STARTING, HS_MINER_REASON_NONE, now);
    return HS_MINER_ACTION_REQUEST_START;
}

enum hs_miner_lifecycle_status hs_miner_lifecycle_init(
    const struct hs_miner_lifecycle_policy *policy,
    struct hs_miner_lifecycle *destination)
{
    struct hs_miner_lifecycle initialized = {0};
    if (!hs_span_nonempty(policy, sizeof(*policy)) || !hs_span_nonempty(destination, sizeof(*destination)))
        return HS_MINER_LIFECYCLE_INVALID_ARGUMENT;
    if (!policy_valid(policy)) return HS_MINER_LIFECYCLE_INVALID_POLICY;
    initialized.policy = *policy;
    initialized.abi = HS_MINER_LIFECYCLE_ABI;
    initialized.phase = HS_MINER_STOPPED;
    initialized.intent = HS_MINER_COMMAND_STOP;
    initialized.owner_quiescent = 1;
    *destination = initialized;
    return HS_MINER_LIFECYCLE_OK;
}

struct hs_miner_lifecycle_result hs_miner_lifecycle_step(
    struct hs_miner_lifecycle *state, const struct hs_miner_lifecycle_input *input)
{
    struct hs_miner_lifecycle s;
    struct hs_miner_lifecycle_input in;
    struct hs_miner_lifecycle_result out = {0};
    int just_draining = 0, event_used = 0;
    if (!hs_span_nonempty(state, sizeof(*state)))
        return error(HS_MINER_LIFECYCLE_INVALID_ARGUMENT);
    s = *state;
    if (!state_valid(&s)) return error(HS_MINER_LIFECYCLE_INVALID_STATE);
    if (!hs_span_nonempty(input, sizeof(*input)))
        return known_error(state,s,HS_MINER_LIFECYCLE_INVALID_ARGUMENT,1);
    in = *input;
    if ((unsigned)in.command > HS_MINER_COMMAND_SHUTDOWN
        || (unsigned)in.owner_event > HS_MINER_OWNER_DRAINED
        || (in.ready_flags & ~HS_MINER_READY_ALL) != 0)
        return known_error(state,s,HS_MINER_LIFECYCLE_INVALID_INPUT,1);
    if (s.clock_started && in.now_ms < s.last_now_ms)
        return known_error(state,s,HS_MINER_LIFECYCLE_TIME_REVERSED,1);
    if ((s.intent == HS_MINER_COMMAND_SHUTDOWN && in.command != HS_MINER_COMMAND_NONE
         && in.command != HS_MINER_COMMAND_SHUTDOWN)
        || (s.phase == HS_MINER_FAULTED && in.command == HS_MINER_COMMAND_RUN))
        return known_error(state,s,HS_MINER_LIFECYCLE_COMMAND_REJECTED,0);
    s.clock_started = 1; s.last_now_ms = in.now_ms;

    if (in.command == HS_MINER_COMMAND_STOP || in.command == HS_MINER_COMMAND_SHUTDOWN) {
        s.intent = in.command;
        s.retry_delay_ms = 0;
        if (s.owner_quiescent) {
            enter(&s, s.intent == HS_MINER_COMMAND_STOP ? HS_MINER_STOPPED : HS_MINER_SHUTDOWN,
                  s.intent == HS_MINER_COMMAND_STOP ? HS_MINER_REASON_USER_STOP : HS_MINER_REASON_SHUTDOWN,
                  in.now_ms);
        } else if (s.phase != HS_MINER_DRAINING && s.phase != HS_MINER_FAULTED) {
            drain(&s, s.intent == HS_MINER_COMMAND_STOP ? HS_MINER_REASON_USER_STOP
                                                      : HS_MINER_REASON_SHUTDOWN, in.now_ms);
            just_draining = 1;
        }
    } else if (in.command == HS_MINER_COMMAND_RUN) {
        if (s.phase == HS_MINER_STOPPED) {
            s.retries_reserved = 0; s.last_failure = HS_MINER_REASON_NONE;
            enter(&s, HS_MINER_WAITING_HARDWARE, HS_MINER_REASON_HARDWARE_MISSING, in.now_ms);
        }
        s.intent = HS_MINER_COMMAND_RUN;
    }

    const int matched = in.owner_event != HS_MINER_OWNER_NONE
                     && in.owner_generation == s.attempt_generation;
    const int ready = in.ready_flags == HS_MINER_READY_ALL;
    switch (s.phase) {
    case HS_MINER_STOPPED:
    case HS_MINER_SHUTDOWN:
        break;
    case HS_MINER_WAITING_HARDWARE:
    case HS_MINER_BACKOFF:
        if (!ready) {
            if (s.phase != HS_MINER_WAITING_HARDWARE)
                enter(&s, HS_MINER_WAITING_HARDWARE, HS_MINER_REASON_HARDWARE_MISSING, in.now_ms);
        } else if (s.retry_delay_ms != 0 && retry_remaining(&s, in.now_ms) != 0) {
            if (s.phase != HS_MINER_BACKOFF)
                enter(&s, HS_MINER_BACKOFF, s.last_failure, in.now_ms);
        } else {
            out.actions |= start(&s, in.now_ms);
        }
        break;
    case HS_MINER_STARTING:
        if (!ready) {
            drain(&s, HS_MINER_REASON_HARDWARE_LOST, in.now_ms);
        } else if (in.now_ms - s.phase_since_ms >= s.policy.startup_timeout_ms) {
            drain(&s, HS_MINER_REASON_START_TIMEOUT, in.now_ms);
        } else if (matched && (in.owner_event == HS_MINER_OWNER_FAILED
                              || in.owner_event == HS_MINER_OWNER_DRAINED)) {
            drain(&s, HS_MINER_REASON_START_FAILED, in.now_ms); event_used = 1;
        } else if (matched && in.owner_event == HS_MINER_OWNER_READY) {
            enter(&s, HS_MINER_RUNNING, HS_MINER_REASON_NONE, in.now_ms); event_used = 1;
        }
        break;
    case HS_MINER_RUNNING:
        if (!ready) {
            drain(&s, HS_MINER_REASON_HARDWARE_LOST, in.now_ms);
        } else if (matched && (in.owner_event == HS_MINER_OWNER_FAILED
                              || in.owner_event == HS_MINER_OWNER_DRAINED)) {
            drain(&s, HS_MINER_REASON_RUN_FAILED, in.now_ms); event_used = 1;
        }
        break;
    case HS_MINER_DRAINING:
        if (matched && in.owner_event == HS_MINER_OWNER_FAILED) {
            s.reason = HS_MINER_REASON_RUN_FAILED;
            s.last_failure = HS_MINER_REASON_RUN_FAILED; event_used = 1;
        }
        if (!just_draining && matched && in.owner_event == HS_MINER_OWNER_DRAINED) {
            s.owner_quiescent = 1; event_used = 1;
            if (s.intent == HS_MINER_COMMAND_SHUTDOWN) {
                enter(&s, HS_MINER_SHUTDOWN, HS_MINER_REASON_SHUTDOWN, in.now_ms);
            } else if (s.intent == HS_MINER_COMMAND_STOP) {
                enter(&s, HS_MINER_STOPPED, HS_MINER_REASON_USER_STOP, in.now_ms);
            } else if (failure_reason(s.reason)) {
                if (s.retries_reserved >= s.policy.max_retries) {
                    enter(&s, HS_MINER_FAULTED, HS_MINER_REASON_RETRY_LIMIT, in.now_ms);
                } else {
                    ++s.retries_reserved;
                    s.retry_since_ms = in.now_ms; s.retry_delay_ms = backoff(&s);
                    enter(&s, ready ? HS_MINER_BACKOFF : HS_MINER_WAITING_HARDWARE,
                          ready ? s.last_failure : HS_MINER_REASON_HARDWARE_MISSING, in.now_ms);
                }
            } else {
                enter(&s, HS_MINER_WAITING_HARDWARE, HS_MINER_REASON_HARDWARE_MISSING, in.now_ms);
            }
        } else if (in.now_ms - s.phase_since_ms >= s.policy.drain_timeout_ms) {
            s.last_failure = HS_MINER_REASON_DRAIN_TIMEOUT;
            enter(&s, HS_MINER_FAULTED, HS_MINER_REASON_DRAIN_TIMEOUT, in.now_ms);
        }
        break;
    case HS_MINER_FAULTED:
        if (matched && in.owner_event == HS_MINER_OWNER_DRAINED) {
            s.owner_quiescent = 1; event_used = 1;
            if (s.intent == HS_MINER_COMMAND_STOP)
                enter(&s, HS_MINER_STOPPED, HS_MINER_REASON_USER_STOP, in.now_ms);
            else if (s.intent == HS_MINER_COMMAND_SHUTDOWN)
                enter(&s, HS_MINER_SHUTDOWN, HS_MINER_REASON_SHUTDOWN, in.now_ms);
        }
        break;
    }

    out.status = HS_MINER_LIFECYCLE_OK;
    out.phase = s.phase; out.reason = s.reason;
    out.action_generation = s.attempt_generation;
    out.missing_ready_flags = HS_MINER_READY_ALL & ~in.ready_flags;
    out.retry_remaining_ms = retry_remaining(&s, in.now_ms);
    out.owner_event_ignored = (uint8_t)(in.owner_event != HS_MINER_OWNER_NONE && !event_used);
    out.work_permitted = (uint8_t)(s.phase == HS_MINER_RUNNING && ready);
    if (out.work_permitted) {
        out.actions |= HS_MINER_ACTION_OPEN_WORK;
    } else {
        out.actions |= HS_MINER_ACTION_CLOSE_WORK;
        if (s.phase == HS_MINER_DRAINING) {
            out.actions |= HS_MINER_ACTION_REQUEST_DRAIN | HS_MINER_ACTION_REQUEST_CANCEL_START;
            if (!ready || failure_reason(s.reason) || s.reason == HS_MINER_REASON_HARDWARE_LOST)
                out.actions |= HS_MINER_ACTION_REQUEST_POWER_OFF;
        } else if (s.phase != HS_MINER_STARTING) {
            out.actions |= HS_MINER_ACTION_REQUEST_POWER_OFF;
            if (s.phase == HS_MINER_FAULTED && !s.owner_quiescent)
                out.actions |= HS_MINER_ACTION_REQUEST_DRAIN | HS_MINER_ACTION_REQUEST_CANCEL_START;
        }
    }
    *state = s;
    return out;
}
