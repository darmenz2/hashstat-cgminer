/* SPDX-License-Identifier: GPL-3.0-only */
/* Coinbase, Merkle root and block-header assembly. */
#include "hs_work.h"
#include "hs_span.h"

static void copy_bytes(uint8_t *dst, const uint8_t *src, size_t n)
{
    for (size_t i = 0; i < n; ++i) dst[i] = src[i];
}
static void put_le32(uint8_t *p, uint32_t n)
{
    for (unsigned i = 0; i < 4U; ++i) p[i] = (uint8_t)(n >> (8U * i));
}

hs_work_status hs_work_build(const hs_work_job *job, uint8_t *scratch,
                            size_t scratch_capacity, hs_work_result *result)
{
    hs_work_result built;
    hs_bytes fields[4];
    uint8_t pair[64];
    size_t total = 0, offset = 0;
    if (!hs_span_valid(job, sizeof(*job)) || !hs_span_valid(result, sizeof(*result)))
        return HS_WORK_INVALID;
    if (job->branch_count > HS_WORK_MAX_BRANCHES) return HS_WORK_LIMIT;
    const size_t branch_bytes = job->branch_count * 32U;
    if (!hs_span_valid(job->merkle_branches, branch_bytes)) return HS_WORK_INVALID;
    fields[0] = job->coinbase1; fields[1] = job->extranonce1;
    fields[2] = job->extranonce2; fields[3] = job->coinbase2;
    for (size_t i = 0; i < 4U; ++i) {
        if (fields[i].length > HS_WORK_MAX_COINBASE_BYTES - total)
            return HS_WORK_LIMIT;
        if (!hs_span_valid(fields[i].data, fields[i].length)) return HS_WORK_INVALID;
        total += fields[i].length;
    }
    if (total == 0U) return HS_WORK_INVALID;
    if (scratch_capacity < total) return HS_WORK_SCRATCH_TOO_SMALL;
    if (!hs_span_valid(scratch, total)) return HS_WORK_INVALID;
    if (hs_spans_overlap(scratch, total, job, sizeof(*job)) ||
        hs_spans_overlap(scratch, total, result, sizeof(*result)) ||
        hs_spans_overlap(scratch, total, job->merkle_branches, branch_bytes))
        return HS_WORK_OVERLAP;
    for (size_t i = 0; i < 4U; ++i)
        if (hs_spans_overlap(scratch, total, fields[i].data, fields[i].length))
            return HS_WORK_OVERLAP;
    if (hs_pow_target_from_compact(job->nbits, built.network_target_le) != HS_POW_OK)
        return HS_WORK_INVALID_NBITS;
    for (size_t i = 0; i < 4U; ++i) {
        copy_bytes(scratch + offset, fields[i].data, fields[i].length);
        offset += fields[i].length;
    }
    if (hs_sha256d(scratch, total, built.coinbase_hash) != HS_POW_OK)
        return HS_WORK_HASH_ERROR;
    copy_bytes(built.merkle_root, built.coinbase_hash, 32U);
    for (size_t i = 0; i < job->branch_count; ++i) {
        copy_bytes(pair, built.merkle_root, 32U);
        copy_bytes(pair + 32U, job->merkle_branches + i * 32U, 32U);
        if (hs_sha256d(pair, sizeof(pair), built.merkle_root) != HS_POW_OK)
            return HS_WORK_HASH_ERROR;
    }
    put_le32(built.header, job->version);
    copy_bytes(built.header + 4U, job->previous_hash_wire, 32U);
    copy_bytes(built.header + 36U, built.merkle_root, 32U);
    put_le32(built.header + 68U, job->ntime);
    put_le32(built.header + 72U, job->nbits);
    put_le32(built.header + 76U, job->nonce);
    *result = built;
    return HS_WORK_OK;
}

hs_work_status hs_work_prevhash_from_stratum(const uint8_t *input, size_t length,
                                           uint8_t output_wire[32])
{
    uint8_t converted[32];
    if (length != 32U || !hs_span_valid(input, 32U) || !hs_span_valid(output_wire, 32U))
        return HS_WORK_INVALID;
    for (size_t word = 0; word < 8U; ++word)
        for (size_t byte = 0; byte < 4U; ++byte)
            converted[word * 4U + byte] = input[word * 4U + 3U - byte];
    copy_bytes(output_wire, converted, 32U);
    return HS_WORK_OK;
}

hs_work_status hs_work_check_nonce(const uint8_t *header, size_t header_length,
                                  uint32_t nonce, const uint8_t target_le[32],
                                  uint8_t digest[32], int *meets)
{
    uint8_t candidate[80], computed[32];
    int accepted;
    if (header_length != 80U || !hs_span_valid(header, 80U) || !hs_span_valid(target_le, 32U) ||
        !hs_span_valid(digest, 32U) || !hs_span_valid(meets, sizeof(*meets)))
        return HS_WORK_INVALID;
    if (hs_spans_overlap(digest, 32U, meets, sizeof(*meets))) return HS_WORK_OVERLAP;
    copy_bytes(candidate, header, 80U);
    put_le32(candidate + 76U, nonce);
    if (hs_sha256d_header80(candidate, 80U, computed) != HS_POW_OK)
        return HS_WORK_HASH_ERROR;
    if (hs_pow_hash_meets_target(computed, target_le, &accepted) != HS_POW_OK)
        return HS_WORK_INVALID;
    copy_bytes(digest, computed, 32U);
    *meets = accepted;
    return HS_WORK_OK;
}
