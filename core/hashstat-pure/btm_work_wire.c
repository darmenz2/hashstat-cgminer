/* SPDX-License-Identifier: GPL-3.0-only */
#include "hs_btm_work_wire.h"
#include "hs_span.h"

static uint16_t crc_unchecked(const uint8_t *data, size_t length, uint16_t seed)
{
    uint32_t state = seed;
    for (size_t i = 0; i < length; ++i) {
        state ^= (uint32_t)data[i] << 8;
        for (unsigned bit = 0; bit < 8U; ++bit) {
            state = ((state << 1) ^ ((state & 0x8000U) ? 0x1021U : 0U)) & 0xffffU;
        }
    }
    return (uint16_t)state;
}

struct hs_wire_crc_result hs_wire_crc16_1021(
    const uint8_t *data, size_t len, uint16_t seed)
{
    struct hs_wire_crc_result result = { HS_WORK_WIRE_OK, 0 };
    if (data == NULL && len != 0U) {
        result.status = HS_WORK_WIRE_INVALID_ARGUMENT;
    } else if (len > HS_WIRE_MAX_CRC_BYTES) {
        result.status = HS_WORK_WIRE_RESOURCE_LIMIT;
    } else if (hs_span_overflows(data, len)) {
        result.status = HS_WORK_WIRE_ADDRESS_OVERFLOW;
    } else {
        result.crc = crc_unchecked(data, len, seed);
    }
    return result;
}

struct hs_work_wire_result hs_btm_work_wire_pack(
    const uint8_t *ring_work_prefix_76, size_t prefix_len, unsigned slot,
    uint8_t *destination, size_t destination_capacity)
{
    struct hs_work_wire_result result = { HS_WORK_WIRE_OK, 0 };
    if (ring_work_prefix_76 == NULL || destination == NULL) {
        result.status = HS_WORK_WIRE_INVALID_ARGUMENT;
    } else if (prefix_len != HS_BTM_RING_WORK_PREFIX_BYTES) {
        result.status = HS_WORK_WIRE_INVALID_LENGTH;
    } else if (slot >= HS_BTM_WORK_SLOT_COUNT) {
        result.status = HS_WORK_WIRE_INVALID_SLOT;
    } else if (destination_capacity < HS_BTM_WORK_WIRE_BYTES) {
        result.status = HS_WORK_WIRE_INSUFFICIENT_CAPACITY;
    } else if (hs_span_overflows(ring_work_prefix_76, prefix_len) ||
               hs_span_overflows(destination, HS_BTM_WORK_WIRE_BYTES)) {
        result.status = HS_WORK_WIRE_ADDRESS_OVERFLOW;
    } else if (hs_spans_overlap(ring_work_prefix_76, prefix_len,
                             destination, HS_BTM_WORK_WIRE_BYTES)) {
        result.status = HS_WORK_WIRE_OVERLAP;
    }
    if (result.status != HS_WORK_WIRE_OK) {
        return result;
    }

    destination[0] = 0x55;
    destination[1] = 0xaa;
    destination[2] = 0x21;
    destination[3] = 0x36;
    destination[4] = (uint8_t)(slot << 3);
    destination[5] = 1;
    for (size_t i = 6; i < 10; ++i) {
        destination[i] = 0;
    }
    for (size_t i = 0; i < 12; ++i) {
        destination[10 + i] = ring_work_prefix_76[64 + i];
    }
    for (size_t i = 0; i < 4; ++i) {
        destination[22 + i] = ring_work_prefix_76[i];
    }
    for (size_t i = 0; i < 32; ++i) {
        destination[26 + i] = ring_work_prefix_76[4 + i];
    }
    for (size_t i = 0; i < 28; ++i) {
        destination[58 + i] = ring_work_prefix_76[36 + i];
    }

    const uint16_t crc = crc_unchecked(destination + 2, 84, 0xffff);
    destination[86] = (uint8_t)(crc >> 8);
    destination[87] = (uint8_t)(crc & 0xffU);
    result.written = HS_BTM_WORK_WIRE_BYTES;
    return result;
}

struct hs_work_wire_result hs_btm_work_ring_from_header80(
    const uint8_t *header, size_t header_length,
    uint8_t *destination, size_t destination_capacity)
{
    struct hs_work_wire_result result = { HS_WORK_WIRE_OK, 0 };
    if (header == NULL || destination == NULL) {
        result.status = HS_WORK_WIRE_INVALID_ARGUMENT;
    } else if (header_length != 80U) {
        result.status = HS_WORK_WIRE_INVALID_LENGTH;
    } else if (destination_capacity < HS_BTM_RING_WORK_PREFIX_BYTES) {
        result.status = HS_WORK_WIRE_INSUFFICIENT_CAPACITY;
    } else if (hs_span_overflows(header, header_length) ||
               hs_span_overflows(destination, HS_BTM_RING_WORK_PREFIX_BYTES)) {
        result.status = HS_WORK_WIRE_ADDRESS_OVERFLOW;
    } else if (hs_spans_overlap(header, header_length,
                             destination, HS_BTM_RING_WORK_PREFIX_BYTES)) {
        result.status = HS_WORK_WIRE_OVERLAP;
    }
    if (result.status != HS_WORK_WIRE_OK)
        return result;

    for (size_t word = 0; word < 16U; ++word)
        for (size_t byte = 0; byte < 4U; ++byte)
            destination[word * 4U + byte] = header[(15U - word) * 4U + byte];
    for (size_t word = 0; word < 3U; ++word)
        for (size_t byte = 0; byte < 4U; ++byte)
            destination[64U + word * 4U + byte] = header[(18U - word) * 4U + byte];
    result.written = HS_BTM_RING_WORK_PREFIX_BYTES;
    return result;
}
