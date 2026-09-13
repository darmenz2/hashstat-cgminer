/* SPDX-License-Identifier: GPL-3.0-only */
#include "hs_bm1362_rx.h"
#include "hs_span.h"

static struct hs_bm1362_rx_result result(enum hs_bm1362_rx_status status)
{
    struct hs_bm1362_rx_result out = {0};
    out.status = status;
    return out;
}

struct hs_bm1362_rx_result hs_bm1362_rx_decode_long(
    const uint8_t *frame, size_t length)
{
    struct hs_bm1362_rx_result out;
    uint32_t nonce, chip, b6, b7;
    if (frame == NULL)
        return result(HS_BM1362_RX_INVALID_ARGUMENT);
    if (length != HS_BM1362_RX_FRAME_BYTES)
        return result(HS_BM1362_RX_INVALID_LENGTH);
    if (hs_span_overflows(frame, length))
        return result(HS_BM1362_RX_ADDRESS_OVERFLOW);
    if (frame[0] != 0xaa || frame[1] != 0x55)
        return result(HS_BM1362_RX_INVALID_SYNC);
    if ((frame[10] & 0x80U) == 0)
        return result(HS_BM1362_RX_NOT_NONCE);

    nonce = ((uint32_t)frame[2] << 24) | ((uint32_t)frame[3] << 16)
          | ((uint32_t)frame[4] << 8) | (uint32_t)frame[5];

    chip = ((nonce >> 17) & UINT32_C(255)) / 2U;
    if (chip >= HS_BM1362_S19_88_CHIPS)
        return result(HS_BM1362_RX_INVALID_CHIP);

    b6 = frame[8];
    b7 = frame[9];
    out = result(HS_BM1362_RX_DECODED_UNVERIFIED);
    out.internal_nonce = nonce;
    out.nonce = (uint32_t)frame[2] | ((uint32_t)frame[3] << 8)
              | ((uint32_t)frame[4] << 16) | ((uint32_t)frame[5] << 24);

    out.internal_version_bits = (b6 >> 3) | ((b7 & 7U) << 21)
                              | ((b6 & 7U) << 13) | ((b7 & 0xf8U) << 5);
    out.version_bits = (out.internal_version_bits << 24)
                     | ((out.internal_version_bits & UINT32_C(0xff00)) << 8)
                     | ((out.internal_version_bits >> 8) & UINT32_C(0xff00))
                     | (out.internal_version_bits >> 24);
    out.slot = (uint8_t)(frame[7] >> 3);
    out.chip_index = (uint8_t)chip;
    out.core_index = (uint8_t)(nonce >> 25);
    out.opaque_payload4 = frame[6];
    out.opaque_tail_low5 = (uint8_t)(frame[10] & 31U);
    return out;
}

static void retain_later_sync(struct hs_bm1362_rx_stream *state)
{
    size_t start, index;
    for (start = 1; start + 1 < HS_BM1362_RX_FRAME_BYTES; ++start) {
        if (state->frame[start] == 0xaa && state->frame[start + 1] == 0x55) {
            state->used = (uint8_t)(HS_BM1362_RX_FRAME_BYTES - start);
            for (index = 0; index < state->used; ++index)
                state->frame[index] = state->frame[start + index];
            return;
        }
    }
    state->used = 0;
    if (state->frame[HS_BM1362_RX_FRAME_BYTES - 1] == 0xaa) {
        state->frame[0] = 0xaa;
        state->used = 1;
    }
}

struct hs_bm1362_rx_result hs_bm1362_rx_stream_push(
    struct hs_bm1362_rx_stream *state, uint8_t byte)
{
    struct hs_bm1362_rx_result out;
    if (state == NULL)
        return result(HS_BM1362_RX_INVALID_ARGUMENT);
    if (hs_span_overflows(state, sizeof(*state)))
        return result(HS_BM1362_RX_ADDRESS_OVERFLOW);
    if (state->used >= HS_BM1362_RX_FRAME_BYTES
        || (state->used >= 1 && state->frame[0] != 0xaa)
        || (state->used >= 2 && state->frame[1] != 0x55))
        return result(HS_BM1362_RX_INVALID_STATE);

    if (state->used == 0) {
        if (byte == 0xaa) {
            state->frame[0] = byte;
            state->used = 1;
        }
        return result(HS_BM1362_RX_NEED_MORE);
    }
    if (state->used == 1) {
        if (byte == 0x55) {
            state->frame[1] = byte;
            state->used = 2;
        } else if (byte != 0xaa) {
            state->used = 0;
        }
        return result(HS_BM1362_RX_NEED_MORE);
    }
    state->frame[state->used++] = byte;
    if (state->used < HS_BM1362_RX_FRAME_BYTES)
        return result(HS_BM1362_RX_NEED_MORE);
    out = hs_bm1362_rx_decode_long(state->frame, HS_BM1362_RX_FRAME_BYTES);
    if (out.status == HS_BM1362_RX_INVALID_CHIP)
        retain_later_sync(state);
    else
        state->used = 0;
    return out;
}
