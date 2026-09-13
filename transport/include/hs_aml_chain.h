/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef HS_AML_CHAIN_H
#define HS_AML_CHAIN_H
#include "hs_aml_uart.h"
#include "hs_job_cache.h"
#include "hs_miner_lifecycle.h"
#include "hs_bm1362_integrity.h"

enum hs_aml_chain_status {
    HS_CHAIN_ARGUMENT = 0, HS_CHAIN_OK, HS_CHAIN_CLOSED, HS_CHAIN_NOT_READY,
    HS_CHAIN_JOB_REJECTED, HS_CHAIN_TRANSPORT_FAILED, HS_CHAIN_CLOCK_ERROR,
    HS_CHAIN_NEED_MORE, HS_CHAIN_BAD_CRC, HS_CHAIN_BAD_TYPE, HS_CHAIN_BAD_CHIP,
    HS_CHAIN_REGISTER, HS_CHAIN_SHARE_REJECTED, HS_CHAIN_SHARE
};
struct hs_aml_chain_event {
    enum hs_aml_chain_status status;
    enum hs_job_status job_status;
    struct hs_aml_uart_result transfer;

    uint8_t register_frame[11];

    struct hs_checked_share share;
};

struct hs_aml_chain {
    const struct hs_aml_chain *self;
    struct hs_aml_uart_session *uart;
    const struct hs_miner_lifecycle *lifecycle;
    uint64_t generation, last_ms;
    unsigned cleanup_errors;
    uint8_t clock_started, active, used;
    uint8_t frame[11];
    struct hs_job_cache jobs;
};
enum hs_aml_chain_status hs_aml_chain_bind(struct hs_aml_chain *,
    struct hs_aml_uart_session *, const struct hs_miner_lifecycle *,
    uint64_t session_tag);
unsigned hs_aml_chain_stop(struct hs_aml_chain *);

struct hs_aml_chain_event hs_aml_chain_send(struct hs_aml_chain *, unsigned slot,
    const struct hs_job_snapshot *, uint64_t now_ms, uint32_t timeout_ms);

struct hs_aml_chain_event hs_aml_chain_push(struct hs_aml_chain *, uint8_t,
                                          uint64_t now_ms);

enum hs_aml_chain_status hs_aml_chain_reset_jobs(struct hs_aml_chain *,
                                              uint64_t session_tag);
#endif
