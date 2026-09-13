/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef HS_AML_FRAME_H
#define HS_AML_FRAME_H

#include <stddef.h>
#include <stdint.h>

#define HS_AML_FRAME_PREFIX_BYTES ((size_t)2)

enum hs_aml_frame_status {
    HS_AML_FRAME_OK = 0,
    HS_AML_FRAME_INVALID_ARGUMENT,
    HS_AML_FRAME_SIZE_OVERFLOW,
    HS_AML_FRAME_DESTINATION_TOO_SMALL,
    HS_AML_FRAME_OVERLAPPING_BUFFERS
};

struct hs_aml_frame_result {
    enum hs_aml_frame_status status;
    size_t frame_bytes;
};

struct hs_aml_frame_result hs_aml_frame_pack(
    const uint8_t *payload,
    size_t payload_len,
    uint8_t *destination,
    size_t destination_capacity);

#endif
