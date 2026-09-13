/* SPDX-License-Identifier: GPL-3.0-only */
#include "hs_bm1362_commands.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static uint8_t reference_taps(const uint8_t *prefix, size_t length)
{
    unsigned a = 1U, b = 1U, c = 1U, d = 1U, e = 1U;
    size_t index;
    unsigned mask;

    for (index = 0U; index < length; ++index) {
        for (mask = 128U; mask != 0U; mask >>= 1U) {
            unsigned feedback = a ^ ((prefix[index] & mask) != 0U ? 1U : 0U);
            unsigned old_b = b, old_c = c, old_d = d, old_e = e;
            a = old_b;
            b = old_c;
            c = old_d ^ feedback;
            d = old_e;
            e = feedback;
        }
    }
    return (uint8_t)((a << 4U) | (b << 3U) | (c << 2U) | (d << 1U) | e);
}

static void check_write(unsigned address, unsigned reg, uint32_t value,
                        unsigned broadcast)
{
    struct hs_bm1362_command_result result;
    uint8_t guarded[17], expected[9];
    uint32_t remaining = value;
    size_t index;

    expected[0] = (uint8_t)(broadcast != 0U ? 0x51U : 0x41U);
    expected[1] = 9U;
    expected[2] = (uint8_t)(broadcast != 0U ? 0U : address);
    expected[3] = (uint8_t)reg;
    for (index = 8U; index > 4U; --index) {
        expected[index - 1U] = (uint8_t)(remaining % UINT32_C(256));
        remaining /= UINT32_C(256);
    }
    expected[8] = reference_taps(expected, 8U);
    memset(guarded, 0xb6, sizeof(guarded));
    if (broadcast != 0U) {
        result = hs_bm1362_encode_broadcast_register_write(reg, value, guarded + 3U, 9U);
    } else {
        result = hs_bm1362_encode_register_write(address, reg, value, guarded + 3U, 9U);
    }
    assert(result.status == HS_BM1362_COMMAND_OK && result.written == 9U);
    assert(memcmp(guarded + 3U, expected, sizeof(expected)) == 0);
    for (index = 0U; index < sizeof(guarded); ++index) {
        if (index < 3U || index > 11U) assert(guarded[index] == 0xb6U);
    }
}

