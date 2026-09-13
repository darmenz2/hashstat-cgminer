/* SPDX-License-Identifier: GPL-3.0-only */
#include "hs_miner_lifecycle.h"
#include <stdio.h>
#include <string.h>

static unsigned long checks;
#define CHECK(c) do { ++checks; if (!(c)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; \
} } while (0)

static const struct hs_miner_lifecycle_policy policy = {100,20,10,25,3};
static struct hs_miner_lifecycle_result tick(struct hs_miner_lifecycle *s,
    uint64_t now, enum hs_miner_command command, uint32_t flags,
    enum hs_miner_owner_event event, uint64_t generation)
{
    const struct hs_miner_lifecycle_input in = {now,command,flags,event,generation};
    return hs_miner_lifecycle_step(s, &in);
}
static int make_phase(struct hs_miner_lifecycle *s, enum hs_miner_phase phase)
{
    struct hs_miner_lifecycle_result out;
    CHECK(hs_miner_lifecycle_init(&policy, s) == HS_MINER_LIFECYCLE_OK);
    if (phase == HS_MINER_STOPPED) return 0;
    if (phase == HS_MINER_SHUTDOWN) {
        out = tick(s,10,HS_MINER_COMMAND_SHUTDOWN,31,HS_MINER_OWNER_NONE,0);
        CHECK(out.phase == phase); return 0;
    }
    out = tick(s,10,HS_MINER_COMMAND_RUN,
               phase == HS_MINER_WAITING_HARDWARE ? 0 : 31,HS_MINER_OWNER_NONE,0);
    if (phase == HS_MINER_WAITING_HARDWARE || phase == HS_MINER_STARTING) {
        CHECK(out.phase == phase); return 0;
    }
    out = tick(s,11,HS_MINER_COMMAND_NONE,31,HS_MINER_OWNER_READY,1);
    CHECK(out.phase == HS_MINER_RUNNING && out.work_permitted);
    if (phase == HS_MINER_RUNNING) return 0;
    out = tick(s,12,HS_MINER_COMMAND_NONE,31,HS_MINER_OWNER_FAILED,1);
    CHECK(out.phase == HS_MINER_DRAINING);
    if (phase == HS_MINER_DRAINING) return 0;
    if (phase == HS_MINER_FAULTED) {
        out = tick(s,32,HS_MINER_COMMAND_NONE,31,HS_MINER_OWNER_NONE,1);
        CHECK(out.phase == phase); return 0;
    }
    out = tick(s,13,HS_MINER_COMMAND_NONE,31,HS_MINER_OWNER_DRAINED,1);
    CHECK(out.phase == HS_MINER_BACKOFF && out.retry_remaining_ms == 10);
    return 0;
}

