#include "hs_chip_frequencies.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static size_t cases_run;

#define DECODE_TEXT(text, count, lower, upper, expected) \
    expect_decode((text), sizeof(text) - 1U, (count), (lower), (upper), (expected))

static struct hs_chip_frequencies_result expect_decode(
    const char *input, size_t input_len, size_t chip_count,
    int32_t minimum, int32_t maximum, const int32_t *expected)
{
    int32_t output[32];
    struct hs_chip_frequencies_result result;
    size_t index;
    CHECK(chip_count <= 31U);
    for (index = 0U; index < 32U; ++index) {
        output[index] = INT32_C(123456789);
    }
    result = hs_chip_frequencies_decode(input, input_len, output, 32U,
                                         chip_count, minimum, maximum);
    CHECK(result.status == HS_CHIP_FREQUENCIES_OK);
    for (index = 0U; index < chip_count; ++index) {
        CHECK(output[index] == expected[index]);
    }
    for (index = chip_count; index < 32U; ++index) {
        CHECK(output[index] == INT32_C(123456789));
    }
    ++cases_run;
    return result;
}

static void test_observed_conversion(void)
{
    struct hs_chip_frequencies_result result;
    const int32_t ordinary[] = { 100, 200, 300, 0 };
    const int32_t zeroes[] = { 0, 0, 0, 0 };
    const int32_t clamped[] = { 100, 100, 200, 300, 300, 0 };
    const int32_t invalid[] = { 0, 0, 0, 0, 0, 0, 0 };
    const int32_t prefix[] = { 123, 234, 17, 0, 1, 12, 100 };
    const int32_t signed_values[] = { -300, -100, 100, 300 };
    const int32_t extremes[] = { INT32_MAX, INT32_MIN, 0, 0 };
    const int32_t signs[] = { 0, 0, 0, 0 };
    const int32_t limits_zero[] = { 0, 0, 0 };

    result = DECODE_TEXT("100:200:300", 4U, 1, 1000, ordinary);
    CHECK(result.parsed_tokens == 3U && result.ignored_tokens == 0U);
    result = DECODE_TEXT(":::100::200:300::", 4U, 1, 1000, ordinary);
    CHECK(result.parsed_tokens == 3U);
    result = DECODE_TEXT("::::", 4U, 1, 1000, zeroes);
    CHECK(result.parsed_tokens == 0U);
    result = expect_decode(NULL, 0U, 4U, 1, 1000, zeroes);
    CHECK(result.parsed_tokens == 0U);
    DECODE_TEXT("", 4U, 1, 1000, zeroes);

    result = DECODE_TEXT("-1:10:200:300:999:0", 6U, 100, 300, clamped);
    CHECK(result.clamped_low == 2U && result.clamped_high == 1U);
    CHECK(result.zero_tokens == 1U);
    result = DECODE_TEXT("abc:+:-: :0:+0:-0", 7U, 100, 300, invalid);
    CHECK(result.parsed_tokens == 7U && result.zero_tokens == 7U);
    DECODE_TEXT("123abc:234.5:017:0x10:1e3:12 34:+100", 7U, 1, 1000, prefix);
    DECODE_TEXT("-1000:-100:+100:1000", 4U, -300, 300, signed_values);
    result = DECODE_TEXT("2147483647:-2147483648:2147483648:-2147483649",
                         4U, INT32_MIN, INT32_MAX, extremes);
    CHECK(result.zero_tokens == 2U);
    DECODE_TEXT("- 1:+ 1:--1:++1", 4U, -300, 300, signs);
    DECODE_TEXT("-1:0:1", 3U, 0, 0, limits_zero);
}

