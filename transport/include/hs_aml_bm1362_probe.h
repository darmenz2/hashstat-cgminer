/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef HS_AML_BM1362_PROBE_H
#define HS_AML_BM1362_PROBE_H

#include "hs_aml_uart.h"
#include "hs_miner_lifecycle.h"

#define HS_AML_BM1362_PROBE_MAX_RX_BYTES ((size_t)4096)

enum hs_aml_bm1362_probe_status {
    HS_PROBE_OK = 0, HS_PROBE_ARGUMENT, HS_PROBE_NOT_READY, HS_PROBE_CLOSED,
    HS_PROBE_ALREADY_ATTEMPTED, HS_PROBE_TRANSPORT_FAILED,
    HS_PROBE_CANCELLED, HS_PROBE_CLOCK_ERROR, HS_PROBE_TIMEOUT, HS_PROBE_BUDGET
};

struct hs_aml_bm1362_probe_readiness {
    uint32_t ready_flags;
    bool explicit_probe_opt_in;
    bool aml_bm1362_binding_verified;
    bool long_reply_mode_verified;
    bool rx_boundary_verified;
    bool tx_cleanup_verified;
};

struct hs_aml_bm1362_probe_result {
    enum hs_aml_bm1362_probe_status status;

    struct hs_aml_uart_result transfer;
    size_t tx_bytes, rx_bytes;
    unsigned bad_crc_frames, other_frames;

    unsigned chain;
    uint8_t chip_address, register_id, opaque[2], frame[11];
    uint32_t value;
};

struct hs_aml_bm1362_probe {
    const struct hs_aml_bm1362_probe *self;
    struct hs_aml_uart_session *uart;
    const struct hs_miner_lifecycle *lifecycle;
    const struct hs_aml_bm1362_probe_readiness *readiness;
    uint64_t generation;
    unsigned cleanup_errors;
    bool active, attempted, matched;
};

enum hs_aml_bm1362_probe_status hs_aml_bm1362_probe_bind(
    struct hs_aml_bm1362_probe *, struct hs_aml_uart_session *,
    const struct hs_miner_lifecycle *, const struct hs_aml_bm1362_probe_readiness *);

struct hs_aml_bm1362_probe_result hs_aml_bm1362_probe_run(
    struct hs_aml_bm1362_probe *, uint32_t timeout_ms);

struct hs_aml_uart_session *hs_aml_bm1362_probe_take_uart(struct hs_aml_bm1362_probe *);

unsigned hs_aml_bm1362_probe_stop(struct hs_aml_bm1362_probe *);

#endif
