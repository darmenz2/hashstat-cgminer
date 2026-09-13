/* SPDX-License-Identifier: GPL-3.0-only */
#include "hashstat-core.h"
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static unsigned nibble(char c)
{
    return c >= '0' && c <= '9' ? (unsigned)(c - '0') : (unsigned)(c - 'a' + 10);
}
static void hex(const char *s, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; ++i) out[i] = (uint8_t)((nibble(s[i*2]) << 4) | nibble(s[i*2+1]));
}
int main(void)
{
    uint32_t out = UINT32_C(0xa5a5a5a5);
    CHECK(hs_core_parse_vmask("1fffe000", 8, HS_CORE_BM1362_MASK, &out));
    CHECK(out == HS_CORE_BM1362_MASK);
    CHECK(hs_core_parse_vmask("1FFFE000", 8, HS_CORE_BM1362_MASK, &out));
    CHECK(out == HS_CORE_BM1362_MASK);
    const char *bad[] = {"", "1fffe00", "1fffe0000", "00000000", "ffffffff",
                         "80000000", "1fffe00z", " 0002000", "+0002000", "0x002000",
                         "00002000\n", "0000\000000", "00002001", "ffffe000"};
    const size_t lengths[] = {0,7,9,8,8,8,8,8,8,8,9,8,8,8};
    for (size_t i = 0; i < sizeof(bad)/sizeof(*bad); ++i) {
        out = UINT32_C(0xa5a5a5a5);
        CHECK(!hs_core_parse_vmask(bad[i], lengths[i], HS_CORE_BM1362_MASK, &out));
        CHECK(out == UINT32_C(0xa5a5a5a5));
    }
    CHECK(!hs_core_parse_vmask(NULL, 8, HS_CORE_BM1362_MASK, &out));
    CHECK(!hs_core_parse_vmask("00002000", 8, HS_CORE_BM1362_MASK, NULL));
    CHECK(!hs_core_parse_vmask("00002000", 8, 0, &out));
    for (unsigned i = 13; i < 29; ++i) {
        char buffer[9];
        const uint32_t value = UINT32_C(1) << i;
        (void)snprintf(buffer, sizeof(buffer), "%08x", (unsigned)value);
        CHECK(hs_core_parse_vmask(buffer, 8, HS_CORE_BM1362_MASK, &out));
        CHECK(out == value);
    }
    const uint64_t exceptional[] = {UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000),
                                    UINT64_C(0x7ff8000000000001), UINT64_C(1)};
    for (size_t i = 0; i < sizeof(exceptional)/sizeof(*exceptional); ++i) {
        double d; memcpy(&d, &exceptional[i], sizeof(d));
        CHECK(!hs_core_valid_difficulty(d));
    }
    CHECK(!hs_core_valid_difficulty(0)); CHECK(!hs_core_valid_difficulty(-0.0));
    CHECK(!hs_core_valid_difficulty(-1)); CHECK(!hs_core_valid_difficulty(DBL_MAX));
    CHECK(!hs_core_valid_difficulty(0x1p-33)); CHECK(!hs_core_valid_difficulty(0x1p64));
    CHECK(hs_core_valid_difficulty(HS_CORE_MIN_DIFFICULTY));
    CHECK(hs_core_valid_difficulty(HS_CORE_MAX_DIFFICULTY));
    CHECK(hs_core_valid_difficulty(1)); CHECK(hs_core_valid_difficulty(0.5));
    CHECK(hs_core_valid_difficulty(65536.125));

    uint8_t genesis[80], data[80], roundtrip[80], digest[32], expected[32];
    hex("010000000000000000000000000000000000000000000000000000000000000000000000"
        "3ba3edfd7a7b12b27ac72c3e67768f617fc81bc3888a51323a9fb8aa4b1e5e4a"
        "29ab5f49ffff001d1dac2b7c", genesis, 80);
    hex("6fe28c0ab6f1b372c1a6a246ae63f74f931e8365e15a089c68d6190000000000", expected, 32);
    CHECK(hs_core_flip_header80(genesis, 80, data));
    CHECK(memcmp(data + 76, "\x7c\x2b\xac\x1d", 4) == 0);
    CHECK(hs_core_nonce_to_upstream(UINT32_C(0x7c2bac1d)) == UINT32_C(0x1dac2b7c));
    CHECK(hs_core_hash_work80(data, 80, digest)); CHECK(memcmp(digest, expected, 32) == 0);
    CHECK(hs_core_flip_header80(data, 80, roundtrip)); CHECK(memcmp(roundtrip, genesis, 80) == 0);
    CHECK(hs_core_flip_header80(data, 80, data)); CHECK(memcmp(data, genesis, 80) == 0);
    uint8_t overlap[88]; memcpy(overlap, genesis, 80);
    CHECK(hs_core_flip_header80(overlap, 80, overlap + 3));
    CHECK(hs_core_flip_header80(overlap + 3, 80, overlap)); CHECK(memcmp(overlap, genesis, 80) == 0);
    memset(roundtrip, 0xa5, sizeof(roundtrip));
    CHECK(!hs_core_flip_header80(NULL, 80, roundtrip));
    CHECK(!hs_core_flip_header80(genesis, 79, roundtrip));
    CHECK(!hs_core_flip_header80(genesis, 81, roundtrip));
    CHECK(!hs_core_flip_header80(genesis, 80, NULL));
    for (size_t i = 0; i < 80; ++i) CHECK(roundtrip[i] == 0xa5);
    memset(digest, 0xa5, sizeof(digest));
    CHECK(!hs_core_hash_work80(data, 79, digest)); CHECK(!hs_core_hash_work80(NULL, 80, digest));
    CHECK(!hs_core_hash_work80(data, 80, NULL));
    for (size_t i = 0; i < 32; ++i) CHECK(digest[i] == 0xa5);
    uint32_t seed = UINT32_C(0x48533838);
    for (unsigned i = 0; i < 10000; ++i) {
        seed = seed * UINT32_C(1664525) + UINT32_C(1013904223);
        CHECK(hs_core_nonce_to_upstream(hs_core_nonce_to_upstream(seed)) == seed);
    }
    printf("PASS %u HashStat core scalar/byte assertions; no hardware test\n", checks);
    return 0;
}