static void test_byte_span_and_whitespace(void)
{
    const char span[] = { '1', '0', '0', ':', '2', '0', '0' };
    const char embedded_nul[] = { '1', '0', '0', '\0', ':', '2', '0', '0' };
    const char non_ascii[] = { (char)0xa0, '2', ':', (char)0xff, '3' };
    const int32_t span_expected[] = { 100, 200, 0 };
    const int32_t short_expected[] = { 10, 0, 0 };
    const int32_t nul_expected[] = { 100, 0, 0 };
    const int32_t whitespace_expected[] = { 12, 13, 14, 15, 16, 17 };
    const int32_t non_ascii_expected[] = { 0, 0 };

    expect_decode(span, sizeof(span), 3U, 1, 1000, span_expected);
    expect_decode(span, 2U, 3U, 1, 1000, short_expected);
    expect_decode(embedded_nul, sizeof(embedded_nul), 3U, 1, 1000, nul_expected);
    DECODE_TEXT("\t12:\n13:\v14:\f15:\r16: 17", 6U, 1, 1000, whitespace_expected);
    expect_decode(non_ascii, sizeof(non_ascii), 2U, 1, 1000, non_ascii_expected);
}

static void test_extra_tokens(void)
{
    const int32_t expected[] = { 100, 200 };
    struct hs_chip_frequencies_result result =
        DECODE_TEXT("100:200:::bad:2147483648:300::", 2U, 1, 1000, expected);
    CHECK(result.parsed_tokens == 2U && result.ignored_tokens == 3U);
    CHECK(result.zero_tokens == 0U);
    result = hs_chip_frequencies_decode("1::2:", 5U, NULL, 0U, 0U, 1, 1000);
    CHECK(result.status == HS_CHIP_FREQUENCIES_OK);
    CHECK(result.parsed_tokens == 0U && result.ignored_tokens == 2U);
    ++cases_run;
}

static void test_rejected_inputs_are_nonmutating(void)
{
    int32_t destination[3] = { 42, 43, 44 };
    struct hs_chip_frequencies_result result;
    result = hs_chip_frequencies_decode(NULL, 1U, destination, 3U, 3U, 1, 100);
    CHECK(result.status == HS_CHIP_FREQUENCIES_INVALID_ARGUMENT);
    result = hs_chip_frequencies_decode("1", 1U, NULL, 3U, 3U, 1, 100);
    CHECK(result.status == HS_CHIP_FREQUENCIES_INVALID_ARGUMENT);
    result = hs_chip_frequencies_decode("1", 1U, destination, 3U, 3U, 101, 100);
    CHECK(result.status == HS_CHIP_FREQUENCIES_INVALID_ARGUMENT);
    result = hs_chip_frequencies_decode("1", 1U, destination, 2U, 3U, 1, 100);
    CHECK(result.status == HS_CHIP_FREQUENCIES_DESTINATION_TOO_SMALL);
    result = hs_chip_frequencies_decode("1", 1U, destination, SIZE_MAX, SIZE_MAX, 1, 100);
    CHECK(result.status == HS_CHIP_FREQUENCIES_TOO_MANY_CHIPS);
    result = hs_chip_frequencies_decode("1", SIZE_MAX, destination, 3U, 3U, 1, 100);
    CHECK(result.status == HS_CHIP_FREQUENCIES_INPUT_TOO_LONG);
    result = hs_chip_frequencies_decode((const char *)destination,
        sizeof(destination), destination, 3U, 3U, 1, 100);
    CHECK(result.status == HS_CHIP_FREQUENCIES_OVERLAPPING_BUFFERS);
    result = hs_chip_frequencies_decode((const char *)destination + 4U,
        4U, destination, 3U, 3U, 1, 100);
    CHECK(result.status == HS_CHIP_FREQUENCIES_OVERLAPPING_BUFFERS);
    CHECK(destination[0] == 42 && destination[1] == 43 && destination[2] == 44);
    ++cases_run;
}

