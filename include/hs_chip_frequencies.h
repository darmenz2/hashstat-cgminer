/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef HS_CHIP_FREQUENCIES_H
#define HS_CHIP_FREQUENCIES_H

#include <stddef.h>
#include <stdint.h>

#define HS_CHIP_FREQUENCIES_MAX_CHIPS ((size_t)4096)
#define HS_CHIP_FREQUENCIES_MAX_INPUT_BYTES ((size_t)65536)

enum hs_chip_frequencies_status {
    HS_CHIP_FREQUENCIES_OK = 0,
    HS_CHIP_FREQUENCIES_INVALID_ARGUMENT,
    HS_CHIP_FREQUENCIES_INPUT_TOO_LONG,
    HS_CHIP_FREQUENCIES_TOO_MANY_CHIPS,
    HS_CHIP_FREQUENCIES_DESTINATION_TOO_SMALL,
    HS_CHIP_FREQUENCIES_OVERLAPPING_BUFFERS
};

struct hs_chip_frequencies_result {
    enum hs_chip_frequencies_status status;
    size_t parsed_tokens;
    size_t zero_tokens;
    size_t clamped_low;
    size_t clamped_high;
    size_t ignored_tokens;
};

struct hs_chip_frequencies_result hs_chip_frequencies_decode(
    const char *input,
    size_t input_len,
    int32_t *destination,
    size_t destination_capacity,
    size_t chip_count,
    int32_t minimum,
    int32_t maximum);

#endif
