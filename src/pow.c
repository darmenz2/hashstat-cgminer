/* SPDX-License-Identifier: GPL-3.0-only */
/* SHA-256 and Bitcoin compact-target arithmetic. */
#include "hs_pow.h"

#include <limits.h>

_Static_assert(CHAR_BIT == 8, "eight-bit bytes required");
_Static_assert(sizeof(size_t) <= sizeof(uint64_t), "size_t wider than length guard");

static const uint32_t round_constant[64] = {
    UINT32_C(0x428a2f98), UINT32_C(0x71374491), UINT32_C(0xb5c0fbcf), UINT32_C(0xe9b5dba5),
    UINT32_C(0x3956c25b), UINT32_C(0x59f111f1), UINT32_C(0x923f82a4), UINT32_C(0xab1c5ed5),
    UINT32_C(0xd807aa98), UINT32_C(0x12835b01), UINT32_C(0x243185be), UINT32_C(0x550c7dc3),
    UINT32_C(0x72be5d74), UINT32_C(0x80deb1fe), UINT32_C(0x9bdc06a7), UINT32_C(0xc19bf174),
    UINT32_C(0xe49b69c1), UINT32_C(0xefbe4786), UINT32_C(0x0fc19dc6), UINT32_C(0x240ca1cc),
    UINT32_C(0x2de92c6f), UINT32_C(0x4a7484aa), UINT32_C(0x5cb0a9dc), UINT32_C(0x76f988da),
    UINT32_C(0x983e5152), UINT32_C(0xa831c66d), UINT32_C(0xb00327c8), UINT32_C(0xbf597fc7),
    UINT32_C(0xc6e00bf3), UINT32_C(0xd5a79147), UINT32_C(0x06ca6351), UINT32_C(0x14292967),
    UINT32_C(0x27b70a85), UINT32_C(0x2e1b2138), UINT32_C(0x4d2c6dfc), UINT32_C(0x53380d13),
    UINT32_C(0x650a7354), UINT32_C(0x766a0abb), UINT32_C(0x81c2c92e), UINT32_C(0x92722c85),
    UINT32_C(0xa2bfe8a1), UINT32_C(0xa81a664b), UINT32_C(0xc24b8b70), UINT32_C(0xc76c51a3),
    UINT32_C(0xd192e819), UINT32_C(0xd6990624), UINT32_C(0xf40e3585), UINT32_C(0x106aa070),
    UINT32_C(0x19a4c116), UINT32_C(0x1e376c08), UINT32_C(0x2748774c), UINT32_C(0x34b0bcb5),
    UINT32_C(0x391c0cb3), UINT32_C(0x4ed8aa4a), UINT32_C(0x5b9cca4f), UINT32_C(0x682e6ff3),
    UINT32_C(0x748f82ee), UINT32_C(0x78a5636f), UINT32_C(0x84c87814), UINT32_C(0x8cc70208),
    UINT32_C(0x90befffa), UINT32_C(0xa4506ceb), UINT32_C(0xbef9a3f7), UINT32_C(0xc67178f2)
};

static uint32_t rotate_right(uint32_t value, unsigned int amount)
{
    return (value >> amount) | (value << (32U - amount));
}

static uint32_t big_endian_word(const uint8_t bytes[4])
{
    return ((uint32_t)bytes[0] << 24U) | ((uint32_t)bytes[1] << 16U) |
           ((uint32_t)bytes[2] << 8U) | (uint32_t)bytes[3];
}

