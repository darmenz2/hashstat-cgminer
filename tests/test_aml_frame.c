#include "hs_aml_frame.h"

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

static void expect_vector(const uint8_t *payload, size_t payload_len,
                          const uint8_t *expected, size_t expected_len)
{
    uint8_t output[320];
    uint8_t input_copy[256];
    struct hs_aml_frame_result result;
    size_t index;

    CHECK(payload_len <= sizeof(input_copy));
    CHECK(expected_len + 2U <= sizeof(output));
    if (payload_len != 0U) {
        memcpy(input_copy, payload, payload_len);
    }
    memset(output, 0xcc, sizeof(output));
    result = hs_aml_frame_pack(payload, payload_len, output + 1U, expected_len);
    CHECK(result.status == HS_AML_FRAME_OK);
    CHECK(result.frame_bytes == expected_len);
    CHECK(output[0] == UINT8_C(0xcc));
    CHECK(memcmp(output + 1U, expected, expected_len) == 0);
    for (index = expected_len + 1U; index < sizeof(output); ++index) {
        CHECK(output[index] == UINT8_C(0xcc));
    }
    if (payload_len != 0U) {
        CHECK(memcmp(payload, input_copy, payload_len) == 0);
    }
    ++cases_run;
}

static void test_vectors(void)
{
    static const uint8_t empty[] = { 0x55, 0xaa };
    static const uint8_t one[] = { 0x00 };
    static const uint8_t one_frame[] = { 0x55, 0xaa, 0x00 };
    static const uint8_t ordinary[] = { 0x01, 0x02, 0x03, 0x04 };
    static const uint8_t ordinary_frame[] = { 0x55, 0xaa, 0x01, 0x02, 0x03, 0x04 };
    static const uint8_t binary[] = { 0x55, 0xaa, 0x00, 0xff, 0x80, 0x7f };
    static const uint8_t binary_frame[] = {
        0x55, 0xaa, 0x55, 0xaa, 0x00, 0xff, 0x80, 0x7f
    };
    uint8_t all_bytes[256];
    uint8_t all_frame[258];
    size_t index;

    expect_vector(NULL, 0U, empty, sizeof(empty));
    expect_vector(one, 0U, empty, sizeof(empty));
    expect_vector(one, sizeof(one), one_frame, sizeof(one_frame));
    expect_vector(ordinary, sizeof(ordinary), ordinary_frame, sizeof(ordinary_frame));
    expect_vector(binary, sizeof(binary), binary_frame, sizeof(binary_frame));
    all_frame[0] = 0x55;
    all_frame[1] = 0xaa;
    for (index = 0U; index < sizeof(all_bytes); ++index) {
        all_bytes[index] = (uint8_t)index;
        all_frame[index + 2U] = (uint8_t)index;
    }
    expect_vector(all_bytes, sizeof(all_bytes), all_frame, sizeof(all_frame));
}

static void expect_error(const uint8_t *payload, size_t payload_len,
                         uint8_t *destination, size_t capacity,
                         enum hs_aml_frame_status status,
                         uint8_t *observed, size_t observed_len)
{
    uint8_t before[64];
    struct hs_aml_frame_result result;
    CHECK(observed_len <= sizeof(before));
    memcpy(before, observed, observed_len);
    result = hs_aml_frame_pack(payload, payload_len, destination, capacity);
    CHECK(result.status == status);
    CHECK(result.frame_bytes == 0U);
    CHECK(memcmp(before, observed, observed_len) == 0);
    ++cases_run;
}

