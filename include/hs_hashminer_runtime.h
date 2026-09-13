/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef HS_HASHMINER_RUNTIME_H
#define HS_HASHMINER_RUNTIME_H
#include "hs_aml_observe.h"
#include "hs_miner_owner.h"
#include <sys/types.h>

#define HS_HASHMINER_REPORT_BYTES 28U
struct hs_aml88_bridge;
enum hs_hashminer_runtime_status {
    HS_HR_OK = 0, HS_HR_ARGUMENT, HS_HR_BUSY, HS_HR_NO_HASHBOARDS,
    HS_HR_UNQUALIFIED, HS_HR_CANCELLED, HS_HR_TIMEOUT, HS_HR_SYSTEM_ERROR,
    HS_HR_PROTOCOL_ERROR, HS_HR_REAP_UNKNOWN, HS_HR_EXHAUSTED,
    HS_HR_POWER_UNVERIFIED, HS_HR_BRIDGE_ERROR
};
enum hs_hashminer_runtime_phase {
    HS_HR_IDLE = 0, HS_HR_OBSERVING, HS_HR_CANCELLING, HS_HR_KILLING,
    HS_HR_FINISHED, HS_HR_STOPPED, HS_HR_FAULTED
};
struct hs_hashminer_runtime_policy {
    uint64_t observation_timeout_ms, cancel_grace_ms, reap_grace_ms;

    bool isolated_single_thread_entry;

    bool exact_aml_gpio_binding;
};
struct hs_hashminer_runtime_result {
    enum hs_hashminer_runtime_status status;
    enum hs_hashminer_runtime_phase phase;
    struct hs_aml_observation observation;
    uint64_t observation_generation;
    pid_t owned_pid;
    bool observation_valid, process_quiescent, work_gate_closed;
    bool mining_ready, physical_power_off_confirmed;
    bool term_sent, kill_sent;
    bool descriptor_cleanup_unknown;
    int child_wait_status;
};
struct hs_hashminer_pending_ack {
    bool pending;
    struct hs_miner_operation_request request;
};
struct hs_hashminer_runtime {
    const struct hs_hashminer_runtime *self;
    struct hs_hashminer_runtime_policy policy;
    hs_aml_attribute_reader reader;
    void *reader_context;
    pid_t child;
    int read_fd, wait_status;
    uint64_t generation, began_ms, cancel_ms, kill_ms, last_now_ms;
    unsigned char report[HS_HASHMINER_REPORT_BYTES + 1U];
    size_t received;
    struct hs_aml_observation observation;
    enum hs_hashminer_runtime_status status;
    enum hs_hashminer_runtime_phase phase;
    bool clock_started, eof, observation_valid, process_quiescent;
    bool stop_requested, term_sent, kill_sent, fault_latched;
    bool work_gate_closed, bridge_failed;
    bool descriptor_cleanup_unknown;
    struct hs_miner_owner *owner;
    struct hs_aml88_bridge *bridge;
    struct hs_hashminer_pending_ack acknowledgments[HS_MINER_OP_COUNT];
};

enum hs_hashminer_runtime_status hs_hashminer_runtime_init(
    struct hs_hashminer_runtime *, const struct hs_hashminer_runtime_policy *,
    hs_aml_attribute_reader, void *reader_context);
enum hs_hashminer_runtime_status hs_hashminer_runtime_observe(struct hs_hashminer_runtime *);

struct hs_hashminer_runtime_result hs_hashminer_runtime_poll(struct hs_hashminer_runtime *);
void hs_hashminer_runtime_cancel(struct hs_hashminer_runtime *);

const struct hs_miner_owner_ops *hs_hashminer_runtime_owner_ops(void);
enum hs_hashminer_runtime_status hs_hashminer_runtime_bind_owner(
    struct hs_hashminer_runtime *, struct hs_miner_owner *, struct hs_aml88_bridge *);
struct hs_miner_owner_result hs_hashminer_runtime_owner_step(
    struct hs_hashminer_runtime *, enum hs_miner_command, enum hs_miner_owner_fault);

bool hs_hashminer_runtime_close_bridge(struct hs_hashminer_runtime *);

#endif
