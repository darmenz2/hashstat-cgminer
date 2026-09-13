/* SPDX-License-Identifier: GPL-3.0-only */
/* Mining data conversion and validation. */
#include "hashstat-core.h"
#include "hashstat-pure/hs_span.h"
#include <float.h>
#include "hashstat-pure/hs_pow.h"

bool hs_core_parse_vmask(const char *text, size_t length,
                         uint32_t allowed_mask, uint32_t *result)
{
    uint32_t parsed = 0;
    if (length != 8U || !hs_span_nonempty(text, length) ||
        !hs_span_nonempty(result, sizeof(*result))) return false;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char byte = (unsigned char)text[i];
        uint32_t digit;
        if (byte >= '0' && byte <= '9') digit = (uint32_t)(byte - '0');
        else if (byte >= 'a' && byte <= 'f') digit = (uint32_t)(byte - 'a' + 10);
        else if (byte >= 'A' && byte <= 'F') digit = (uint32_t)(byte - 'A' + 10);
        else return false;
        parsed = (parsed << 4) | digit;
    }
    if (parsed == 0 || (parsed & ~allowed_mask) != 0) return false;
    *result = parsed;
    return true;
}

bool hs_core_valid_difficulty(double difficulty)
{
    return difficulty == difficulty && difficulty <= DBL_MAX &&
           difficulty >= HS_CORE_MIN_DIFFICULTY &&
           difficulty <= HS_CORE_MAX_DIFFICULTY;
}

bool hs_core_flip_header80(const uint8_t *input, size_t length, uint8_t output[80])
{
    uint8_t converted[80];
    if (length != sizeof(converted) || !hs_span_nonempty(input, length) ||
        !hs_span_nonempty(output, sizeof(converted))) return false;
    for (size_t i = 0; i < sizeof(converted); ++i)
        converted[i] = input[(i & ~(size_t)3) + (3U - (i & 3U))];
    for (size_t i = 0; i < sizeof(converted); ++i) output[i] = converted[i];
    return true;
}

uint32_t hs_core_nonce_to_upstream(uint32_t nonce)
{
    return ((nonce & UINT32_C(0x000000ff)) << 24) |
           ((nonce & UINT32_C(0x0000ff00)) << 8) |
           ((nonce & UINT32_C(0x00ff0000)) >> 8) |
           ((nonce & UINT32_C(0xff000000)) >> 24);
}

bool hs_core_hash_work80(const uint8_t *work_data, size_t length, uint8_t digest[32])
{
    uint8_t header[80];
    if (!hs_span_nonempty(digest, 32U) ||
        !hs_core_flip_header80(work_data, length, header)) return false;
    return hs_sha256d_header80(header, sizeof(header), digest) == HS_POW_OK;
}