int main(void)
{
    struct hs_miner_lifecycle s, saved, base;
    struct hs_miner_lifecycle_policy p;
    struct hs_miner_lifecycle_result out;
    struct hs_miner_lifecycle_input in = {0};
    uint64_t now, generation;
    unsigned phase, command, flags, event, stale, count;

    memset(&s,0x5a,sizeof(s)); saved = s;
    CHECK(hs_miner_lifecycle_init(NULL,&s) == HS_MINER_LIFECYCLE_INVALID_ARGUMENT);
    CHECK(memcmp(&s,&saved,sizeof(s)) == 0);
    CHECK(hs_miner_lifecycle_init(&policy,NULL) == HS_MINER_LIFECYCLE_INVALID_ARGUMENT);
    CHECK(hs_miner_lifecycle_init((void *)(UINTPTR_MAX-3),&s) == HS_MINER_LIFECYCLE_INVALID_ARGUMENT);
    for (count = 0; count < 6; ++count) {
        p = policy;
        if (count == 0) p.startup_timeout_ms = 0;
        if (count == 1) p.drain_timeout_ms = 0;
        if (count == 2) p.first_backoff_ms = 0;
        if (count == 3) p.max_backoff_ms = 9;
        if (count == 4) p.max_retries = 33;
        if (count == 5) p.max_retries = UINT32_MAX;
        CHECK(hs_miner_lifecycle_init(&p,&s) == HS_MINER_LIFECYCLE_INVALID_POLICY);
        CHECK(memcmp(&s,&saved,sizeof(s)) == 0);
    }
    CHECK(hs_miner_lifecycle_init(&policy,&s) == HS_MINER_LIFECYCLE_OK);
    CHECK(s.phase == HS_MINER_STOPPED && s.intent == HS_MINER_COMMAND_STOP);
    CHECK(s.attempt_generation == 0 && s.owner_quiescent);

    for (flags = 0; flags < 32; ++flags) {
        out = tick(&s,flags,HS_MINER_COMMAND_NONE,flags,HS_MINER_OWNER_READY,0);
        CHECK(out.phase == HS_MINER_STOPPED && !out.work_permitted);
        CHECK(!(out.actions & HS_MINER_ACTION_REQUEST_START));
    }

    for (flags = 0; flags < 31; ++flags) {
        CHECK(hs_miner_lifecycle_init(&policy,&s) == HS_MINER_LIFECYCLE_OK);
        out = tick(&s,0,HS_MINER_COMMAND_RUN,flags,HS_MINER_OWNER_READY,1);
        CHECK(out.phase == HS_MINER_WAITING_HARDWARE && !out.work_permitted);
        CHECK(!(out.actions & HS_MINER_ACTION_REQUEST_START));
        CHECK(out.missing_ready_flags == (31U ^ flags));
        CHECK(s.attempt_generation == 0);
    }
    CHECK(make_phase(&s,HS_MINER_WAITING_HARDWARE) == 0);
    out = tick(&s,11,HS_MINER_COMMAND_NONE,31,HS_MINER_OWNER_READY,1);
    CHECK(out.phase == HS_MINER_STARTING && !out.work_permitted);
    CHECK(out.actions & HS_MINER_ACTION_REQUEST_START);
    CHECK(out.owner_event_ignored && out.action_generation == 1);
    out = tick(&s,12,HS_MINER_COMMAND_NONE,31,HS_MINER_OWNER_READY,0);
    CHECK(out.phase == HS_MINER_STARTING && out.owner_event_ignored);
    out = tick(&s,13,HS_MINER_COMMAND_NONE,31,HS_MINER_OWNER_READY,1);
    CHECK(out.phase == HS_MINER_RUNNING && out.work_permitted);
    out = tick(&s,14,HS_MINER_COMMAND_STOP,31,HS_MINER_OWNER_READY,1);
    CHECK(out.phase == HS_MINER_DRAINING && !out.work_permitted);
    CHECK((out.actions & (HS_MINER_ACTION_CLOSE_WORK|HS_MINER_ACTION_REQUEST_DRAIN)) ==
          (HS_MINER_ACTION_CLOSE_WORK|HS_MINER_ACTION_REQUEST_DRAIN));
    CHECK(out.owner_event_ignored);
    out = tick(&s,15,HS_MINER_COMMAND_NONE,31,HS_MINER_OWNER_DRAINED,1);
    CHECK(out.phase == HS_MINER_STOPPED && s.owner_quiescent);
    for (now = 16; now < 116; ++now) {
        out = tick(&s,now,HS_MINER_COMMAND_NONE,31,HS_MINER_OWNER_READY,1);
        CHECK(out.phase == HS_MINER_STOPPED && !out.work_permitted);
        CHECK(!(out.actions & HS_MINER_ACTION_REQUEST_START));
    }

    for (flags = 0; flags < 31; ++flags) {
        CHECK(make_phase(&s,HS_MINER_RUNNING) == 0);
        out = tick(&s,12,HS_MINER_COMMAND_NONE,flags,HS_MINER_OWNER_READY,1);
        CHECK(out.phase == HS_MINER_DRAINING && !out.work_permitted);
        CHECK(out.actions & HS_MINER_ACTION_REQUEST_POWER_OFF);
        out = tick(&s,13,HS_MINER_COMMAND_NONE,flags,HS_MINER_OWNER_DRAINED,1);
        CHECK(out.phase == HS_MINER_WAITING_HARDWARE && !out.work_permitted);
        CHECK(s.retries_reserved == 0);
    }

    CHECK(make_phase(&s,HS_MINER_RUNNING) == 0);
    out = tick(&s,12,HS_MINER_COMMAND_NONE,31,HS_MINER_OWNER_DRAINED,1);
    CHECK(out.phase == HS_MINER_DRAINING && out.reason == HS_MINER_REASON_RUN_FAILED);

    CHECK(make_phase(&s,HS_MINER_STARTING) == 0);
    out = tick(&s,109,HS_MINER_COMMAND_NONE,31,HS_MINER_OWNER_NONE,1);
    CHECK(out.phase == HS_MINER_STARTING);
    out = tick(&s,110,HS_MINER_COMMAND_NONE,31,HS_MINER_OWNER_READY,1);
    CHECK(out.phase == HS_MINER_DRAINING && out.reason == HS_MINER_REASON_START_TIMEOUT);
    CHECK(out.actions & HS_MINER_ACTION_REQUEST_POWER_OFF);
    out = tick(&s,130,HS_MINER_COMMAND_NONE,31,HS_MINER_OWNER_NONE,1);
    CHECK(out.phase == HS_MINER_FAULTED && out.reason == HS_MINER_REASON_DRAIN_TIMEOUT);
    saved = s;
    out = tick(&s,131,HS_MINER_COMMAND_RUN,31,HS_MINER_OWNER_READY,1);
    CHECK(out.status == HS_MINER_LIFECYCLE_COMMAND_REJECTED && !out.work_permitted);
    CHECK(memcmp(&s,&saved,sizeof(s)) == 0);
    out = tick(&s,131,HS_MINER_COMMAND_STOP,31,HS_MINER_OWNER_NONE,1);
    CHECK(out.phase == HS_MINER_FAULTED);
    out = tick(&s,132,HS_MINER_COMMAND_NONE,31,HS_MINER_OWNER_DRAINED,1);
    CHECK(out.phase == HS_MINER_STOPPED);
    out = tick(&s,133,HS_MINER_COMMAND_RUN,31,HS_MINER_OWNER_NONE,1);
    CHECK(out.phase == HS_MINER_STARTING && out.action_generation == 2);

    CHECK(make_phase(&s,HS_MINER_RUNNING) == 0); now = 12;
    for (count = 0; count < 4; ++count) {
        generation = s.attempt_generation;
        out = tick(&s,now++,HS_MINER_COMMAND_RUN,31,HS_MINER_OWNER_FAILED,generation);
        CHECK(out.phase == HS_MINER_DRAINING && !out.work_permitted);
        out = tick(&s,now++,HS_MINER_COMMAND_NONE,31,HS_MINER_OWNER_DRAINED,generation);
        if (count == 3) {
            CHECK(out.phase == HS_MINER_FAULTED && out.reason == HS_MINER_REASON_RETRY_LIMIT);
            break;
        }
        const uint64_t expected_delay = count == 0 ? 10 : count == 1 ? 20 : 25;
        CHECK(out.phase == HS_MINER_BACKOFF && out.retry_remaining_ms == expected_delay);
        CHECK(s.retries_reserved == count + 1);

        out = tick(&s,now,HS_MINER_COMMAND_RUN,0,HS_MINER_OWNER_READY,generation);
        CHECK(out.phase == HS_MINER_WAITING_HARDWARE && !out.work_permitted);
        const uint64_t start_time = s.retry_since_ms + expected_delay;
        out = tick(&s,start_time-1,HS_MINER_COMMAND_RUN,31,HS_MINER_OWNER_READY,generation);
        CHECK(out.phase == HS_MINER_BACKOFF && out.retry_remaining_ms == 1);
        out = tick(&s,start_time,HS_MINER_COMMAND_RUN,31,HS_MINER_OWNER_READY,generation);
        CHECK(out.phase == HS_MINER_STARTING && out.action_generation == generation + 1);
        CHECK(!out.work_permitted && out.owner_event_ignored);
        out = tick(&s,start_time+1,HS_MINER_COMMAND_NONE,31,HS_MINER_OWNER_READY,generation+1);
        CHECK(out.phase == HS_MINER_RUNNING && out.work_permitted);
        now = start_time+2;
    }
    CHECK(s.retries_reserved == 3 && s.attempt_generation == 4);
    out = tick(&s,now,HS_MINER_COMMAND_NONE,31,HS_MINER_OWNER_READY,4);
    CHECK(out.phase == HS_MINER_FAULTED && !out.work_permitted);

    CHECK(make_phase(&s,HS_MINER_BACKOFF) == 0);
    out = tick(&s,14,HS_MINER_COMMAND_STOP,31,HS_MINER_OWNER_NONE,1);
    CHECK(out.phase == HS_MINER_STOPPED && s.retry_delay_ms == 0);
    out = tick(&s,UINT64_MAX,HS_MINER_COMMAND_NONE,31,HS_MINER_OWNER_READY,1);
    CHECK(out.phase == HS_MINER_STOPPED && !out.work_permitted);

    CHECK(make_phase(&s,HS_MINER_RUNNING) == 0);
    out = tick(&s,12,HS_MINER_COMMAND_STOP,31,HS_MINER_OWNER_NONE,1);
    for (now = 13; now < 32; ++now) {
        out = tick(&s,now,HS_MINER_COMMAND_STOP,31,HS_MINER_OWNER_FAILED,1);
        CHECK(out.phase == HS_MINER_DRAINING && s.phase_since_ms == 12);
        CHECK(out.actions & HS_MINER_ACTION_REQUEST_POWER_OFF);
    }
    out = tick(&s,32,HS_MINER_COMMAND_STOP,31,HS_MINER_OWNER_FAILED,1);
    CHECK(out.phase == HS_MINER_FAULTED && !out.work_permitted);

    CHECK(make_phase(&s,HS_MINER_RUNNING) == 0);
    out = tick(&s,12,HS_MINER_COMMAND_SHUTDOWN,31,HS_MINER_OWNER_READY,1);
    CHECK(out.phase == HS_MINER_DRAINING && !out.work_permitted);
    out = tick(&s,13,HS_MINER_COMMAND_RUN,31,HS_MINER_OWNER_READY,1);
    CHECK(out.status == HS_MINER_LIFECYCLE_COMMAND_REJECTED && !out.work_permitted);
    out = tick(&s,13,HS_MINER_COMMAND_NONE,31,HS_MINER_OWNER_DRAINED,1);
    CHECK(out.phase == HS_MINER_SHUTDOWN);
    out = tick(&s,14,HS_MINER_COMMAND_NONE,31,HS_MINER_OWNER_READY,1);
    CHECK(out.phase == HS_MINER_SHUTDOWN && !out.work_permitted);

    p = policy; p.startup_timeout_ms = UINT64_MAX; p.drain_timeout_ms = UINT64_MAX;
    p.first_backoff_ms = UINT64_MAX-1; p.max_backoff_ms = UINT64_MAX;
    CHECK(hs_miner_lifecycle_init(&p,&s) == HS_MINER_LIFECYCLE_OK);
    out = tick(&s,UINT64_MAX-10,HS_MINER_COMMAND_RUN,31,HS_MINER_OWNER_NONE,0);
    CHECK(out.phase == HS_MINER_STARTING);
    out = tick(&s,UINT64_MAX-9,HS_MINER_COMMAND_NONE,31,HS_MINER_OWNER_FAILED,1);
    CHECK(out.phase == HS_MINER_DRAINING);
    out = tick(&s,UINT64_MAX-8,HS_MINER_COMMAND_NONE,31,HS_MINER_OWNER_DRAINED,1);
    CHECK(out.phase == HS_MINER_BACKOFF && out.retry_remaining_ms == UINT64_MAX-1);
    out = tick(&s,UINT64_MAX,HS_MINER_COMMAND_NONE,31,HS_MINER_OWNER_NONE,1);
    CHECK(out.phase == HS_MINER_BACKOFF && out.retry_remaining_ms == UINT64_MAX-9);
    saved = s;
    out = tick(&s,0,HS_MINER_COMMAND_NONE,31,HS_MINER_OWNER_READY,1);
    CHECK(out.status == HS_MINER_LIFECYCLE_TIME_REVERSED && !out.work_permitted);
    CHECK(s.phase == HS_MINER_FAULTED && s.reason == HS_MINER_REASON_SUPERVISOR_ERROR);
    out = tick(&s,UINT64_MAX,HS_MINER_COMMAND_NONE,31,HS_MINER_OWNER_READY,1);
    CHECK(out.phase == HS_MINER_FAULTED && !out.work_permitted);
    CHECK(hs_miner_lifecycle_init(&policy,&s) == HS_MINER_LIFECYCLE_OK);
    s.attempt_generation = UINT64_MAX;
    out = tick(&s,0,HS_MINER_COMMAND_RUN,31,HS_MINER_OWNER_NONE,0);
    CHECK(out.phase == HS_MINER_FAULTED && out.reason == HS_MINER_REASON_GENERATION_EXHAUSTED);
    CHECK(s.attempt_generation == UINT64_MAX && !(out.actions & HS_MINER_ACTION_REQUEST_START));

    p = policy; p.max_retries = 0;
    CHECK(hs_miner_lifecycle_init(&p,&s) == HS_MINER_LIFECYCLE_OK);
    out = tick(&s,0,HS_MINER_COMMAND_RUN,31,HS_MINER_OWNER_NONE,0);
    out = tick(&s,1,HS_MINER_COMMAND_NONE,31,HS_MINER_OWNER_FAILED,1);
    out = tick(&s,2,HS_MINER_COMMAND_NONE,31,HS_MINER_OWNER_DRAINED,1);
    CHECK(out.phase == HS_MINER_FAULTED && out.reason == HS_MINER_REASON_RETRY_LIMIT);
    CHECK(s.retries_reserved == 0 && s.attempt_generation == 1);

    p = policy; p.first_backoff_ms = UINT64_MAX/2+1; p.max_backoff_ms = UINT64_MAX;
    CHECK(hs_miner_lifecycle_init(&p,&s) == HS_MINER_LIFECYCLE_OK);
    out = tick(&s,0,HS_MINER_COMMAND_RUN,31,HS_MINER_OWNER_NONE,0);
    out = tick(&s,0,HS_MINER_COMMAND_NONE,31,HS_MINER_OWNER_FAILED,1);
    out = tick(&s,0,HS_MINER_COMMAND_NONE,31,HS_MINER_OWNER_DRAINED,1);
    CHECK(out.retry_remaining_ms == p.first_backoff_ms);
    now = p.first_backoff_ms;
    out = tick(&s,now,HS_MINER_COMMAND_NONE,31,HS_MINER_OWNER_NONE,1);
    CHECK(out.phase == HS_MINER_STARTING && out.action_generation == 2);
    out = tick(&s,now,HS_MINER_COMMAND_NONE,31,HS_MINER_OWNER_FAILED,2);
    out = tick(&s,now,HS_MINER_COMMAND_NONE,31,HS_MINER_OWNER_DRAINED,2);
    CHECK(out.retry_remaining_ms == UINT64_MAX && s.retries_reserved == 2);
    out = tick(&s,UINT64_MAX,HS_MINER_COMMAND_NONE,31,HS_MINER_OWNER_NONE,2);
    CHECK(out.phase == HS_MINER_BACKOFF && out.retry_remaining_ms == now);

    for (phase = 0; phase <= HS_MINER_SHUTDOWN; ++phase) {
        CHECK(make_phase(&base,(enum hs_miner_phase)phase) == 0);
        for (command = 0; command <= HS_MINER_COMMAND_SHUTDOWN; ++command)
        for (flags = 0; flags < 32; ++flags)
        for (event = 0; event <= HS_MINER_OWNER_DRAINED; ++event)
        for (stale = 0; stale < 2; ++stale) {
            s = base; saved = s; now = s.last_now_ms+1;
            generation = s.attempt_generation;
            if (stale) generation = generation == 0 ? 1 : generation-1;
            out = tick(&s,now,(enum hs_miner_command)command,flags,
                       (enum hs_miner_owner_event)event,generation);
            CHECK(out.status == HS_MINER_LIFECYCLE_OK ||
                  out.status == HS_MINER_LIFECYCLE_COMMAND_REJECTED);
            CHECK(!out.work_permitted || (out.phase == HS_MINER_RUNNING && flags == 31));
            CHECK(!((out.actions & HS_MINER_ACTION_OPEN_WORK) &&
                    (out.actions & HS_MINER_ACTION_CLOSE_WORK)));
            if (out.actions & HS_MINER_ACTION_REQUEST_START) {
                CHECK(out.phase == HS_MINER_STARTING && flags == 31 && !out.work_permitted);
                CHECK(out.action_generation == saved.attempt_generation+1);
            }
            if (out.status != HS_MINER_LIFECYCLE_OK)
                CHECK(memcmp(&s,&saved,sizeof(s)) == 0);
            out = tick(&s,now+1,HS_MINER_COMMAND_NONE,flags,HS_MINER_OWNER_NONE,0);
            CHECK(out.status == HS_MINER_LIFECYCLE_OK);
        }
    }

    CHECK(make_phase(&s,HS_MINER_RUNNING) == 0); saved = s;
    in.now_ms = 11; in.ready_flags = 31; in.command = (enum hs_miner_command)-1;
    out = hs_miner_lifecycle_step(&s,&in);
    CHECK(out.status == HS_MINER_LIFECYCLE_INVALID_INPUT && !out.work_permitted);
    CHECK(s.phase == HS_MINER_FAULTED && s.reason == HS_MINER_REASON_SUPERVISOR_ERROR);
    CHECK(out.action_generation == 1 && (out.actions & HS_MINER_ACTION_REQUEST_DRAIN));
    out = tick(&s,12,HS_MINER_COMMAND_NONE,31,HS_MINER_OWNER_READY,1);
    CHECK(out.phase == HS_MINER_FAULTED && !out.work_permitted);
    out = tick(&s,13,HS_MINER_COMMAND_NONE,31,HS_MINER_OWNER_DRAINED,1);
    CHECK(out.phase == HS_MINER_FAULTED && s.owner_quiescent);
    out = tick(&s,14,HS_MINER_COMMAND_RUN,31,HS_MINER_OWNER_NONE,1);
    CHECK(out.status == HS_MINER_LIFECYCLE_COMMAND_REJECTED && !out.work_permitted);
    out = tick(&s,14,HS_MINER_COMMAND_STOP,31,HS_MINER_OWNER_NONE,1);
    CHECK(out.phase == HS_MINER_STOPPED && !out.work_permitted);
    out = tick(&s,15,HS_MINER_COMMAND_NONE,31,HS_MINER_OWNER_READY,1);
    CHECK(out.phase == HS_MINER_STOPPED && !out.work_permitted);
    out = tick(&s,16,HS_MINER_COMMAND_RUN,31,HS_MINER_OWNER_NONE,1);
    CHECK(out.phase == HS_MINER_STARTING && out.action_generation == 2);
    CHECK(make_phase(&s,HS_MINER_RUNNING) == 0);
    in.command = HS_MINER_COMMAND_NONE; in.ready_flags = 32;
    out = hs_miner_lifecycle_step(&s,&in);
    CHECK(out.status == HS_MINER_LIFECYCLE_INVALID_INPUT);
    CHECK(s.phase == HS_MINER_FAULTED);
    CHECK(make_phase(&s,HS_MINER_RUNNING) == 0);
    in.ready_flags = 31; in.owner_event = (enum hs_miner_owner_event)99;
    out = hs_miner_lifecycle_step(&s,&in);
    CHECK(out.status == HS_MINER_LIFECYCLE_INVALID_INPUT);
    CHECK(s.phase == HS_MINER_FAULTED);
    in.owner_event = HS_MINER_OWNER_NONE;
    s.abi ^= 1; saved = s;
    out = hs_miner_lifecycle_step(&s,&in);
    CHECK(out.status == HS_MINER_LIFECYCLE_INVALID_STATE && !out.work_permitted);
    CHECK(memcmp(&s,&saved,sizeof(s)) == 0);
    out = hs_miner_lifecycle_step(NULL,&in);
    CHECK(out.status == HS_MINER_LIFECYCLE_INVALID_ARGUMENT && !out.work_permitted);
    CHECK(make_phase(&s,HS_MINER_RUNNING) == 0);
    out = hs_miner_lifecycle_step(&s,(void *)(UINTPTR_MAX-3));
    CHECK(out.status == HS_MINER_LIFECYCLE_INVALID_ARGUMENT && !out.work_permitted);
    CHECK(s.phase == HS_MINER_FAULTED);
    printf("PASS lifecycle policy: %lu checks; no I/O or healthy-mining claim\n",checks);
    return 0;
}
