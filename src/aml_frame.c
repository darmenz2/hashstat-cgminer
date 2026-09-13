/* SPDX-License-Identifier: GPL-3.0-only */
#include "hs_aml_frame.h"
#include "hs_span.h"

struct hs_aml_frame_result hs_aml_frame_pack(
    const uint8_t *payload,
    size_t payload_len,
    uint8_t *destination,
    size_t destination_capacity)
{
    struct hs_aml_frame_result result = { HS_AML_FRAME_OK, 0U };
    size_t frame_len;
    size_t index;

    if ((payload == NULL && payload_len != 0U) || destination == NULL) {
        result.status = HS_AML_FRAME_INVALID_ARGUMENT;
        return result;
    }
    if (payload_len > SIZE_MAX - HS_AML_FRAME_PREFIX_BYTES) {
        result.status = HS_AML_FRAME_SIZE_OVERFLOW;
        return result;
    }
    frame_len = payload_len + HS_AML_FRAME_PREFIX_BYTES;
    if (destination_capacity < frame_len) {
        result.status = HS_AML_FRAME_DESTINATION_TOO_SMALL;
        return result;
    }
    if (hs_span_overflows(payload, payload_len) ||
        hs_span_overflows(destination, frame_len)) {
        result.status = HS_AML_FRAME_SIZE_OVERFLOW;
        return result;
    }
    if (hs_spans_overlap(payload, payload_len, destination, frame_len)) {
        result.status = HS_AML_FRAME_OVERLAPPING_BUFFERS;
        return result;
    }

    destination[0] = UINT8_C(0x55);
    destination[1] = UINT8_C(0xaa);
    for (index = 0U; index < payload_len; ++index) {
        destination[index + HS_AML_FRAME_PREFIX_BYTES] = payload[index];
    }
    result.frame_bytes = frame_len;
    return result;
}