int main(void)
{
    static const uint8_t fixed[][5] = {
        {0x42, 0x05, 0x00, 0x00, 0x0e},
        {0x42, 0x05, 0x00, 0x04, 0x1a},
        {0x42, 0x05, 0x02, 0x04, 0x07},
        {0x42, 0x05, 0xae, 0x04, 0x0c},
        {0x42, 0x05, 0xff, 0xff, 0x05}
    };
    static const uint8_t probe[5] = {0x52, 0x05, 0x00, 0x04, 0x1e};
    struct hs_bm1362_command_result result;
    uint8_t guarded[17], before[17];
    unsigned address, reg;
    size_t index, capacity;
    unsigned long checks = 0UL;

    for (address = 0U; address < 256U; ++address) {
        for (reg = 0U; reg < 256U; ++reg) {
            memset(guarded, 0xa7, sizeof(guarded));
            result = hs_bm1362_encode_register_read(address, reg, guarded + 3U, 9U);
            assert(result.status == HS_BM1362_COMMAND_OK && result.written == 5U);
            assert(guarded[3] == 0x42U && guarded[4] == 5U);
            assert(guarded[5] == address && guarded[6] == reg);
            assert(guarded[7] == reference_taps(guarded + 3U, 4U));
            for (index = 0U; index < sizeof(guarded); ++index) {
                if (index < 3U || index > 7U) assert(guarded[index] == 0xa7U);
            }
            ++checks;
        }
    }
    for (index = 0U; index < sizeof(fixed) / sizeof(fixed[0]); ++index) {
        result = hs_bm1362_encode_register_read(fixed[index][2], fixed[index][3], guarded, 5U);
        assert(result.status == HS_BM1362_COMMAND_OK);
        assert(memcmp(guarded, fixed[index], 5U) == 0);
        ++checks;
    }
    memset(guarded, 0xa7, sizeof(guarded));
    result = hs_bm1362_encode_probe(guarded + 3U, 5U);
    assert(result.status == HS_BM1362_COMMAND_OK && result.written == 5U);
    assert(memcmp(guarded + 3U, probe, 5U) == 0);
    assert(probe[4] == reference_taps(probe, 4U));
    for (index = 0U; index < sizeof(guarded); ++index) {
        if (index < 3U || index > 7U) assert(guarded[index] == 0xa7U);
    }
    ++checks;

    memset(guarded, 0x69, sizeof(guarded));
    memcpy(before, guarded, sizeof(before));
    for (capacity = 0U; capacity < 5U; ++capacity) {
        result = hs_bm1362_encode_register_read(0U, 4U, guarded, capacity);
        assert(result.status == HS_BM1362_COMMAND_INSUFFICIENT_CAPACITY && result.written == 0U);
        assert(memcmp(guarded, before, sizeof(before)) == 0);
        result = hs_bm1362_encode_probe(guarded, capacity);
        assert(result.status == HS_BM1362_COMMAND_INSUFFICIENT_CAPACITY && result.written == 0U);
        assert(memcmp(guarded, before, sizeof(before)) == 0);
        checks += 2UL;
    }
    result = hs_bm1362_encode_register_read(256U, 4U, guarded, sizeof(guarded));
    assert(result.status == HS_BM1362_COMMAND_INVALID_ADDRESS && result.written == 0U);
    result = hs_bm1362_encode_register_read(UINT_MAX, 4U, guarded, sizeof(guarded));
    assert(result.status == HS_BM1362_COMMAND_INVALID_ADDRESS && result.written == 0U);
    result = hs_bm1362_encode_register_read(0U, 256U, guarded, sizeof(guarded));
    assert(result.status == HS_BM1362_COMMAND_INVALID_REGISTER && result.written == 0U);
    result = hs_bm1362_encode_register_read(0U, UINT_MAX, guarded, sizeof(guarded));
    assert(result.status == HS_BM1362_COMMAND_INVALID_REGISTER && result.written == 0U);
    assert(memcmp(guarded, before, sizeof(before)) == 0);
    checks += 4UL;

    result = hs_bm1362_encode_register_read(0U, 4U, NULL, SIZE_MAX);
    assert(result.status == HS_BM1362_COMMAND_INVALID_ARGUMENT && result.written == 0U);
    result = hs_bm1362_encode_probe(NULL, 5U);
    assert(result.status == HS_BM1362_COMMAND_INVALID_ARGUMENT && result.written == 0U);
    checks += 2UL;
    for (index = 0U; index < 4U; ++index) {
        uint8_t *invalid_span = (uint8_t *)(UINTPTR_MAX - index);
        result = hs_bm1362_encode_register_read(0U, 4U, invalid_span, 5U);
        assert(result.status == HS_BM1362_COMMAND_ADDRESS_OVERFLOW && result.written == 0U);
        result = hs_bm1362_encode_probe(invalid_span, SIZE_MAX);
        assert(result.status == HS_BM1362_COMMAND_ADDRESS_OVERFLOW && result.written == 0U);
        checks += 2UL;
    }

    for (address = 0U; address < 256U; ++address) {
        uint8_t expected_assignment[5] = {0x40, 5, 0, 0, 0};
        expected_assignment[2] = (uint8_t)address;
        expected_assignment[4] = reference_taps(expected_assignment, 4U);
        memset(guarded, 0xb6, sizeof(guarded));
        result = hs_bm1362_encode_address_assignment(address, guarded + 3U, 5U);
        assert(result.status == HS_BM1362_COMMAND_OK && result.written == 5U);
        assert(memcmp(guarded + 3U, expected_assignment, 5U) == 0);
        for (index = 0U; index < sizeof(guarded); ++index) {
            if (index < 3U || index > 7U) assert(guarded[index] == 0xb6U);
        }
        ++checks;
        for (reg = 0U; reg < 256U; ++reg) {
            uint32_t value = (uint32_t)address * UINT32_C(0x01000001) ^
                             (uint32_t)reg * UINT32_C(0x00010100);
            check_write(address, reg, value, 0U);
            ++checks;
        }
        check_write(0U, address, (uint32_t)address * UINT32_C(0x01010101), 1U);
        ++checks;
    }
    for (index = 0U; index < 32U; ++index) {
        uint32_t value = UINT32_C(1) << index;
        check_write(0U, 0U, value, 0U);
        check_write(255U, 255U, ~value, 0U);
        check_write(0U, 8U, value, 1U);
        checks += 3UL;
    }
    {
        uint32_t state = UINT32_C(0x13621988);
        for (index = 0U; index < 10000U; ++index) {
            state ^= state << 13U;
            state ^= state >> 17U;
            state ^= state << 5U;
            check_write((unsigned)(state & UINT32_C(255)),
                        (unsigned)((state >> 8U) & UINT32_C(255)), state, 0U);
            ++checks;
        }
    }
    {
        static const uint8_t inactive[5] = {0x53, 5, 0, 0, 3};
        static const uint8_t assign[4][5] = {
            {0x40, 5, 0, 0, 0x1c}, {0x40, 5, 2, 0, 1},
            {0x40, 5, 0xae, 0, 0x0a}, {0x40, 5, 0xff, 0, 3}
        };
        static const uint8_t writes[4][9] = {
            {0x41, 9, 0, 8, 0, 0, 0, 0, 0x12},
            {0x41, 9, 0xff, 8, 0xff, 0xff, 0xff, 0xff, 0x0c},
            {0x51, 9, 0, 8, 0x12, 0x34, 0x56, 0x78, 0x1b},
            {0x41, 9, 2, 0x3c, 0x80, 0, 0x85, 0x40, 0x1b}
        };
        static const uint32_t values[4] = {
            UINT32_C(0), UINT32_MAX, UINT32_C(0x12345678), UINT32_C(0x80008540)
        };
        memset(guarded, 0xb6, sizeof(guarded));
        result = hs_bm1362_encode_inactivate(guarded + 3U, 5U);
        assert(result.status == HS_BM1362_COMMAND_OK && result.written == 5U);
        assert(memcmp(guarded + 3U, inactive, 5U) == 0);
        for (index = 0U; index < sizeof(guarded); ++index) {
            if (index < 3U || index > 7U) assert(guarded[index] == 0xb6U);
        }
        ++checks;
        for (index = 0U; index < 4U; ++index) {
            result = hs_bm1362_encode_address_assignment(assign[index][2], guarded, 5U);
            assert(result.status == HS_BM1362_COMMAND_OK && result.written == 5U);
            assert(memcmp(guarded, assign[index], 5U) == 0);
            if (writes[index][0] == 0x51U) {
                result = hs_bm1362_encode_broadcast_register_write(
                    writes[index][3], values[index], guarded, 9U);
            } else {
                result = hs_bm1362_encode_register_write(
                    writes[index][2], writes[index][3], values[index], guarded, 9U);
            }
            assert(result.status == HS_BM1362_COMMAND_OK && result.written == 9U);
            assert(memcmp(guarded, writes[index], 9U) == 0);
            checks += 2UL;
        }
    }

    memset(guarded, 0x69, sizeof(guarded));
    memcpy(before, guarded, sizeof(before));
    for (capacity = 0U; capacity < 9U; ++capacity) {
        result = hs_bm1362_encode_register_write(0U, 0U, 0U, guarded, capacity);
        assert(result.status == HS_BM1362_COMMAND_INSUFFICIENT_CAPACITY && result.written == 0U);
        result = hs_bm1362_encode_broadcast_register_write(0U, 0U, guarded, capacity);
        assert(result.status == HS_BM1362_COMMAND_INSUFFICIENT_CAPACITY && result.written == 0U);
        checks += 2UL;
        if (capacity < 5U) {
            result = hs_bm1362_encode_inactivate(guarded, capacity);
            assert(result.status == HS_BM1362_COMMAND_INSUFFICIENT_CAPACITY && result.written == 0U);
            result = hs_bm1362_encode_address_assignment(0U, guarded, capacity);
            assert(result.status == HS_BM1362_COMMAND_INSUFFICIENT_CAPACITY && result.written == 0U);
            checks += 2UL;
        }
        assert(memcmp(guarded, before, sizeof(before)) == 0);
    }
    for (index = 0U; index < 2U; ++index) {
        unsigned invalid = index == 0U ? 256U : UINT_MAX;
        result = hs_bm1362_encode_register_write(invalid, 0U, 0U, guarded, sizeof(guarded));
        assert(result.status == HS_BM1362_COMMAND_INVALID_ADDRESS && result.written == 0U);
        result = hs_bm1362_encode_register_write(0U, invalid, 0U, guarded, sizeof(guarded));
        assert(result.status == HS_BM1362_COMMAND_INVALID_REGISTER && result.written == 0U);
        result = hs_bm1362_encode_broadcast_register_write(invalid, 0U, guarded, sizeof(guarded));
        assert(result.status == HS_BM1362_COMMAND_INVALID_REGISTER && result.written == 0U);
        result = hs_bm1362_encode_address_assignment(invalid, guarded, sizeof(guarded));
        assert(result.status == HS_BM1362_COMMAND_INVALID_ADDRESS && result.written == 0U);
        assert(memcmp(guarded, before, sizeof(before)) == 0);
        checks += 4UL;
    }
    result = hs_bm1362_encode_register_write(0U, 0U, 0U, NULL, SIZE_MAX);
    assert(result.status == HS_BM1362_COMMAND_INVALID_ARGUMENT && result.written == 0U);
    result = hs_bm1362_encode_broadcast_register_write(0U, 0U, NULL, SIZE_MAX);
    assert(result.status == HS_BM1362_COMMAND_INVALID_ARGUMENT && result.written == 0U);
    result = hs_bm1362_encode_inactivate(NULL, SIZE_MAX);
    assert(result.status == HS_BM1362_COMMAND_INVALID_ARGUMENT && result.written == 0U);
    result = hs_bm1362_encode_address_assignment(0U, NULL, SIZE_MAX);
    assert(result.status == HS_BM1362_COMMAND_INVALID_ARGUMENT && result.written == 0U);
    checks += 4UL;
    for (index = 0U; index < 8U; ++index) {
        uint8_t *invalid_span = (uint8_t *)(UINTPTR_MAX - index);
        result = hs_bm1362_encode_register_write(0U, 0U, 0U, invalid_span, SIZE_MAX);
        assert(result.status == HS_BM1362_COMMAND_ADDRESS_OVERFLOW && result.written == 0U);
        result = hs_bm1362_encode_broadcast_register_write(0U, 0U, invalid_span, SIZE_MAX);
        assert(result.status == HS_BM1362_COMMAND_ADDRESS_OVERFLOW && result.written == 0U);
        checks += 2UL;
        if (index < 4U) {
            result = hs_bm1362_encode_inactivate(invalid_span, SIZE_MAX);
            assert(result.status == HS_BM1362_COMMAND_ADDRESS_OVERFLOW && result.written == 0U);
            result = hs_bm1362_encode_address_assignment(0U, invalid_span, SIZE_MAX);
            assert(result.status == HS_BM1362_COMMAND_ADDRESS_OVERFLOW && result.written == 0U);
            checks += 2UL;
        }
    }
    printf("BM1362 pure command encoders: %lu cases passed; no hardware\n", checks);
    return 0;
}