static void test_resource_boundaries(void)
{
    char bytes[HS_CHIP_FREQUENCIES_MAX_INPUT_BYTES];
    int32_t destination[HS_CHIP_FREQUENCIES_MAX_CHIPS];
    struct hs_chip_frequencies_result result;
    size_t index;
    memset(bytes, '9', sizeof(bytes));
    result = hs_chip_frequencies_decode(bytes, sizeof(bytes), destination,
        HS_CHIP_FREQUENCIES_MAX_CHIPS, HS_CHIP_FREQUENCIES_MAX_CHIPS, 1, 1000);
    CHECK(result.status == HS_CHIP_FREQUENCIES_OK);
    CHECK(result.parsed_tokens == 1U && result.zero_tokens == 1U);
    for (index = 0U; index < HS_CHIP_FREQUENCIES_MAX_CHIPS; ++index) {
        CHECK(destination[index] == 0);
    }
    memset(bytes, ':', sizeof(bytes));
    result = hs_chip_frequencies_decode(bytes, sizeof(bytes), destination,
        HS_CHIP_FREQUENCIES_MAX_CHIPS, HS_CHIP_FREQUENCIES_MAX_CHIPS, 1, 1000);
    CHECK(result.status == HS_CHIP_FREQUENCIES_OK && result.parsed_tokens == 0U);
    ++cases_run;
}

static void test_host_conversion_differential(void)
{
    static const unsigned char alphabet[] =
        ": +-01234567890123456789abcXYZ.\t\n\r\v\f\x80\xff";
    uint32_t seed = UINT32_C(0x84c23917);
    size_t trial;

    for (trial = 0U; trial < 20000U; ++trial) {
        char input[129];
        char copy[129];
        int32_t actual[16];
        int32_t expected[16] = { 0 };
        size_t position;
        size_t length;
        size_t tokens = 0U;
        struct hs_chip_frequencies_result result;

        seed = seed * UINT32_C(1664525) + UINT32_C(1013904223);
        length = (size_t)(seed % UINT32_C(129));
        for (position = 0U; position < length; ++position) {
            seed = seed * UINT32_C(1664525) + UINT32_C(1013904223);
            input[position] = (char)alphabet[(size_t)(seed >> 16U) %
                                             (sizeof(alphabet) - 1U)];
        }
        input[length] = '\0';
        memcpy(copy, input, length + 1U);
        position = 0U;
        while (position < length) {
            size_t start;
            long long value;
            while (position < length && copy[position] == ':') {
                ++position;
            }
            start = position;
            while (position < length && copy[position] != ':') {
                ++position;
            }
            if (start == position) {
                break;
            }
            if (position < length) {
                copy[position] = '\0';
                ++position;
            }
            if (tokens < 16U) {
                errno = 0;
                value = strtoll(copy + start, NULL, 10);
                if (errno != 0 || value < (long long)INT32_MIN ||
                    value > (long long)INT32_MAX || value == 0) {
                    expected[tokens] = 0;
                } else if (value < -300LL) {
                    expected[tokens] = -300;
                } else if (value > 300LL) {
                    expected[tokens] = 300;
                } else {
                    expected[tokens] = (int32_t)value;
                }
            }
            ++tokens;
        }
        result = hs_chip_frequencies_decode(input, length, actual, 16U, 16U,
                                             -300, 300);
        CHECK(result.status == HS_CHIP_FREQUENCIES_OK);
        CHECK(memcmp(actual, expected, sizeof(actual)) == 0);
        CHECK(result.parsed_tokens == (tokens < 16U ? tokens : 16U));
        CHECK(result.ignored_tokens == (tokens > 16U ? tokens - 16U : 0U));
    }
    ++cases_run;
}

int main(void)
{
    test_observed_conversion();
    test_byte_span_and_whitespace();
    test_extra_tokens();
    test_rejected_inputs_are_nonmutating();
    test_resource_boundaries();
    test_host_conversion_differential();
    printf("chip frequencies: %zu grouped cases passed\n", cases_run);
    puts("including 20000 deterministic host-oracle differential inputs");
    return EXIT_SUCCESS;
}
