/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef HS_WORK_H
#define HS_WORK_H
#include <stddef.h>
#include <stdint.h>
#include "hs_pow.h"

#define HS_WORK_MAX_COINBASE_BYTES ((size_t)65536)
#define HS_WORK_MAX_BRANCHES ((size_t)32)

typedef struct hs_bytes { const uint8_t *data; size_t length; } hs_bytes;
typedef struct hs_work_job {
    hs_bytes coinbase1, extranonce1, extranonce2, coinbase2;

    const uint8_t *merkle_branches;
    size_t branch_count;

    uint8_t previous_hash_wire[32];
    uint32_t version, ntime, nbits, nonce;
} hs_work_job;

typedef struct hs_work_result {
    uint8_t header[80];
    uint8_t coinbase_hash[32];
    uint8_t merkle_root[32];
    uint8_t network_target_le[32];
} hs_work_result;

typedef enum hs_work_status {
    HS_WORK_OK = 0,
    HS_WORK_INVALID = 1,
    HS_WORK_LIMIT = 2,
    HS_WORK_SCRATCH_TOO_SMALL = 3,
    HS_WORK_OVERLAP = 4,
    HS_WORK_INVALID_NBITS = 5,
    HS_WORK_HASH_ERROR = 6
} hs_work_status;

hs_work_status hs_work_build(const hs_work_job *job, uint8_t *scratch,
                            size_t scratch_capacity, hs_work_result *result);

hs_work_status hs_work_prevhash_from_stratum(const uint8_t *input, size_t length,
                                           uint8_t output_wire[32]);

hs_work_status hs_work_check_nonce(const uint8_t *header, size_t header_length,
                                  uint32_t nonce, const uint8_t target_le[32],
                                  uint8_t digest[32], int *meets);
#endif
