/* SPDX-License-Identifier: GPL-3.0-only */
/* Asynchronous lifecycle executor. */
#ifndef HS_MINER_OWNER_H
#define HS_MINER_OWNER_H

#include "hs_miner_lifecycle.h"

#define HS_MINER_OWNER_ABI UINT32_C(0x484d4f31)
#define HS_MINER_OWNER_INITIAL_QUIESCENT UINT32_C(1)
#define HS_MINER_OWNER_INITIAL_POWER_OFF UINT32_C(2)
#define HS_MINER_OWNER_INITIAL_WORK_CLOSED UINT32_C(4)
#define HS_MINER_OWNER_INITIAL_ALL UINT32_C(7)

#define HS_MINER_OWNER_ATTEMPT_TOKENS UINT64_C(6)

enum hs_miner_operation {
    HS_MINER_OP_START = 0,
    HS_MINER_OP_CANCEL_START,
    HS_MINER_OP_DRAIN,
    HS_MINER_OP_POWER_OFF,
    HS_MINER_OP_COUNT
};
enum hs_miner_operation_state {
    HS_MINER_OP_IDLE = 0,
    HS_MINER_OP_PENDING,
    HS_MINER_OP_SUCCEEDED,
    HS_MINER_OP_FAILED,
    HS_MINER_OP_TIMED_OUT,
    HS_MINER_OP_RETIRED
};
enum hs_miner_owner_status {
    HS_MINER_EXEC_OK = 0,
    HS_MINER_EXEC_INVALID_ARGUMENT,
    HS_MINER_EXEC_REENTRANT,
    HS_MINER_EXEC_TIME_REVERSED,
    HS_MINER_EXEC_SAFETY_FAULT,
    HS_MINER_EXEC_GATE_FAILED,
    HS_MINER_EXEC_OPERATION_REJECTED,
    HS_MINER_EXEC_OPERATION_FAILED,
    HS_MINER_EXEC_OPERATION_TIMEOUT,
    HS_MINER_EXEC_CLEANUP_TIMEOUT,
    HS_MINER_EXEC_TOKEN_EXHAUSTED,
    HS_MINER_EXEC_LIFECYCLE_ERROR,
    HS_MINER_EXEC_COMMAND_REJECTED
};
enum hs_miner_owner_fault {
    HS_MINER_FAULT_NONE = 0,
    HS_MINER_FAULT_RUNTIME,
    HS_MINER_FAULT_THERMAL,
    HS_MINER_FAULT_FAN
};
enum hs_miner_ack_outcome {
    HS_MINER_ACK_SUCCESS = 0,
    HS_MINER_ACK_FAILURE
};
struct hs_miner_operation_request {
    enum hs_miner_operation operation;
    uint64_t generation;
    uint64_t token;
    uint64_t since_ms;
    uint64_t timeout_ms;
};
struct hs_miner_operation_ack {
    uint8_t present;
    enum hs_miner_operation operation;
    enum hs_miner_ack_outcome outcome;
    uint64_t generation;
    uint64_t token;
};
struct hs_miner_owner_ops {
    int (*begin)(void *context, const struct hs_miner_operation_request *request);

    int (*work_gate)(void *context, uint64_t generation, int open);
};
struct hs_miner_owner_policy {
    struct hs_miner_lifecycle_policy lifecycle;

    uint64_t cleanup_operation_timeout_ms;
};
struct hs_miner_owned_operation {
    struct hs_miner_operation_request request;
    enum hs_miner_operation_state state;
};
struct hs_miner_owner {
    uint32_t abi;
    struct hs_miner_lifecycle lifecycle;
    struct hs_miner_owner_ops ops;
    void *context;
    struct hs_miner_owned_operation operations[HS_MINER_OP_COUNT];
    uint64_t cleanup_operation_timeout_ms;
    uint64_t last_token;
    uint64_t last_now_ms;
    uint64_t cleanup_since_ms;
    enum hs_miner_owner_status fault;
    uint8_t clock_started;
    uint8_t busy;
    uint8_t work_permitted;
    uint8_t cleanup_active;
    uint8_t final_drain;
    uint8_t final_power_off;
    uint8_t power_off_confirmed;
    uint8_t attempt_failed;
};
struct hs_miner_owner_input {
    uint64_t now_ms;
    enum hs_miner_command command;
    uint32_t ready_flags;
    enum hs_miner_owner_fault fault;
    struct hs_miner_operation_ack ack;
};
struct hs_miner_owner_result {
    enum hs_miner_owner_status status;
    enum hs_miner_owner_status latched_fault;
    enum hs_miner_phase phase;
    enum hs_miner_reason reason;
    uint64_t generation;
    uint32_t retries_reserved;
    uint8_t work_permitted;
    uint8_t cleanup_active;
    uint8_t power_off_confirmed;
    uint8_t quiescent_confirmed;
    uint8_t ack_ignored;
};

enum hs_miner_owner_status hs_miner_owner_init(
    const struct hs_miner_owner_policy *policy,
    const struct hs_miner_owner_ops *ops, void *context,
    uint32_t initial_confirmations, struct hs_miner_owner *destination);

struct hs_miner_owner_result hs_miner_owner_step(
    struct hs_miner_owner *owner, const struct hs_miner_owner_input *input);

#endif
