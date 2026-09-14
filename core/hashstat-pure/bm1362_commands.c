/* SPDX-License-Identifier: GPL-3.0-only */
#include "hs_bm1362_commands.h"
#include "hs_span.h"

static uint8_t crc5_bytes(const uint8_t *bytes, size_t length)
{
    unsigned crc = 0x1fU;
    size_t byte_index;
    unsigned bit;

    for (byte_index = 0U; byte_index < length; ++byte_index) {
        for (bit = 8U; bit != 0U; --bit) {
            unsigned feedback = ((crc >> 4U) ^
                                 ((unsigned)bytes[byte_index] >> (bit - 1U))) & 1U;
            crc = ((crc << 1U) & 0x1fU) ^ (feedback != 0U ? 0x05U : 0U);
        }
    }
    return (uint8_t)crc;
}

static struct hs_bm1362_command_result validate(
    unsigned chip_address, unsigned register_id, uint8_t *destination,
    size_t destination_capacity, size_t required_length)
{
    struct hs_bm1362_command_result result = { HS_BM1362_COMMAND_OK, 0U };

    if (destination == NULL) {
        result.status = HS_BM1362_COMMAND_INVALID_ARGUMENT;
        return result;
    }
    if (chip_address > 255U) {
        result.status = HS_BM1362_COMMAND_INVALID_ADDRESS;
        return result;
    }
    if (register_id > 255U) {
        result.status = HS_BM1362_COMMAND_INVALID_REGISTER;
        return result;
    }
    if (destination_capacity < required_length) {
        result.status = HS_BM1362_COMMAND_INSUFFICIENT_CAPACITY;
        return result;
    }
    if (hs_span_overflows(destination, required_length)) {
        result.status = HS_BM1362_COMMAND_ADDRESS_OVERFLOW;
        return result;
    }

    return result;
}

static struct hs_bm1362_command_result encode_short(
    uint8_t opcode, unsigned chip_address, unsigned register_id,
    uint8_t *destination, size_t destination_capacity)
{
    struct hs_bm1362_command_result result = validate(
        chip_address, register_id, destination, destination_capacity,
        HS_BM1362_SHORT_PAYLOAD_BYTES);
    uint8_t prefix[4];
    size_t index;

    if (result.status != HS_BM1362_COMMAND_OK) return result;
    prefix[0] = opcode;
    prefix[1] = UINT8_C(5);
    prefix[2] = (uint8_t)chip_address;
    prefix[3] = (uint8_t)register_id;
    for (index = 0U; index < sizeof(prefix); ++index) {
        destination[index] = prefix[index];
    }
    destination[4] = crc5_bytes(prefix, sizeof(prefix));
    result.written = HS_BM1362_SHORT_PAYLOAD_BYTES;
    return result;
}

struct hs_bm1362_command_result hs_bm1362_encode_register_read(
    unsigned chip_address, unsigned register_id,
    uint8_t *destination, size_t destination_capacity)
{
    return encode_short(UINT8_C(0x42), chip_address, register_id,
                        destination, destination_capacity);
}

struct hs_bm1362_command_result hs_bm1362_encode_probe(
    uint8_t *destination, size_t destination_capacity)
{
    return encode_short(UINT8_C(0x52), 0U, 4U, destination, destination_capacity);
}

static struct hs_bm1362_command_result encode_write(
    uint8_t opcode, unsigned chip_address, unsigned register_id, uint32_t value,
    uint8_t *destination, size_t destination_capacity)
{
    struct hs_bm1362_command_result result = validate(
        chip_address, register_id, destination, destination_capacity,
        HS_BM1362_WRITE_PAYLOAD_BYTES);
    uint8_t prefix[8];
    size_t index;

    if (result.status != HS_BM1362_COMMAND_OK) return result;
    prefix[0] = opcode;
    prefix[1] = UINT8_C(9);
    prefix[2] = (uint8_t)chip_address;
    prefix[3] = (uint8_t)register_id;
    prefix[4] = (uint8_t)(value >> 24U);
    prefix[5] = (uint8_t)(value >> 16U);
    prefix[6] = (uint8_t)(value >> 8U);
    prefix[7] = (uint8_t)value;
    for (index = 0U; index < sizeof(prefix); ++index) {
        destination[index] = prefix[index];
    }
    destination[8] = crc5_bytes(prefix, sizeof(prefix));
    result.written = HS_BM1362_WRITE_PAYLOAD_BYTES;
    return result;
}

struct hs_bm1362_command_result hs_bm1362_encode_register_write(
    unsigned chip_address, unsigned register_id, uint32_t value,
    uint8_t *destination, size_t destination_capacity)
{
    return encode_write(UINT8_C(0x41), chip_address, register_id, value,
                        destination, destination_capacity);
}

struct hs_bm1362_command_result hs_bm1362_encode_broadcast_register_write(
    unsigned register_id, uint32_t value,
    uint8_t *destination, size_t destination_capacity)
{
    return encode_write(UINT8_C(0x51), 0U, register_id, value,
                        destination, destination_capacity);
}

struct hs_bm1362_command_result hs_bm1362_encode_inactivate(
    uint8_t *destination, size_t destination_capacity)
{
    return encode_short(UINT8_C(0x53), 0U, 0U, destination, destination_capacity);
}

struct hs_bm1362_command_result hs_bm1362_encode_address_assignment(
    unsigned chip_address, uint8_t *destination, size_t destination_capacity)
{
    return encode_short(UINT8_C(0x40), chip_address, 0U,
                        destination, destination_capacity);
}

struct hs_bm1362_command_result hs_bm1362_encode_chip_id_request(
    uint8_t *destination, size_t destination_capacity)
{
    return encode_short(UINT8_C(0x52), 0U, 0U, destination, destination_capacity);
}
