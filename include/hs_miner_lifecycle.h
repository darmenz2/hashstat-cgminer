/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef HS_MINER_LIFECYCLE_H
#define HS_MINER_LIFECYCLE_H

#include <stdint.h>

/* Supervisory policy. Hardware sequencing and interlocks belong to the owner. */
#define HS_MINER_LIFECYCLE_ABI UINT32_C(0x484c4331)
#define HS_MINER_LIFECYCLE_MAX_RETRIES 32U

enum hs_miner_phase {
    HS_MINER_STOPPED = 0,
    HS_MINER_WAITING_HARDWARE,
    HS_MINER_STARTING,
    HS_MINER_RUNNING,
    HS_MINER_DRAINING,
    HS_MINER_BACKOFF,
    HS_MINER_FAULTED,
    HS_MINER_SHUTDOWN
};
enum hs_miner_command {
    HS_MINER_COMMAND_NONE = 0,
    HS_MINER_COMMAND_RUN,
    HS_MINER_COMMAND_STOP,
    HS_MINER_COMMAND_SHUTDOWN
};
enum hs_miner_owner_event {
    HS_MINER_OWNER_NONE = 0,
    HS_MINER_OWNER_READY,
    HS_MINER_OWNER_FAILED,
    HS_MINER_OWNER_DRAINED
};
enum hs_miner_reason {
    HS_MINER_REASON_NONE = 0,
    HS_MINER_REASON_USER_STOP,
    HS_MINER_REASON_SHUTDOWN,
    HS_MINER_REASON_HARDWARE_MISSING,
    HS_MINER_REASON_START_FAILED,
    HS_MINER_REASON_START_TIMEOUT,
    HS_MINER_REASON_HARDWARE_LOST,
    HS_MINER_REASON_RUN_FAILED,
    HS_MINER_REASON_DRAIN_TIMEOUT,
    HS_MINER_REASON_RETRY_LIMIT,
    HS_MINER_REASON_GENERATION_EXHAUSTED,
    HS_MINER_REASON_SUPERVISOR_ERROR
};
enum hs_miner_lifecycle_status {
    HS_MINER_LIFECYCLE_OK = 0,
    HS_MINER_LIFECYCLE_INVALID_ARGUMENT,
    HS_MINER_LIFECYCLE_INVALID_POLICY,
    HS_MINER_LIFECYCLE_INVALID_STATE,
    HS_MINER_LIFECYCLE_INVALID_INPUT,
    HS_MINER_LIFECYCLE_TIME_REVERSED,
    HS_MINER_LIFECYCLE_COMMAND_REJECTED
};
enum hs_miner_ready_flag {
    HS_MINER_READY_CONTROLLER = 1U << 0,
    HS_MINER_READY_HASHBOARDS = 1U << 1,
    HS_MINER_READY_FANS = 1U << 2,
    HS_MINER_READY_CONFIGURATION = 1U << 3,
    HS_MINER_READY_SAFETY = 1U << 4
};
#define HS_MINER_READY_ALL UINT32_C(31)

enum hs_miner_action {
    HS_MINER_ACTION_CLOSE_WORK = 1U << 0,
    HS_MINER_ACTION_OPEN_WORK = 1U << 1,
    HS_MINER_ACTION_REQUEST_START = 1U << 2,
    HS_MINER_ACTION_REQUEST_DRAIN = 1U << 3,
    HS_MINER_ACTION_REQUEST_CANCEL_START = 1U << 4,
    HS_MINER_ACTION_REQUEST_POWER_OFF = 1U << 5
};

struct hs_miner_lifecycle_policy {
    uint64_t startup_timeout_ms;
    uint64_t drain_timeout_ms;
    uint64_t first_backoff_ms;
    uint64_t max_backoff_ms;

    uint32_t max_retries;
};

struct hs_miner_lifecycle {
    uint32_t abi;
    struct hs_miner_lifecycle_policy policy;
    enum hs_miner_phase phase;
    enum hs_miner_command intent;
    enum hs_miner_reason reason;
    enum hs_miner_reason last_failure;
    uint32_t retries_reserved;
    uint64_t attempt_generation;
    uint64_t phase_since_ms;
    uint64_t last_now_ms;
    uint64_t retry_since_ms;
    uint64_t retry_delay_ms;
    uint8_t clock_started;
    uint8_t owner_quiescent;
};

struct hs_miner_lifecycle_input {
    uint64_t now_ms;
    enum hs_miner_command command;
    uint32_t ready_flags;
    enum hs_miner_owner_event owner_event;
    uint64_t owner_generation;
};

struct hs_miner_lifecycle_result {
    enum hs_miner_lifecycle_status status;
    enum hs_miner_phase phase;
    enum hs_miner_reason reason;
    uint32_t actions;
    uint32_t missing_ready_flags;
    uint64_t action_generation;
    uint64_t retry_remaining_ms;
    uint8_t work_permitted;
    uint8_t owner_event_ignored;
};

enum hs_miner_lifecycle_status hs_miner_lifecycle_init(
    const struct hs_miner_lifecycle_policy *policy,
    struct hs_miner_lifecycle *destination);

struct hs_miner_lifecycle_result hs_miner_lifecycle_step(
    struct hs_miner_lifecycle *state,
    const struct hs_miner_lifecycle_input *input);

#endif
