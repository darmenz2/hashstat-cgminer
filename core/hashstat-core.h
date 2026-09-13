/* SPDX-License-Identifier: GPL-3.0-only
 * Original HashStat integration helpers, 2026. Not vendor hardware code.
 */
#ifndef HASHSTAT_CORE_H
#define HASHSTAT_CORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HS_CORE_BM1362_MASK UINT32_C(0x1fffe000)

#define HS_CORE_MIN_DIFFICULTY 0x1p-32
#define HS_CORE_MAX_DIFFICULTY 0x1p63

bool hs_core_parse_vmask(const char *text, size_t length,
                         uint32_t allowed_mask, uint32_t *result);
bool hs_core_valid_difficulty(double difficulty);

bool hs_core_flip_header80(const uint8_t *input, size_t length, uint8_t output[80]);

uint32_t hs_core_nonce_to_upstream(uint32_t canonical_nonce);

bool hs_core_hash_work80(const uint8_t *work_data, size_t length, uint8_t digest[32]);
#endif
