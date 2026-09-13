#include "hs_chip_frequencies.h"
#include "hs_span.h"

static int ascii_space(unsigned char value)
{
    return value == 0x20U || (value >= 0x09U && value <= 0x0dU);
}

static int32_t decimal_prefix_i32(const char *token, size_t length)
{
    size_t position = 0U;
    uint32_t magnitude = 0U;
    uint32_t limit = UINT32_C(2147483647);
    int negative = 0;

    while (position < length && ascii_space((unsigned char)token[position])) {
        ++position;
    }
    if (position < length && (token[position] == '+' || token[position] == '-')) {
        negative = token[position] == '-';
        ++position;
    }
    if (negative) {
        limit = UINT32_C(2147483648);
    }

    while (position < length) {
        const unsigned char byte = (unsigned char)token[position];
        uint32_t digit;
        if (byte < (unsigned char)'0' || byte > (unsigned char)'9') {
            break;
        }
        digit = (uint32_t)(byte - (unsigned char)'0');
        if (magnitude > (limit - digit) / UINT32_C(10)) {
            return 0;
        }
        magnitude = magnitude * UINT32_C(10) + digit;
        ++position;
    }
    if (negative && magnitude == UINT32_C(2147483648)) {
        return INT32_MIN;
    }
    return negative ? -(int32_t)magnitude : (int32_t)magnitude;
}

struct hs_chip_frequencies_result hs_chip_frequencies_decode(
    const char *input,
    size_t input_len,
    int32_t *destination,
    size_t destination_capacity,
    size_t chip_count,
    int32_t minimum,
    int32_t maximum)
{
    struct hs_chip_frequencies_result result = {
        HS_CHIP_FREQUENCIES_OK, 0U, 0U, 0U, 0U, 0U
    };
    size_t effective_length = 0U;
    size_t position = 0U;
    size_t index;

    if ((input == NULL && input_len != 0U) ||
        (destination == NULL && chip_count != 0U) || minimum > maximum) {
        result.status = HS_CHIP_FREQUENCIES_INVALID_ARGUMENT;
        return result;
    }
    if (input_len > HS_CHIP_FREQUENCIES_MAX_INPUT_BYTES) {
        result.status = HS_CHIP_FREQUENCIES_INPUT_TOO_LONG;
        return result;
    }
    if (chip_count > HS_CHIP_FREQUENCIES_MAX_CHIPS) {
        result.status = HS_CHIP_FREQUENCIES_TOO_MANY_CHIPS;
        return result;
    }
    if (destination_capacity < chip_count) {
        result.status = HS_CHIP_FREQUENCIES_DESTINATION_TOO_SMALL;
        return result;
    }
    if (hs_spans_overlap(input, input_len, destination,
                      chip_count * sizeof(*destination))) {
        result.status = HS_CHIP_FREQUENCIES_OVERLAPPING_BUFFERS;
        return result;
    }
    while (effective_length < input_len && input[effective_length] != '\0') {
        ++effective_length;
    }
    for (index = 0U; index < chip_count; ++index) {
        destination[index] = 0;
    }
    while (position < effective_length) {
        size_t start;
        int32_t value;

        while (position < effective_length && input[position] == ':') {
            ++position;
        }
        start = position;
        while (position < effective_length && input[position] != ':') {
            ++position;
        }
        if (start == position) {
            break;
        }
        if (result.parsed_tokens == chip_count) {
            ++result.ignored_tokens;
            continue;
        }
        value = decimal_prefix_i32(input + start, position - start);
        if (value == 0) {
            ++result.zero_tokens;
        } else if (value < minimum) {
            value = minimum;
            ++result.clamped_low;
        } else if (value > maximum) {
            value = maximum;
            ++result.clamped_high;
        }
        destination[result.parsed_tokens] = value;
        ++result.parsed_tokens;
    }
    return result;
}
