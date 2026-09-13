/* SPDX-License-Identifier: GPL-3.0-only */
#include "hs_btm_work_wire.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned cases;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "failed %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static void check_crc(const uint8_t *bytes, size_t len, uint16_t seed,
                      uint16_t expected)
{
    const struct hs_wire_crc_result r = hs_wire_crc16_1021(bytes, len, seed);
    CHECK(r.status == HS_WORK_WIRE_OK);
    CHECK(r.crc == expected);
    ++cases;
}

static void crc_vectors(void)
{
    uint8_t ascending[256], zero[256] = { 0 };
    for (size_t i = 0; i < sizeof ascending; ++i) ascending[i] = (uint8_t)i;

    check_crc(NULL, 0, 0xffff, 0xffff);
    check_crc(NULL, 0, 0x1234, 0x1234);
    check_crc((const uint8_t *)"123456789", 9, 0xffff, 0x29b1);
    check_crc(zero, sizeof zero, 0xffff, 0x41e8);
    check_crc(ascending, sizeof ascending, 0xffff, 0x3fbd);
    check_crc(ascending, sizeof ascending, 0, 0x7e55);
    struct hs_wire_crc_result r = hs_wire_crc16_1021(NULL, 1, 0xffff);
    CHECK(r.status == HS_WORK_WIRE_INVALID_ARGUMENT && r.crc == 0);
    r = hs_wire_crc16_1021(ascending, SIZE_MAX, 0xffff);
    CHECK(r.status == HS_WORK_WIRE_RESOURCE_LIMIT && r.crc == 0);
    r = hs_wire_crc16_1021((const uint8_t *)(uintptr_t)(UINTPTR_MAX - 2U), 4, 0xffff);
    CHECK(r.status == HS_WORK_WIRE_ADDRESS_OVERFLOW && r.crc == 0);
    cases += 3;
}

static unsigned hex_digit(char value)
{
    if (value >= '0' && value <= '9') return (unsigned)(value - '0');
    if (value >= 'a' && value <= 'f') return (unsigned)(value - 'a') + 10U;
    CHECK(0);
    return 0;
}

static void vector(const uint8_t *prefix, unsigned slot, const char *expected_hex)
{
    uint8_t expected[HS_BTM_WORK_WIRE_BYTES];
    uint8_t output[HS_BTM_WORK_WIRE_BYTES + 2];
    uint8_t before[HS_BTM_RING_WORK_PREFIX_BYTES];
    CHECK(strlen(expected_hex) == sizeof expected * 2U);
    for (size_t i = 0; i < sizeof expected; ++i) {
        expected[i] = (uint8_t)((hex_digit(expected_hex[i * 2U]) << 4) |
                                hex_digit(expected_hex[i * 2U + 1U]));
    }
    memset(output, 0xc3, sizeof output);
    memcpy(before, prefix, sizeof before);
    const struct hs_work_wire_result r = hs_btm_work_wire_pack(
        prefix, sizeof before, slot, output + 1, sizeof expected);
    CHECK(r.status == HS_WORK_WIRE_OK && r.written == sizeof expected);
    CHECK(memcmp(output + 1, expected, sizeof expected) == 0);
    CHECK(memcmp(prefix, before, sizeof before) == 0);
    CHECK(output[0] == 0xc3 && output[sizeof output - 1U] == 0xc3);
    ++cases;
}

static void packet_vectors(void)
{
    uint8_t prefix[HS_BTM_RING_WORK_PREFIX_BYTES] = { 0 };

    vector(prefix, 0,
        "55aa21360001000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000009887");
    for (size_t i = 0; i < sizeof prefix; ++i) prefix[i] = (uint8_t)i;
    vector(prefix, 31,
        "55aa2136f80100000000404142434445464748494a4b000102030405060708090a0b"
        "0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c"
        "2d2e2f303132333435363738393a3b3c3d3e3ffc72");
    memset(prefix, 0xff, sizeof prefix);
    vector(prefix, 17,
        "55aa2136880100000000ffffffffffffffffffffffffffffffffffffffffffffff"
        "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
        "fffffffffffffffffffffffffffffffffffffffffff919");
}

static void rejected(const uint8_t *prefix, size_t len, unsigned slot,
                     uint8_t *output, size_t capacity,
                     enum hs_work_wire_status expected)
{
    uint8_t before[160];
    CHECK(capacity <= sizeof before);
    if (output != NULL) memcpy(before, output, capacity);
    const struct hs_work_wire_result r = hs_btm_work_wire_pack(
        prefix, len, slot, output, capacity);
    CHECK(r.status == expected && r.written == 0);
    if (output != NULL) CHECK(memcmp(before, output, capacity) == 0);
    ++cases;
}

static void adversarial(void)
{
    uint8_t source[76], output[160];
    memset(source, 0xe1, sizeof source);
    memset(output, 0xc7, sizeof output);
    rejected(NULL, 76, 0, output, 88, HS_WORK_WIRE_INVALID_ARGUMENT);
    rejected(source, 76, 0, NULL, 88, HS_WORK_WIRE_INVALID_ARGUMENT);
    rejected(source, 0, 0, output, 88, HS_WORK_WIRE_INVALID_LENGTH);
    rejected(source, 75, 0, output, 88, HS_WORK_WIRE_INVALID_LENGTH);
    rejected(source, 77, 0, output, 88, HS_WORK_WIRE_INVALID_LENGTH);
    rejected(source, SIZE_MAX, 0, output, 88, HS_WORK_WIRE_INVALID_LENGTH);
    rejected(source, 76, 32, output, 88, HS_WORK_WIRE_INVALID_SLOT);
    rejected(source, 76, UINT_MAX, output, 88, HS_WORK_WIRE_INVALID_SLOT);
    for (size_t cap = 0; cap < 88; ++cap) {
        rejected(source, 76, 0, output, cap, HS_WORK_WIRE_INSUFFICIENT_CAPACITY);
    }
    rejected((const uint8_t *)(uintptr_t)(UINTPTR_MAX - 32U), 76, 0,
             output, 88, HS_WORK_WIRE_ADDRESS_OVERFLOW);
    const struct hs_work_wire_result r = hs_btm_work_wire_pack(
        source, 76, 0, (uint8_t *)(uintptr_t)(UINTPTR_MAX - 32U), 88);
    CHECK(r.status == HS_WORK_WIRE_ADDRESS_OVERFLOW && r.written == 0);
    ++cases;

    uint8_t arena[256], saved[256];
    for (size_t in = 0; in <= 168; ++in) {
        for (size_t out = 0; out <= 168; ++out) {
            for (size_t i = 0; i < sizeof arena; ++i) arena[i] = (uint8_t)i;
            memcpy(saved, arena, sizeof arena);
            const int overlap = in < out + 88U && out < in + 76U;
            const struct hs_work_wire_result result = hs_btm_work_wire_pack(
                arena + in, 76, 0, arena + out, sizeof arena - out);
            CHECK(result.status == (overlap ? HS_WORK_WIRE_OVERLAP : HS_WORK_WIRE_OK));
            CHECK(result.written == (overlap ? 0U : 88U));
            for (size_t i = 0; i < sizeof arena; ++i) {
                if (overlap || i < out || i >= out + 88U) CHECK(arena[i] == saved[i]);
            }
            ++cases;
        }
    }
}

int main(void)
{
    crc_vectors();
    packet_vectors();
    adversarial();
    printf("btm work wire: %u cases passed\n", cases);
    return 0;
}
