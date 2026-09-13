/* SPDX-License-Identifier: GPL-3.0-only */

#ifndef HS_POW_H
#define HS_POW_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HS_SHA256_DIGEST_BYTES ((size_t)32)
#define HS_BITCOIN_HEADER_BYTES ((size_t)80)

#define HS_SHA256_MAX_INPUT_BYTES ((size_t)16777216)

typedef enum hs_pow_status {
    HS_POW_OK = 0,
    HS_POW_INVALID = 1,
    HS_POW_INPUT_TOO_LONG = 2,
    HS_POW_TARGET_ZERO = 3,
    HS_POW_TARGET_NEGATIVE = 4,
    HS_POW_TARGET_OVERFLOW = 5,
    HS_POW_TARGET_ABOVE_LIMIT = 6
} hs_pow_status;

hs_pow_status hs_sha256(const uint8_t *data, size_t length, uint8_t out32[32]);
hs_pow_status hs_sha256d(const uint8_t *data, size_t length, uint8_t out32[32]);

hs_pow_status hs_sha256d_header80(const uint8_t *header, size_t length,
                                 uint8_t out32[32]);

hs_pow_status hs_pow_target_from_compact(uint32_t compact, uint8_t target_le[32]);

hs_pow_status hs_pow_compact_from_target(const uint8_t target_le[32],
                                        uint32_t *compact);

hs_pow_status hs_pow_hash_meets_target(const uint8_t digest32[32],
                                      const uint8_t target_le[32], int *meets);

hs_pow_status hs_pow_check_compact(const uint8_t digest32[32], uint32_t compact,
                                  const uint8_t pow_limit_le[32], int *meets);

#ifdef __cplusplus
}
#endif
#endif
