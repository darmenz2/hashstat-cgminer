/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef HS_BM1362_RX_H
#define HS_BM1362_RX_H

#include <stddef.h>
#include <stdint.h>

#define HS_BM1362_RX_FRAME_BYTES ((size_t)11)
#define HS_BM1362_S19_88_CHIPS 88U
#define HS_BM1362_RX_INTERNAL_VERSION_BITS_MASK UINT32_C(0x00e0ff1f)
#define HS_BM1362_RX_VERSION_BITS_MASK UINT32_C(0x1fffe000)

enum hs_bm1362_rx_status {
    HS_BM1362_RX_NEED_MORE = 0,
    HS_BM1362_RX_DECODED_UNVERIFIED,
    HS_BM1362_RX_NOT_NONCE,
    HS_BM1362_RX_INVALID_ARGUMENT,
    HS_BM1362_RX_INVALID_LENGTH,
    HS_BM1362_RX_INVALID_SYNC,
    HS_BM1362_RX_INVALID_CHIP,
    HS_BM1362_RX_ADDRESS_OVERFLOW,
    HS_BM1362_RX_INVALID_STATE
};

struct hs_bm1362_rx_result {
    enum hs_bm1362_rx_status status;

    uint32_t nonce;

    uint32_t internal_nonce;

    uint32_t version_bits;

    uint32_t internal_version_bits;
    uint8_t slot;
    uint8_t chip_index;

    uint8_t core_index;
    uint8_t opaque_payload4;
    uint8_t opaque_tail_low5;
};

struct hs_bm1362_rx_result hs_bm1362_rx_decode_long(
    const uint8_t *frame, size_t length);

struct hs_bm1362_rx_stream {
    uint8_t frame[11];
    uint8_t used;
};

struct hs_bm1362_rx_result hs_bm1362_rx_stream_push(
    struct hs_bm1362_rx_stream *state, uint8_t byte);

#endif
