/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef HS_BM1362_SETUP_H
#define HS_BM1362_SETUP_H
#include <stddef.h>
#include <stdint.h>

#define HS_BM1362_SETUP_FRAME_BYTES 11U
#define HS_BM1362_SETUP_WRITES 7U
#define HS_BM1362_SETUP_MAX_TIMEOUT_MS UINT32_C(30000)

enum hs_bm1362_setup_status {
    HS_SETUP_ARGUMENT = 0, HS_SETUP_PENDING, HS_SETUP_WRITE,
    HS_SETUP_ACK_REQUIRED, HS_SETUP_WAIT, HS_SETUP_TX_COMPLETE,
    HS_SETUP_FAILED, HS_SETUP_CANCELLED
};
enum hs_bm1362_setup_stage {
    HS_SETUP_ASSERT_A8 = 0, HS_SETUP_ASSERT_18, HS_SETUP_CLEAR_A8,
    HS_SETUP_CLEAR_18, HS_SETUP_CLOCK_SELECT, HS_SETUP_CLOCK_DELAY,
    HS_SETUP_CORE_ENABLE, HS_SETUP_STAGE_DONE
};
enum hs_bm1362_setup_failure {
    HS_SETUP_FAILURE_NONE = 0, HS_SETUP_FAILURE_TRANSPORT,
    HS_SETUP_FAILURE_PARTIAL, HS_SETUP_FAILURE_DEADLINE,
    HS_SETUP_FAILURE_CLOCK, HS_SETUP_FAILURE_ACK,
    HS_SETUP_FAILURE_ENCODING, HS_SETUP_FAILURE_CANCEL
};
enum hs_bm1362_setup_receipt {
    HS_SETUP_TRANSFER_COMPLETE = 0, HS_SETUP_TRANSFER_FAILED,
    HS_SETUP_TRANSFER_CANCELLED
};
struct hs_bm1362_setup_config {
    uint64_t attempt_tag;
    uint32_t chain_cached_a8, chain_cached_18;
    uint32_t write_timeout_ms;
    unsigned chip_address;
    unsigned fast;
};
struct hs_bm1362_setup_result {
    enum hs_bm1362_setup_status status;
    enum hs_bm1362_setup_stage stage;
    enum hs_bm1362_setup_failure failure;
    uint64_t attempt_tag;
    unsigned write_id, completed_writes;
    uint32_t remaining_ms, register_value;
    size_t frame_bytes, confirmed_bytes;
    int transport_error;
    uint8_t register_id, chip_address;
    uint8_t frame[HS_BM1362_SETUP_FRAME_BYTES];
};

struct hs_bm1362_setup {
    const struct hs_bm1362_setup *self;
    struct hs_bm1362_setup_config config;
    uint64_t last_ms, since_ms;
    enum hs_bm1362_setup_stage stage;
    enum hs_bm1362_setup_failure failure;
    unsigned completed_writes;
    uint8_t state, wait_kind;
    size_t confirmed_bytes;
    int transport_error;
};
enum hs_bm1362_setup_status hs_bm1362_setup_init(struct hs_bm1362_setup *,
    const struct hs_bm1362_setup_config *, uint64_t now_ms);

struct hs_bm1362_setup_result hs_bm1362_setup_next(struct hs_bm1362_setup *, uint64_t now_ms);

struct hs_bm1362_setup_result hs_bm1362_setup_ack(struct hs_bm1362_setup *,
    uint64_t now_ms, uint64_t attempt_tag, unsigned write_id,
    enum hs_bm1362_setup_receipt, size_t confirmed_bytes, int transport_error);

struct hs_bm1362_setup_result hs_bm1362_setup_cancel(struct hs_bm1362_setup *, uint64_t now_ms);
#endif
