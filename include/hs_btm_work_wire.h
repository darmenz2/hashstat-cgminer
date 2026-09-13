/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef HS_BTM_WORK_WIRE_H
#define HS_BTM_WORK_WIRE_H

#include <stddef.h>
#include <stdint.h>

#define HS_BTM_RING_WORK_PREFIX_BYTES ((size_t)76)
#define HS_BTM_WORK_WIRE_BYTES ((size_t)88)
#define HS_BTM_WORK_SLOT_COUNT 32U

#define HS_WIRE_MAX_CRC_BYTES ((size_t)65536)

enum hs_work_wire_status {
    HS_WORK_WIRE_OK = 0,
    HS_WORK_WIRE_INVALID_ARGUMENT,
    HS_WORK_WIRE_INVALID_LENGTH,
    HS_WORK_WIRE_INVALID_SLOT,
    HS_WORK_WIRE_INSUFFICIENT_CAPACITY,
    HS_WORK_WIRE_ADDRESS_OVERFLOW,
    HS_WORK_WIRE_OVERLAP,
    HS_WORK_WIRE_RESOURCE_LIMIT
};

struct hs_wire_crc_result {
    enum hs_work_wire_status status;
    uint16_t crc;
};

struct hs_work_wire_result {
    enum hs_work_wire_status status;
    size_t written;
};

struct hs_wire_crc_result hs_wire_crc16_1021(
    const uint8_t *data, size_t len, uint16_t seed);

struct hs_work_wire_result hs_btm_work_ring_from_header80(
    const uint8_t *header, size_t header_length,
    uint8_t *destination, size_t destination_capacity);

struct hs_work_wire_result hs_btm_work_wire_pack(
    const uint8_t *ring_work_prefix_76, size_t prefix_len, unsigned slot,
    uint8_t *destination, size_t destination_capacity);

#endif
