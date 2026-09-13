/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef HS_BM1362_COMMANDS_H
#define HS_BM1362_COMMANDS_H

#include <stddef.h>
#include <stdint.h>

#define HS_BM1362_READ_PAYLOAD_BYTES ((size_t)5)
#define HS_BM1362_SHORT_PAYLOAD_BYTES ((size_t)5)
#define HS_BM1362_WRITE_PAYLOAD_BYTES ((size_t)9)

enum hs_bm1362_command_status {
    HS_BM1362_COMMAND_OK = 0,
    HS_BM1362_COMMAND_INVALID_ARGUMENT,
    HS_BM1362_COMMAND_INVALID_ADDRESS,
    HS_BM1362_COMMAND_INVALID_REGISTER,
    HS_BM1362_COMMAND_INSUFFICIENT_CAPACITY,
    HS_BM1362_COMMAND_ADDRESS_OVERFLOW
};

struct hs_bm1362_command_result {
    enum hs_bm1362_command_status status;
    size_t written;
};

struct hs_bm1362_command_result hs_bm1362_encode_register_read(
    unsigned chip_address, unsigned register_id,
    uint8_t *destination, size_t destination_capacity);

struct hs_bm1362_command_result hs_bm1362_encode_probe(
    uint8_t *destination, size_t destination_capacity);

struct hs_bm1362_command_result hs_bm1362_encode_register_write(
    unsigned chip_address, unsigned register_id, uint32_t value,
    uint8_t *destination, size_t destination_capacity);

struct hs_bm1362_command_result hs_bm1362_encode_broadcast_register_write(
    unsigned register_id, uint32_t value,
    uint8_t *destination, size_t destination_capacity);

struct hs_bm1362_command_result hs_bm1362_encode_inactivate(
    uint8_t *destination, size_t destination_capacity);

struct hs_bm1362_command_result hs_bm1362_encode_address_assignment(
    unsigned chip_address, uint8_t *destination, size_t destination_capacity);

#endif