static void compress_block(uint32_t state[8], const uint8_t block[64])
{
    uint32_t schedule[64];
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    size_t index;
    for (index = 0; index < 16; ++index) {
        schedule[index] = big_endian_word(block + 4 * index);
    }
    for (index = 16; index < 64; ++index) {
        const uint32_t earlier = schedule[index - 15];
        const uint32_t recent = schedule[index - 2];
        const uint32_t small0 = rotate_right(earlier, 7) ^ rotate_right(earlier, 18) ^ (earlier >> 3U);
        const uint32_t small1 = rotate_right(recent, 17) ^ rotate_right(recent, 19) ^ (recent >> 10U);
        schedule[index] = schedule[index - 16] + small0 + schedule[index - 7] + small1;
    }
    for (index = 0; index < 64; ++index) {
        const uint32_t large0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        const uint32_t large1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        const uint32_t choose = (e & f) ^ (~e & g);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t step1 = h + large1 + choose + round_constant[index] + schedule[index];
        const uint32_t step2 = large0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + step1;
        d = c;
        c = b;
        b = a;
        a = step1 + step2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static hs_pow_status hash_arguments(const uint8_t *data, size_t length,
                                    const uint8_t *out)
{
    if (out == NULL || (data == NULL && length != 0)) {
        return HS_POW_INVALID;
    }
    if (length > HS_SHA256_MAX_INPUT_BYTES || (uint64_t)length > UINT64_MAX / UINT64_C(8)) {
        return HS_POW_INPUT_TOO_LONG;
    }
    return HS_POW_OK;
}

static void hash_unchecked(const uint8_t *data, size_t length, uint8_t result[32])
{
    uint32_t state[8];
    uint8_t tail[128];
    size_t offset = 0;
    size_t remaining;
    size_t padded;
    size_t index;
    const uint64_t bit_length = (uint64_t)length * UINT64_C(8);
    state[0] = UINT32_C(0x6a09e667); state[1] = UINT32_C(0xbb67ae85);
    state[2] = UINT32_C(0x3c6ef372); state[3] = UINT32_C(0xa54ff53a);
    state[4] = UINT32_C(0x510e527f); state[5] = UINT32_C(0x9b05688c);
    state[6] = UINT32_C(0x1f83d9ab); state[7] = UINT32_C(0x5be0cd19);
    while (length - offset >= 64) {
        compress_block(state, data + offset);
        offset += 64;
    }
    remaining = length - offset;
    padded = remaining < 56 ? 64U : 128U;
    for (index = 0; index < padded; ++index) {
        tail[index] = 0;
    }
    for (index = 0; index < remaining; ++index) {
        tail[index] = data[offset + index];
    }
    tail[remaining] = 0x80;
    for (index = 0; index < 8; ++index) {
        tail[padded - 1 - index] = (uint8_t)(bit_length >> (8U * index));
    }
    compress_block(state, tail);
    if (padded == 128) {
        compress_block(state, tail + 64);
    }

    for (index = 0; index < 32; ++index) {
        result[index] = (uint8_t)(state[index / 4] >> (24U - 8U * (index % 4)));
    }
}

hs_pow_status hs_sha256(const uint8_t *data, size_t length, uint8_t out32[32])
{
    const hs_pow_status status = hash_arguments(data, length, out32);
    if (status != HS_POW_OK) {
        return status;
    }
    hash_unchecked(data, length, out32);
    return HS_POW_OK;
}

hs_pow_status hs_sha256d(const uint8_t *data, size_t length, uint8_t out32[32])
{
    uint8_t first_digest[32];
    const hs_pow_status status = hash_arguments(data, length, out32);
    if (status != HS_POW_OK) {
        return status;
    }
    hash_unchecked(data, length, first_digest);
    hash_unchecked(first_digest, sizeof(first_digest), out32);
    return HS_POW_OK;
}

hs_pow_status hs_sha256d_header80(const uint8_t *header, size_t length,
                                 uint8_t out32[32])
{
    if (length != HS_BITCOIN_HEADER_BYTES) {
        return HS_POW_INVALID;
    }
    return hs_sha256d(header, length, out32);
}

static int nonzero_target(const uint8_t value[32])
{
    size_t index;
    for (index = 0; index < 32; ++index) {
        if (value[index] != 0) {
            return 1;
        }
    }
    return 0;
}

static int compare_little_endian(const uint8_t first[32], const uint8_t second[32])
{
    size_t index = 32;
    while (index != 0) {
        --index;
        if (first[index] != second[index]) {
            return first[index] < second[index] ? -1 : 1;
        }
    }
    return 0;
}

hs_pow_status hs_pow_target_from_compact(uint32_t compact, uint8_t target_le[32])
{
    const uint32_t exponent = compact >> 24U;
    uint32_t mantissa = compact & UINT32_C(0x007fffff);
    uint8_t target[32];
    size_t index;
    if (target_le == NULL) {
        return HS_POW_INVALID;
    }
    if (exponent <= 3) {
        mantissa >>= 8U * (3U - exponent);
    }
    if (mantissa == 0) {
        return HS_POW_TARGET_ZERO;
    }
    if ((compact & UINT32_C(0x00800000)) != 0) {
        return HS_POW_TARGET_NEGATIVE;
    }
    for (index = 0; index < 32; ++index) {
        target[index] = 0;
    }

    for (index = 0; index < 3; ++index) {
        const uint8_t digit = (uint8_t)(mantissa >> (8U * index));
        const size_t position = exponent <= 3 ? index : (size_t)(exponent - 3U) + index;
        if (digit != 0) {
            if (position >= 32) {
                return HS_POW_TARGET_OVERFLOW;
            }
            target[position] = digit;
        }
    }
    for (index = 0; index < 32; ++index) {
        target_le[index] = target[index];
    }
    return HS_POW_OK;
}

hs_pow_status hs_pow_compact_from_target(const uint8_t target_le[32],
                                        uint32_t *compact)
{
    size_t size = 32;
    size_t index;
    uint32_t result = 0;
    if (target_le == NULL || compact == NULL) {
        return HS_POW_INVALID;
    }
    while (size != 0 && target_le[size - 1] == 0) {
        --size;
    }
    if (size <= 3) {
        for (index = 0; index < size; ++index) {
            result |= (uint32_t)target_le[index] << (8U * (3U - size + index));
        }
    } else {
        for (index = 0; index < 3; ++index) {
            result |= (uint32_t)target_le[size - 3 + index] << (8U * index);
        }
    }
    if ((result & UINT32_C(0x00800000)) != 0) {
        result >>= 8U;
        ++size;
    }
    result |= (uint32_t)size << 24U;
    *compact = result;
    return HS_POW_OK;
}

hs_pow_status hs_pow_hash_meets_target(const uint8_t digest32[32],
                                      const uint8_t target_le[32], int *meets)
{
    int result;
    if (digest32 == NULL || target_le == NULL || meets == NULL) {
        return HS_POW_INVALID;
    }
    if (!nonzero_target(target_le)) {
        return HS_POW_TARGET_ZERO;
    }
    result = compare_little_endian(digest32, target_le) <= 0;
    *meets = result;
    return HS_POW_OK;
}

hs_pow_status hs_pow_check_compact(const uint8_t digest32[32], uint32_t compact,
                                  const uint8_t pow_limit_le[32], int *meets)
{
    uint8_t target[32];
    hs_pow_status status;
    if (digest32 == NULL || pow_limit_le == NULL || meets == NULL) {
        return HS_POW_INVALID;
    }
    if (!nonzero_target(pow_limit_le)) {
        return HS_POW_TARGET_ZERO;
    }
    status = hs_pow_target_from_compact(compact, target);
    if (status != HS_POW_OK) {
        return status;
    }
    if (compare_little_endian(target, pow_limit_le) > 0) {
        return HS_POW_TARGET_ABOVE_LIMIT;
    }
    return hs_pow_hash_meets_target(digest32, target, meets);
}
