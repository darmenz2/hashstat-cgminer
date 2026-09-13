/* SPDX-License-Identifier: GPL-3.0-only */

#include "hs_hashminer_runtime.h"
#include "hashstat-aml88-bridge.h"

bool hs_hashminer_runtime_close_bridge(struct hs_hashminer_runtime *r)
{
    if (!r || r->self != r) return false;
    r->work_gate_closed = true;
    if (!r->bridge) return !r->bridge_failed;
    if (r->bridge->self != r->bridge || hs_aml88_bridge_stop(r->bridge) != 0)
        r->bridge_failed = true;
    return !r->bridge_failed;
}
static int gate(void *context, uint64_t generation, int open)
{
    struct hs_hashminer_runtime *r = context;
    if (!r || r->self != r || !r->owner ||
        generation != r->owner->lifecycle.attempt_generation || open) return 0;
    return hs_hashminer_runtime_close_bridge(r) ? 1 : 0;
}
static int begin(void *context, const struct hs_miner_operation_request *request)
{
    struct hs_hashminer_runtime *r = context;
    if (!r || r->self != r || !r->owner || !request ||
        (unsigned)request->operation >= HS_MINER_OP_COUNT || !request->token ||
        !request->generation || request->generation != r->owner->lifecycle.attempt_generation) return 0;
    const struct hs_miner_owned_operation *owned = &r->owner->operations[request->operation];
    if (owned->state != HS_MINER_OP_PENDING || owned->request.token != request->token ||
        owned->request.generation != request->generation ||
        owned->request.since_ms != request->since_ms ||
        owned->request.timeout_ms != request->timeout_ms) return 0;

    if (request->operation == HS_MINER_OP_START || request->operation == HS_MINER_OP_POWER_OFF)
        return 0;
    struct hs_hashminer_pending_ack *pending = &r->acknowledgments[request->operation];
    if (pending->pending) return 0;
    pending->pending = true; pending->request = *request;
    hs_hashminer_runtime_cancel(r);
    return 1;
}
static const struct hs_miner_owner_ops operations = {begin, gate};
const struct hs_miner_owner_ops *hs_hashminer_runtime_owner_ops(void) { return &operations; }

enum hs_hashminer_runtime_status hs_hashminer_runtime_bind_owner(
    struct hs_hashminer_runtime *r, struct hs_miner_owner *owner, struct hs_aml88_bridge *bridge)
{
    if (!r || r->self != r || !owner || owner->abi != HS_MINER_OWNER_ABI ||
        owner->ops.begin != begin || owner->ops.work_gate != gate || owner->context != r ||
        (r->owner && r->owner != owner) || (r->bridge && r->bridge != bridge)) return HS_HR_ARGUMENT;
    if (owner->lifecycle.phase != HS_MINER_STOPPED || !owner->lifecycle.owner_quiescent ||
        !owner->power_off_confirmed || owner->work_permitted) return HS_HR_POWER_UNVERIFIED;
    if (bridge && (bridge->self != bridge || bridge->lifecycle != &owner->lifecycle))
        return HS_HR_ARGUMENT;
    r->owner = owner; r->bridge = bridge;
    return hs_hashminer_runtime_close_bridge(r) ? HS_HR_OK : HS_HR_BRIDGE_ERROR;
}

struct hs_miner_owner_result hs_hashminer_runtime_owner_step(
    struct hs_hashminer_runtime *r, enum hs_miner_command command, enum hs_miner_owner_fault fault)
{
    struct hs_miner_owner_result error = {0};
    error.status = HS_MINER_EXEC_INVALID_ARGUMENT; error.phase = HS_MINER_FAULTED;
    if (!r || r->self != r || !r->owner) return error;
    if (command == HS_MINER_COMMAND_STOP || command == HS_MINER_COMMAND_SHUTDOWN ||
        fault != HS_MINER_FAULT_NONE) hs_hashminer_runtime_cancel(r);
    struct hs_hashminer_runtime_result process = hs_hashminer_runtime_poll(r);
    struct hs_miner_owner_input in = {0};
    in.now_ms = r->last_now_ms; in.command = command; in.fault = fault;

    if (r->fault_latched || process.status == HS_HR_REAP_UNKNOWN || r->bridge_failed)
        in.fault = HS_MINER_FAULT_RUNTIME;
    for (unsigned n = HS_MINER_OP_CANCEL_START; n <= HS_MINER_OP_DRAIN; ++n) {
        struct hs_hashminer_pending_ack *pending = &r->acknowledgments[n];
        if (!pending->pending || !process.process_quiescent) continue;
        bool ok = n == HS_MINER_OP_CANCEL_START || hs_hashminer_runtime_close_bridge(r);
        in.ack.present = 1;
        in.ack.operation = pending->request.operation;
        in.ack.generation = pending->request.generation;
        in.ack.token = pending->request.token;
        in.ack.outcome = ok ? HS_MINER_ACK_SUCCESS : HS_MINER_ACK_FAILURE;
        pending->pending = false;
        break;
    }
    return hs_miner_owner_step(r->owner, &in);
}