static void test_rejections(void)
{
    const uint8_t payload[] = { 1, 2, 3 };
    uint8_t output[32];
    size_t capacity;
    memset(output, 0x9d, sizeof(output));

    expect_error(NULL, 1U, output, sizeof(output), HS_AML_FRAME_INVALID_ARGUMENT,
                 output, sizeof(output));
    expect_error(payload, sizeof(payload), NULL, 100U, HS_AML_FRAME_INVALID_ARGUMENT,
                 output, sizeof(output));
    expect_error(NULL, 0U, NULL, 0U, HS_AML_FRAME_INVALID_ARGUMENT,
                 output, sizeof(output));
    for (capacity = 0U; capacity < sizeof(payload) + 2U; ++capacity) {
        expect_error(payload, sizeof(payload), output, capacity,
                     HS_AML_FRAME_DESTINATION_TOO_SMALL, output, sizeof(output));
    }
    expect_error(NULL, 0U, output, 1U, HS_AML_FRAME_DESTINATION_TOO_SMALL,
                 output, sizeof(output));
    expect_error(payload, SIZE_MAX, output, SIZE_MAX, HS_AML_FRAME_SIZE_OVERFLOW,
                 output, sizeof(output));
    expect_error(payload, SIZE_MAX - 1U, output, SIZE_MAX, HS_AML_FRAME_SIZE_OVERFLOW,
                 output, sizeof(output));

    expect_error(payload, SIZE_MAX - 2U, output, SIZE_MAX, HS_AML_FRAME_SIZE_OVERFLOW,
                 output, sizeof(output));
    expect_error(output, 4U, output, sizeof(output), HS_AML_FRAME_OVERLAPPING_BUFFERS,
                 output, sizeof(output));
    expect_error(output + 2U, 4U, output, sizeof(output), HS_AML_FRAME_OVERLAPPING_BUFFERS,
                 output, sizeof(output));
    expect_error(output, 8U, output + 6U, 12U, HS_AML_FRAME_OVERLAPPING_BUFFERS,
                 output, sizeof(output));
}

static void test_exhaustive_small_spans(void)
{
    uint8_t actual[40];
    uint8_t expected[40];
    uint8_t original[40];
    size_t source_offset;
    size_t destination_offset;
    size_t length;
    size_t capacity;
    size_t index;

    for (index = 0U; index < sizeof(original); ++index) {
        original[index] = (uint8_t)(index * 5U + 3U);
    }
    for (source_offset = 0U; source_offset <= 12U; ++source_offset) {
        for (destination_offset = 0U; destination_offset <= 12U; ++destination_offset) {
            for (length = 0U; length <= 8U; ++length) {
                for (capacity = 0U; capacity <= 10U; ++capacity) {
                    struct hs_aml_frame_result result;
                    const size_t needed = length + 2U;
                    const int overlap = length != 0U &&
                        source_offset < destination_offset + needed &&
                        destination_offset < source_offset + length;
                    enum hs_aml_frame_status status = HS_AML_FRAME_OK;

                    memcpy(actual, original, sizeof(actual));
                    memcpy(expected, original, sizeof(expected));
                    if (capacity < needed) {
                        status = HS_AML_FRAME_DESTINATION_TOO_SMALL;
                    } else if (overlap) {
                        status = HS_AML_FRAME_OVERLAPPING_BUFFERS;
                    } else {
                        expected[destination_offset] = 0x55;
                        expected[destination_offset + 1U] = 0xaa;
                        for (index = 0U; index < length; ++index) {
                            expected[destination_offset + index + 2U] =
                                original[source_offset + index];
                        }
                    }
                    result = hs_aml_frame_pack(actual + source_offset, length,
                        actual + destination_offset, capacity);
                    CHECK(result.status == status);
                    CHECK(result.frame_bytes == (status == HS_AML_FRAME_OK ? needed : 0U));
                    CHECK(memcmp(actual, expected, sizeof(actual)) == 0);
                    ++cases_run;
                }
            }
        }
    }
}

static void test_capacity_tail_is_not_output(void)
{
    uint8_t storage[32];
    struct hs_aml_frame_result result;
    size_t index;

    for (index = 0U; index < sizeof(storage); ++index) {
        storage[index] = (uint8_t)index;
    }

    result = hs_aml_frame_pack(storage + 20U, 3U, storage, sizeof(storage));
    CHECK(result.status == HS_AML_FRAME_OK && result.frame_bytes == 5U);
    CHECK(storage[0] == 0x55 && storage[1] == 0xaa);
    CHECK(storage[2] == 20 && storage[3] == 21 && storage[4] == 22);
    for (index = 5U; index < sizeof(storage); ++index) {
        CHECK(storage[index] == (uint8_t)index);
    }
    ++cases_run;
}

int main(void)
{
    test_vectors();
    test_rejections();
    test_exhaustive_small_spans();
    test_capacity_tail_is_not_output();
    printf("aml_frame: %zu cases passed\n", cases_run);
    return EXIT_SUCCESS;
}
