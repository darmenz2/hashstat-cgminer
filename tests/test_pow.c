/* SPDX-License-Identifier: GPL-3.0-only */

#include "hs_pow.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifdef NDEBUG
#error "Assertions are mandatory in this test executable"
#endif

static unsigned int tests_passed;
static uint8_t large_fixture[16777216];

static unsigned int nibble(char value)
{
    assert((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'));
    return (unsigned int)(value <= '9' ? value - '0' : value - 'a' + 10);
}

static void unhex(const char *value, uint8_t *bytes, size_t length)
{
    size_t i;
    assert(strlen(value) == length * 2);
    for (i = 0; i < length; ++i) {
        bytes[i] = (uint8_t)((nibble(value[2 * i]) << 4U) | nibble(value[2 * i + 1]));
    }
}

static void expect_hex(const uint8_t digest[32], const char *expected)
{
    uint8_t bytes[32];
    unhex(expected, bytes, 32);
    assert(memcmp(digest, bytes, 32) == 0);
}

static void expect_sentinel(const uint8_t *bytes, size_t length)
{
    size_t i;
    for (i = 0; i < length; ++i) {
        assert(bytes[i] == 0xa5);
    }
}

static void genesis(uint8_t header[80])
{
    unhex("01000000"
          "0000000000000000000000000000000000000000000000000000000000000000"
          "3ba3edfd7a7b12b27ac72c3e67768f617fc81bc3888a51323a9fb8aa4b1e5e4a"
          "29ab5f49ffff001d1dac2b7c", header, 80);
}

static void mainnet_limit(uint8_t limit[32])
{
    memset(limit, 0xff, 28);
    memset(limit + 28, 0, 4);
}

static void test_nist_abc(void)
{
    uint8_t digest[32];
    assert(hs_sha256((const uint8_t *)"abc", 3, digest) == HS_POW_OK);
    expect_hex(digest, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

static void test_nist_two_blocks(void)
{
    const char input[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    uint8_t digest[32];
    assert(sizeof(input) - 1 == 56);
    assert(hs_sha256((const uint8_t *)input, sizeof(input) - 1, digest) == HS_POW_OK);
    expect_hex(digest, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

static void test_empty(void)
{
    uint8_t digest[32];
    assert(hs_sha256(NULL, 0, digest) == HS_POW_OK);
    expect_hex(digest, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

static void test_binary_padding_boundaries(void)
{
    static const struct { size_t length; const char *hash; } cases[] = {
        {1, "6e340b9cffb37a989ca544e6bb780a2c78901d3fb33738768511a30617afa01d"},
        {55, "463eb28e72f82e0a96c0a4cc53690c571281131f672aa229e0d45ae59b598b59"},
        {56, "da2ae4d6b36748f2a318f23e7ab1dfdf45acdc9d049bd80e59de82a60895f562"},
        {63, "29af2686fd53374a36b0846694cc342177e428d1647515f078784d69cdb9e488"},
        {64, "fdeab9acf3710362bd2658cdc9a29e8f9c757fcf9811603a8c447cd1d9151108"},
        {65, "4bfd2c8b6f1eec7a2afeb48b934ee4b2694182027e6d0fc075074f2fabb31781"},
        {119, "da18797ed7c3a777f0847f429724a2d8cd5138e6ed2895c3fa1a6d39d18f7ec6"},
        {120, "f52b23db1fbb6ded89ef42a23ce0c8922c45f25c50b568a93bf1c075420bbb7c"},
        {127, "92ca0fa6651ee2f97b884b7246a562fa71250fedefe5ebf270d31c546bfea976"},
        {128, "471fb943aa23c511f6f72f8d1652d9c880cfa392ad80503120547703e56a2be5"},
        {129, "5099c6a56203f9687f7d33f4bfdf576d31dc91f6b695ecea38b2770c87631135"},
        {255, "3f8591112c6bbe5c963965954e293108b7208ed2af893e500d859368c654eabe"},
        {256, "40aff2e9d2d8922e47afd4648e6967497158785fbd1da870e7110266bf944880"}
    };
    uint8_t input[256], digest[32];
    size_t i;
    for (i = 0; i < sizeof(input); ++i) {
        input[i] = (uint8_t)i;
    }
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        assert(hs_sha256(input, cases[i].length, digest) == HS_POW_OK);
        expect_hex(digest, cases[i].hash);
    }
}

static void test_million_a(void)
{
    uint8_t digest[32];
    memset(large_fixture, 'a', 1000000);
    assert(hs_sha256(large_fixture, 1000000, digest) == HS_POW_OK);
    expect_hex(digest, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

static void test_exact_input_cap(void)
{
    uint8_t digest[32];
    assert(HS_SHA256_MAX_INPUT_BYTES == sizeof(large_fixture));
    memset(large_fixture, 0, sizeof(large_fixture));
    assert(hs_sha256(large_fixture, sizeof(large_fixture), digest) == HS_POW_OK);
    expect_hex(digest, "080acf35a507ac9849cfcba47dc2ad83e01b75663a516279c8b9d243b719643e");
}

static void test_double_hash(void)
{
    uint8_t digest[32];
    assert(hs_sha256d(NULL, 0, digest) == HS_POW_OK);
    expect_hex(digest, "5df6e0e2761359d30a8275058e299fcc0381534545f55cf43e41983f5d4c9456");
    assert(hs_sha256d((const uint8_t *)"abc", 3, digest) == HS_POW_OK);
    expect_hex(digest, "4f8b42c22dd3729b519ba6f68d2da7cc5b2d606d05daed5ad5128cc03e6c6358");
}

static void test_genesis_header(void)
{
    uint8_t header[80], digest[32], display[32];
    size_t i;
    genesis(header);
    assert(hs_sha256d_header80(header, sizeof(header), digest) == HS_POW_OK);
    expect_hex(digest, "6fe28c0ab6f1b372c1a6a246ae63f74f931e8365e15a089c68d6190000000000");
    for (i = 0; i < 32; ++i) {
        display[i] = digest[31 - i];
    }
    expect_hex(display, "000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f");
}

static void test_header_length_guards(void)
{
    uint8_t header[81], digest[32];
    memset(header, 0, sizeof(header));
    memset(digest, 0xa5, sizeof(digest));
    assert(hs_sha256d_header80(header, 79, digest) == HS_POW_INVALID);
    assert(hs_sha256d_header80(header, 81, digest) == HS_POW_INVALID);
    assert(hs_sha256d_header80(NULL, 80, digest) == HS_POW_INVALID);
    assert(hs_sha256d_header80(header, 80, NULL) == HS_POW_INVALID);
    expect_sentinel(digest, sizeof(digest));
}

static void test_hash_null_arguments(void)
{
    uint8_t digest[32];
    memset(digest, 0xa5, sizeof(digest));
    assert(hs_sha256(NULL, 1, digest) == HS_POW_INVALID);
    assert(hs_sha256d(NULL, 1, digest) == HS_POW_INVALID);
    assert(hs_sha256(NULL, 0, NULL) == HS_POW_INVALID);
    assert(hs_sha256d(NULL, 0, NULL) == HS_POW_INVALID);
    expect_sentinel(digest, sizeof(digest));
}

static void test_hash_length_guards_before_read(void)
{
    uint8_t one = 0, digest[32];
    memset(digest, 0xa5, sizeof(digest));
    assert(hs_sha256(&one, HS_SHA256_MAX_INPUT_BYTES + 1, digest) == HS_POW_INPUT_TOO_LONG);
    assert(hs_sha256d(&one, HS_SHA256_MAX_INPUT_BYTES + 1, digest) == HS_POW_INPUT_TOO_LONG);
    assert(hs_sha256(&one, SIZE_MAX, digest) == HS_POW_INPUT_TOO_LONG);
    assert(hs_sha256d(&one, SIZE_MAX, digest) == HS_POW_INPUT_TOO_LONG);
    expect_sentinel(digest, sizeof(digest));
}

static void test_sha_overlap(void)
{
    uint8_t data[300], expected[32];
    size_t i;
    for (i = 0; i < sizeof(data); ++i) {
        data[i] = (uint8_t)i;
    }
    assert(hs_sha256(data, 256, expected) == HS_POW_OK);
    assert(hs_sha256(data, 256, data + 127) == HS_POW_OK);
    assert(memcmp(expected, data + 127, 32) == 0);
    assert(hs_sha256(data, 32, expected) == HS_POW_OK);
    assert(hs_sha256(data, 32, data) == HS_POW_OK);
    assert(memcmp(expected, data, 32) == 0);
}

static void test_double_hash_overlap(void)
{
    uint8_t header[80], expected[32];
    genesis(header);
    assert(hs_sha256d_header80(header, 80, expected) == HS_POW_OK);
    assert(hs_sha256d_header80(header, 80, header + 16) == HS_POW_OK);
    assert(memcmp(expected, header + 16, 32) == 0);
}

static void test_unaligned_and_exact_output_span(void)
{
    uint8_t input[6] = {0, 'a', 'b', 'c', 0, 0}, output[34];
    memset(output, 0xa5, sizeof(output));
    assert(hs_sha256(input + 1, 3, output + 1) == HS_POW_OK);
    expect_hex(output + 1, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    assert(output[0] == 0xa5 && output[33] == 0xa5);
}

static void test_compact_small_targets(void)
{
    static const struct { uint32_t compact; uint32_t magnitude; } cases[] = {
        {UINT32_C(0x01123456), UINT32_C(0x12)},
        {UINT32_C(0x02123456), UINT32_C(0x1234)},
        {UINT32_C(0x03123456), UINT32_C(0x123456)},
        {UINT32_C(0x04123456), UINT32_C(0x12345600)},
        {UINT32_C(0x01010000), UINT32_C(1)}
    };
    uint8_t target[32];
    size_t i, j;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        assert(hs_pow_target_from_compact(cases[i].compact, target) == HS_POW_OK);
        for (j = 0; j < 32; ++j) {
            assert(target[j] == (j < 4 ? (uint8_t)(cases[i].magnitude >> (8U * j)) : 0));
        }
    }
}

static void test_compact_mainnet_target(void)
{
    uint8_t target[34];
    size_t i;
    memset(target, 0xa5, sizeof(target));
    assert(hs_pow_target_from_compact(UINT32_C(0x1d00ffff), target + 1) == HS_POW_OK);
    for (i = 0; i < 32; ++i) {
        assert(target[i + 1] == (i == 26 || i == 27 ? 0xff : 0));
    }
    assert(target[0] == 0xa5 && target[33] == 0xa5);
}

static void test_compact_noncanonical_accepted(void)
{
    uint8_t first[32], second[32];
    uint32_t encoded;
    assert(hs_pow_target_from_compact(UINT32_C(0x01123456), first) == HS_POW_OK);
    assert(hs_pow_target_from_compact(UINT32_C(0x01120000), second) == HS_POW_OK);
    assert(memcmp(first, second, 32) == 0);
    assert(hs_pow_compact_from_target(first, &encoded) == HS_POW_OK);
    assert(encoded == UINT32_C(0x01120000));
}

static void test_compact_zero_cases_output_untouched(void)
{
    static const uint32_t cases[] = {0, UINT32_C(0x00123456), UINT32_C(0x01003456),
        UINT32_C(0x02000056), UINT32_C(0x03000000), UINT32_C(0x04000000),
        UINT32_C(0x00923456), UINT32_C(0x01803456), UINT32_C(0x02800056),
        UINT32_C(0xff000000), UINT32_C(0xff800000)};
    uint8_t target[32];
    size_t i;
    memset(target, 0xa5, sizeof(target));
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        assert(hs_pow_target_from_compact(cases[i], target) == HS_POW_TARGET_ZERO);
        expect_sentinel(target, sizeof(target));
    }
}

static void test_compact_negative_cases_output_untouched(void)
{
    static const uint32_t cases[] = {UINT32_C(0x01fedcba), UINT32_C(0x04923456),
        UINT32_C(0x1d80ffff), UINT32_C(0xff800001)};
    uint8_t target[32];
    size_t i;
    memset(target, 0xa5, sizeof(target));
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        assert(hs_pow_target_from_compact(cases[i], target) == HS_POW_TARGET_NEGATIVE);
        expect_sentinel(target, sizeof(target));
    }
}

static void test_compact_overflow_boundaries(void)
{
    static const uint32_t cases[] = {UINT32_C(0x21010000), UINT32_C(0x217fffff),
        UINT32_C(0x22000100), UINT32_C(0x23000001), UINT32_C(0xff123456)};
    uint8_t target[32];
    size_t i;
    memset(target, 0xa5, sizeof(target));
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        assert(hs_pow_target_from_compact(cases[i], target) == HS_POW_TARGET_OVERFLOW);
        expect_sentinel(target, sizeof(target));
    }
}

static void test_compact_valid_large_exponents(void)
{
    uint8_t target[32];
    size_t i;
    assert(hs_pow_target_from_compact(UINT32_C(0x2100ffff), target) == HS_POW_OK);
    for (i = 0; i < 32; ++i) {
        assert(target[i] == (i >= 30 ? 0xff : 0));
    }
    assert(hs_pow_target_from_compact(UINT32_C(0x220000ff), target) == HS_POW_OK);
    for (i = 0; i < 32; ++i) {
        assert(target[i] == (i == 31 ? 0xff : 0));
    }
    assert(hs_pow_target_from_compact(UINT32_C(0x207fffff), target) == HS_POW_OK);
    assert(target[31] == 0x7f && target[30] == 0xff && target[29] == 0xff);
}

static void test_compact_null_output(void)
{
    assert(hs_pow_target_from_compact(UINT32_C(0x1d00ffff), NULL) == HS_POW_INVALID);
}

static void test_encode_zero_and_sign_boundaries(void)
{
    uint8_t target[32] = {0};
    uint32_t compact = UINT32_MAX;
    assert(hs_pow_compact_from_target(target, &compact) == HS_POW_OK && compact == 0);
    target[0] = 0x7f;
    assert(hs_pow_compact_from_target(target, &compact) == HS_POW_OK && compact == UINT32_C(0x017f0000));
    target[0] = 0x80;
    assert(hs_pow_compact_from_target(target, &compact) == HS_POW_OK && compact == UINT32_C(0x02008000));
    target[0] = 0; target[1] = 0x80;
    assert(hs_pow_compact_from_target(target, &compact) == HS_POW_OK && compact == UINT32_C(0x03008000));
}

static void test_encode_precision_truncation(void)
{
    uint8_t target[32] = {0x78, 0x56, 0x34, 0x12}, restored[32];
    uint32_t compact;
    int meets = -1;
    assert(hs_pow_compact_from_target(target, &compact) == HS_POW_OK);
    assert(compact == UINT32_C(0x04123456));
    assert(hs_pow_target_from_compact(compact, restored) == HS_POW_OK);
    assert(restored[0] == 0 && restored[1] == 0x56 && restored[2] == 0x34 && restored[3] == 0x12);
    assert(hs_pow_hash_meets_target(restored, target, &meets) == HS_POW_OK && meets == 1);
    assert(hs_pow_hash_meets_target(target, restored, &meets) == HS_POW_OK && meets == 0);
}

static void test_encode_maximum(void)
{
    uint8_t target[32], restored[32];
    uint32_t compact;
    memset(target, 0xff, sizeof(target));
    assert(hs_pow_compact_from_target(target, &compact) == HS_POW_OK);
    assert(compact == UINT32_C(0x2100ffff));
    assert(hs_pow_target_from_compact(compact, restored) == HS_POW_OK);
    assert(restored[31] == 0xff && restored[30] == 0xff && restored[29] == 0);
}

static void test_encode_every_power_of_two(void)
{
    uint8_t target[32], restored[32];
    uint32_t compact;
    size_t bit;
    for (bit = 0; bit < 256; ++bit) {
        memset(target, 0, sizeof(target));
        target[bit / 8] = (uint8_t)(1U << (bit % 8));
        assert(hs_pow_compact_from_target(target, &compact) == HS_POW_OK);
        assert((compact & UINT32_C(0x00800000)) == 0);
        assert(hs_pow_target_from_compact(compact, restored) == HS_POW_OK);
        assert(memcmp(target, restored, 32) == 0);
    }
}

static void test_encode_overlap_and_invalid_outputs(void)
{
    uint32_t storage[8];
    uint8_t *target = (uint8_t *)storage;
    uint32_t value = UINT32_C(0xa5a5a5a5);
    memset(storage, 0, sizeof(storage));
    target[26] = 0xff; target[27] = 0xff;
    assert(hs_pow_compact_from_target(target, storage) == HS_POW_OK);
    assert(storage[0] == UINT32_C(0x1d00ffff));
    assert(hs_pow_compact_from_target(NULL, &value) == HS_POW_INVALID);
    assert(value == UINT32_C(0xa5a5a5a5));
    assert(hs_pow_compact_from_target(target, NULL) == HS_POW_INVALID);
}

static void test_comparator_equal_below_above(void)
{
    uint8_t target[32] = {0}, digest[32] = {0};
    int meets = -1;
    target[0] = 2;
    digest[0] = 1;
    assert(hs_pow_hash_meets_target(digest, target, &meets) == HS_POW_OK && meets == 1);
    digest[0] = 2;
    assert(hs_pow_hash_meets_target(digest, target, &meets) == HS_POW_OK && meets == 1);
    digest[0] = 3;
    assert(hs_pow_hash_meets_target(digest, target, &meets) == HS_POW_OK && meets == 0);
}

static void test_comparator_little_endian_and_carry(void)
{
    uint8_t target[32] = {0}, digest[32] = {0};
    int meets;
    target[31] = 1;
    memset(digest, 0xff, 31);
    assert(hs_pow_hash_meets_target(digest, target, &meets) == HS_POW_OK && meets == 1);
    digest[31] = 1;
    assert(hs_pow_hash_meets_target(digest, target, &meets) == HS_POW_OK && meets == 0);
    memset(digest, 0xff, 32);
    memset(target, 0xff, 32);
    assert(hs_pow_hash_meets_target(digest, target, &meets) == HS_POW_OK && meets == 1);
    --target[0];
    assert(hs_pow_hash_meets_target(digest, target, &meets) == HS_POW_OK && meets == 0);
}

static void test_genesis_pow_and_wrong_endian(void)
{
    uint8_t header[80], digest[32], reverse[32], limit[32];
    int meets;
    size_t i;
    genesis(header); mainnet_limit(limit);
    assert(hs_sha256d_header80(header, 80, digest) == HS_POW_OK);
    assert(hs_pow_check_compact(digest, UINT32_C(0x1d00ffff), limit, &meets) == HS_POW_OK && meets == 1);
    for (i = 0; i < 32; ++i) {
        reverse[i] = digest[31 - i];
    }
    assert(hs_pow_check_compact(reverse, UINT32_C(0x1d00ffff), limit, &meets) == HS_POW_OK && meets == 0);
}

static void test_changed_nonce_does_not_pass(void)
{
    uint8_t header[80], digest[32], limit[32];
    int meets;
    genesis(header); mainnet_limit(limit);
    memset(header + 76, 0, 4);
    assert(hs_sha256d_header80(header, 80, digest) == HS_POW_OK);
    expect_hex(digest, "bf483998a9b44cbf5a113973e34da96b5cf3c7757d75ac3bd7c6b30af5a7c12b");
    assert(hs_pow_check_compact(digest, UINT32_C(0x1d00ffff), limit, &meets) == HS_POW_OK && meets == 0);
}

static void test_explicit_pow_limit(void)
{
    uint8_t digest[32] = {0}, limit[32];
    int meets = -7;
    assert(hs_pow_target_from_compact(UINT32_C(0x1d00ffff), limit) == HS_POW_OK);
    assert(hs_pow_check_compact(digest, UINT32_C(0x1d00ffff), limit, &meets) == HS_POW_OK && meets == 1);
    meets = -7;
    assert(hs_pow_check_compact(digest, UINT32_C(0x1d010000), limit, &meets) == HS_POW_TARGET_ABOVE_LIMIT);
    assert(meets == -7);
    memset(limit, 0, 32);
    assert(hs_pow_check_compact(digest, UINT32_C(0x1d00ffff), limit, &meets) == HS_POW_TARGET_ZERO);
    assert(meets == -7);
}

static void test_comparison_invalid_outputs_untouched(void)
{
    uint8_t digest[32] = {0}, limit[32], zero[32] = {0};
    int meets = -7;
    mainnet_limit(limit);
    assert(hs_pow_hash_meets_target(NULL, limit, &meets) == HS_POW_INVALID);
    assert(hs_pow_hash_meets_target(digest, NULL, &meets) == HS_POW_INVALID);
    assert(hs_pow_hash_meets_target(digest, limit, NULL) == HS_POW_INVALID);
    assert(hs_pow_hash_meets_target(digest, zero, &meets) == HS_POW_TARGET_ZERO);
    assert(hs_pow_check_compact(NULL, UINT32_C(0x1d00ffff), limit, &meets) == HS_POW_INVALID);
    assert(hs_pow_check_compact(digest, UINT32_C(0x1d00ffff), NULL, &meets) == HS_POW_INVALID);
    assert(hs_pow_check_compact(digest, UINT32_C(0x1d00ffff), limit, NULL) == HS_POW_INVALID);
    assert(hs_pow_check_compact(digest, UINT32_C(0x1d80ffff), limit, &meets) == HS_POW_TARGET_NEGATIVE);
    assert(hs_pow_check_compact(digest, UINT32_C(0x23000001), limit, &meets) == HS_POW_TARGET_OVERFLOW);
    assert(hs_pow_check_compact(digest, 0, limit, &meets) == HS_POW_TARGET_ZERO);
    assert(meets == -7);
}

static void test_comparison_output_alias(void)
{
    int storage[32];
    uint8_t *digest = (uint8_t *)storage;
    uint8_t target[32] = {0};
    memset(storage, 0, sizeof(storage));
    target[0] = 2; digest[0] = 1;
    assert(hs_pow_hash_meets_target(digest, target, storage) == HS_POW_OK);
    assert(storage[0] == 1);
    memset(storage, 0, sizeof(storage));
    digest[0] = 2;
    assert(hs_pow_hash_meets_target(target, digest, storage) == HS_POW_OK);
    assert(storage[0] == 1);
}

#define RUN(name) do { name(); ++tests_passed; } while (0)
int main(void)
{
    RUN(test_nist_abc);
    RUN(test_nist_two_blocks);
    RUN(test_empty);
    RUN(test_binary_padding_boundaries);
    RUN(test_million_a);
    RUN(test_exact_input_cap);
    RUN(test_double_hash);
    RUN(test_genesis_header);
    RUN(test_header_length_guards);
    RUN(test_hash_null_arguments);
    RUN(test_hash_length_guards_before_read);
    RUN(test_sha_overlap);
    RUN(test_double_hash_overlap);
    RUN(test_unaligned_and_exact_output_span);
    RUN(test_compact_small_targets);
    RUN(test_compact_mainnet_target);
    RUN(test_compact_noncanonical_accepted);
    RUN(test_compact_zero_cases_output_untouched);
    RUN(test_compact_negative_cases_output_untouched);
    RUN(test_compact_overflow_boundaries);
    RUN(test_compact_valid_large_exponents);
    RUN(test_compact_null_output);
    RUN(test_encode_zero_and_sign_boundaries);
    RUN(test_encode_precision_truncation);
    RUN(test_encode_maximum);
    RUN(test_encode_every_power_of_two);
    RUN(test_encode_overlap_and_invalid_outputs);
    RUN(test_comparator_equal_below_above);
    RUN(test_comparator_little_endian_and_carry);
    RUN(test_genesis_pow_and_wrong_endian);
    RUN(test_changed_nonce_does_not_pass);
    RUN(test_explicit_pow_limit);
    RUN(test_comparison_invalid_outputs_untouched);
    RUN(test_comparison_output_alias);
    printf("pow: %u test groups passed; synthetic/NIST/genesis only, no vendor nonce path\n", tests_passed);
    return 0;
}
