/* SPDX-License-Identifier: GPL-3.0-or-later
 * Public captured fixture bytes attributed to Mujina, Ryan Kuester and
 * contributors, commit 019d1b0916457bb7ecd7cb7e93fee63c0329f9a2.
 * These are NOT HashStat hardware captures. See RX-INTEGRITY.md.
 * All generated/corruption tests are original offline tests, not ASIC traffic.
 */
#include "hs_bm1362_integrity.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t assertions;
#define CHECK(expression) do { ++assertions; if (!(expression)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); exit(1); \
} } while (0)

struct fixture {
    uint8_t frame[11];
    enum hs_bm1362_response_kind kind;
};
static const struct fixture fixtures[] = {

    {{0xaa,0x55,0x81,0xc9,0x77,0xd0,0x00,0xf2,0x7c,0x3e,0x89}, HS_BM1362_RESPONSE_NONCE},

    {{0xaa,0x55,0x13,0x70,0x00,0x00,0x00,0x00,0x00,0x00,0x10}, HS_BM1362_RESPONSE_REGISTER},

    {{0xaa,0x55,0x18,0x00,0xa6,0x40,0x02,0x99,0x22,0xf9,0x91}, HS_BM1362_RESPONSE_NONCE},
    {{0xaa,0x55,0x07,0x35,0xcd,0xcf,0x02,0x5e,0x00,0x2e,0x96}, HS_BM1362_RESPONSE_NONCE},
    {{0xaa,0x55,0x46,0x03,0x32,0xe7,0x00,0xc3,0x2c,0x83,0x99}, HS_BM1362_RESPONSE_NONCE},

    {{0xaa,0x55,0x4c,0x03,0x52,0x75,0x0c,0xd2,0x05,0xa2,0x9c}, HS_BM1362_RESPONSE_NONCE}
};




static uint8_t polynomial_oracle(const uint8_t payload[9])
{
    unsigned char coefficients[77] = {0};
    unsigned residual = 0;
    size_t i;
    for (i = 0; i < 5; ++i) coefficients[i] = 1;
    for (i = 0; i < 72; ++i) {
        unsigned shift = 7u - (unsigned)(i % 8u);
        coefficients[i] ^= (unsigned char)(((unsigned)payload[i / 8u] >> shift) & 1u);
    }
    for (i = 0; i < 72; ++i) {
        if (coefficients[i] != 0) {
            coefficients[i] ^= 1u;
            coefficients[i + 3] ^= 1u;
            coefficients[i + 5] ^= 1u;
        }
    }
    for (i = 72; i < 77; ++i) residual = (residual << 1u) | coefficients[i];
    return (uint8_t)residual;
}

static void check_framing_failure(struct hs_bm1362_integrity r,
                                  enum hs_bm1362_integrity_status expected)
{
    CHECK(r.status == expected);
    CHECK(!r.framing_valid && !r.crc_checked && !r.crc_valid && !r.supported_type);
    CHECK(r.kind == HS_BM1362_RESPONSE_UNKNOWN);
    CHECK(r.type_bits == 0 && r.crc_residual == 0);
}

static void test_captures(void)
{
    size_t f;
    for (f = 0; f < sizeof(fixtures) / sizeof(fixtures[0]); ++f) {
        uint8_t saved[11];
        uint8_t changed[11];
        struct hs_bm1362_integrity r;
        size_t bit;
        memcpy(saved, fixtures[f].frame, sizeof(saved));
        r = hs_bm1362_inspect_integrity(fixtures[f].frame, 11);
        CHECK(r.status == HS_BM1362_INTEGRITY_OK);
        CHECK(r.framing_valid && r.crc_checked && r.crc_valid && r.supported_type);
        CHECK(r.kind == fixtures[f].kind);
        CHECK(r.crc_residual == 0);
        CHECK(polynomial_oracle(&fixtures[f].frame[2]) == 0);
        CHECK(memcmp(saved, fixtures[f].frame, sizeof(saved)) == 0);
        for (bit = 0; bit < 88; ++bit) {
            memcpy(changed, saved, sizeof(changed));
            changed[bit / 8u] ^= (uint8_t)(1u << (unsigned)(bit % 8u));
            r = hs_bm1362_inspect_integrity(changed, sizeof(changed));
            CHECK(r.status != HS_BM1362_INTEGRITY_OK);
            if (bit < 16) check_framing_failure(r, HS_BM1362_INTEGRITY_SYNC);
            else {
                CHECK(r.status == HS_BM1362_INTEGRITY_CRC_MISMATCH);
                CHECK(r.framing_valid && r.crc_checked && !r.crc_valid);
                CHECK(r.crc_residual == polynomial_oracle(&changed[2]));
            }
        }
    }
}

static void test_bounds(void)
{
    static const size_t invalid_lengths[] = {12,13,22,255,65536,SIZE_MAX};
    size_t length, i;
    uint8_t single_byte = 0xaa;
    for (length = 0; length < 11; ++length) {


        uint8_t *prefix = malloc(length ? length : 1);
        CHECK(prefix != NULL);
        if (length) memcpy(prefix, fixtures[0].frame, length);
        check_framing_failure(hs_bm1362_inspect_integrity(prefix, length), HS_BM1362_INTEGRITY_LENGTH);
        check_framing_failure(hs_bm1362_inspect_integrity(NULL, length), HS_BM1362_INTEGRITY_ARGUMENT);
        free(prefix);
    }
    check_framing_failure(hs_bm1362_inspect_integrity(NULL, 11), HS_BM1362_INTEGRITY_ARGUMENT);
    for (i = 0; i < 10; ++i) {
        const uint8_t *wrapped = (const uint8_t *)(uintptr_t)(UINTPTR_MAX - i);
        check_framing_failure(hs_bm1362_inspect_integrity(wrapped, 11), HS_BM1362_INTEGRITY_ARGUMENT);
        check_framing_failure(hs_bm1362_inspect_integrity(wrapped, 10), HS_BM1362_INTEGRITY_LENGTH);
        check_framing_failure(hs_bm1362_inspect_integrity(wrapped, SIZE_MAX), HS_BM1362_INTEGRITY_LENGTH);
    }
    for (i = 0; i < sizeof(invalid_lengths) / sizeof(invalid_lengths[0]); ++i) {

        check_framing_failure(hs_bm1362_inspect_integrity(&single_byte, invalid_lengths[i]), HS_BM1362_INTEGRITY_LENGTH);
        check_framing_failure(hs_bm1362_inspect_integrity(NULL, invalid_lengths[i]), HS_BM1362_INTEGRITY_ARGUMENT);
    }
    for (i = 0; i < 256; ++i) {
        uint8_t frame[11];
        memcpy(frame, fixtures[0].frame, sizeof(frame));
        if (i != 0xaa) {
            frame[0] = (uint8_t)i;
            check_framing_failure(hs_bm1362_inspect_integrity(frame, 11), HS_BM1362_INTEGRITY_SYNC);
        }
        memcpy(frame, fixtures[0].frame, sizeof(frame));
        if (i != 0x55) {
            frame[1] = (uint8_t)i;
            check_framing_failure(hs_bm1362_inspect_integrity(frame, 11), HS_BM1362_INTEGRITY_SYNC);
        }
    }
    {
        uint8_t unaligned[12];
        memcpy(&unaligned[1], fixtures[0].frame, 11);
        CHECK(hs_bm1362_inspect_integrity(&unaligned[1], 11).status == HS_BM1362_INTEGRITY_OK);
    }
}

static void check_payload(const uint8_t frame[11])
{
    struct hs_bm1362_integrity r = hs_bm1362_inspect_integrity(frame, 11);
    uint8_t residual = polynomial_oracle(&frame[2]);
    uint8_t flags = (uint8_t)(frame[10] & 0xe0u);
    bool supported = flags == 0 || flags == 0x80;
    enum hs_bm1362_response_kind expected_kind = flags == 0 ? HS_BM1362_RESPONSE_REGISTER :
        (flags == 0x80 ? HS_BM1362_RESPONSE_NONCE : HS_BM1362_RESPONSE_UNKNOWN);
    CHECK(r.framing_valid && r.crc_checked);
    CHECK(r.crc_residual == residual);
    CHECK(r.crc_valid == (residual == 0));
    CHECK(r.type_bits == flags && r.supported_type == supported);
    CHECK(r.kind == expected_kind);
    CHECK(r.status == (residual != 0 ? HS_BM1362_INTEGRITY_CRC_MISMATCH :
        (supported ? HS_BM1362_INTEGRITY_OK : HS_BM1362_INTEGRITY_UNSUPPORTED_TYPE)));
}

static void test_all_types(void)
{
    unsigned type;
    for (type = 0; type < 8; ++type) {
        unsigned low, valid = 0;
        uint8_t frame[11];
        memcpy(frame, fixtures[0].frame, sizeof(frame));
        for (low = 0; low < 32; ++low) {
            struct hs_bm1362_integrity r;
            frame[10] = (uint8_t)((type << 5u) | low);
            check_payload(frame);
            if (polynomial_oracle(&frame[2]) != 0) continue;
            ++valid;
            r = hs_bm1362_inspect_integrity(frame, sizeof(frame));
            CHECK(r.crc_valid);


            CHECK(r.status == ((type == 0 || type == 4) ? HS_BM1362_INTEGRITY_OK : HS_BM1362_INTEGRITY_UNSUPPORTED_TYPE));
        }
        CHECK(valid == 1);
    }
}

static uint32_t random_word(uint32_t *state)
{
    *state ^= *state << 13u;
    *state ^= *state >> 17u;
    *state ^= *state << 5u;
    return *state;
}

static void test_differential(void)
{
    uint32_t state = UINT32_C(0x1362c5a1);
    size_t iteration, i;
    for (iteration = 0; iteration < 10000; ++iteration) {
        uint8_t frame[11] = {0xaa,0x55};
        uint8_t saved[11];
        for (i = 2; i < sizeof(frame); ++i) frame[i] = (uint8_t)random_word(&state);
        memcpy(saved, frame, sizeof(saved));
        check_payload(frame);
        CHECK(memcmp(saved, frame, sizeof(saved)) == 0);
    }
}

int main(void)
{
    test_captures();
    test_bounds();
    test_all_types();
    test_differential();
    printf("BM1362 opt-in RX integrity: %zu assertions passed; no I/O or hardware\n", assertions);
    return 0;
}
