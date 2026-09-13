/* SPDX-License-Identifier: GPL-3.0-only */
#include "hs_work.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned from_hex(char c)
{
    if (c >= '0' && c <= '9') return (unsigned)(c - '0');
    assert(c >= 'a' && c <= 'f');
    return (unsigned)(c - 'a') + 10U;
}
static void decode(const char *s, uint8_t *out, size_t n)
{
    assert(strlen(s) == n * 2U);
    for (size_t i = 0; i < n; ++i)
        out[i] = (uint8_t)((from_hex(s[i * 2U]) << 4U) | from_hex(s[i * 2U + 1U]));
}
static int filled(const void *p, size_t n, uint8_t value)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; ++i) if (b[i] != value) return 0;
    return 1;
}

int main(void)
{
    static const char coinbase_hex[] =
        "01000000010000000000000000000000000000000000000000000000000000000000000000"
        "ffffffff4d04ffff001d0104455468652054696d65732030332f4a616e2f3230303920436861"
        "6e63656c6c6f72206f6e206272696e6b206f66207365636f6e64206261696c6f757420666f"
        "722062616e6b73ffffffff0100f2052a01000000434104678afdb0fe5548271967f1a67130"
        "b7105cd6a828e03909a67962e0ea1f61deb649f6bc3f4cef38c4f35504e51ec112de5c"
        "384df7ba0b8d578a4c702b6bf11d5fac00000000";
    uint8_t coinbase[(sizeof(coinbase_hex) - 1U) / 2U], scratch[512];
    uint8_t expected_merkle[32], expected_hash[32], digest[32], previous[40];
    hs_work_job job = {0};
    hs_work_result result, saved;
    int meets = 7;
    decode(coinbase_hex, coinbase, sizeof(coinbase));
    decode("3ba3edfd7a7b12b27ac72c3e67768f617fc81bc3888a51323a9fb8aa4b1e5e4a",
           expected_merkle, 32U);
    decode("6fe28c0ab6f1b372c1a6a246ae63f74f931e8365e15a089c68d6190000000000",
           expected_hash, 32U);
    job.coinbase1 = (hs_bytes){coinbase, sizeof(coinbase)};
    job.version = 1; job.ntime = UINT32_C(1231006505);
    job.nbits = UINT32_C(0x1d00ffff); job.nonce = UINT32_C(2083236893);
    memset(scratch, 0x5a, sizeof(scratch));
    assert(hs_work_build(&job, scratch, sizeof(scratch), &result) == HS_WORK_OK);
    assert(memcmp(result.coinbase_hash, expected_merkle, 32U) == 0);
    assert(memcmp(result.merkle_root, expected_merkle, 32U) == 0);
    assert(memcmp(scratch, coinbase, sizeof(coinbase)) == 0);
    assert(filled(scratch + sizeof(coinbase), sizeof(scratch) - sizeof(coinbase), 0x5a));
    assert(hs_work_check_nonce(result.header, 80U, job.nonce,
                              result.network_target_le, digest, &meets) == HS_WORK_OK);
    assert(meets == 1 && memcmp(digest, expected_hash, 32U) == 0);
    saved = result;
    assert(hs_work_check_nonce(result.header, 80U, job.nonce + 1U,
                              result.network_target_le, digest, &meets) == HS_WORK_OK);
    assert(meets == 0 && memcmp(&result, &saved, sizeof(result)) == 0);

    job.coinbase1 = (hs_bytes){coinbase, 23U};
    job.extranonce1 = (hs_bytes){coinbase + 23U, 7U};
    job.extranonce2 = (hs_bytes){coinbase + 30U, 9U};
    job.coinbase2 = (hs_bytes){coinbase + 39U, sizeof(coinbase) - 39U};
    assert(hs_work_build(&job, scratch, sizeof(scratch), &result) == HS_WORK_OK);
    assert(memcmp(&result, &saved, sizeof(result)) == 0);

    for (size_t i = 0; i < sizeof(previous); ++i) previous[i] = (uint8_t)i;
    assert(hs_work_prevhash_from_stratum(previous, 32U, previous + 1U) == HS_WORK_OK);
    for (size_t word = 0; word < 8U; ++word)
        for (size_t byte = 0; byte < 4U; ++byte)
            assert(previous[1U + word * 4U + byte] == word * 4U + 3U - byte);

#define FAIL(call, status) do { \
    memset(&result, 0xa5, sizeof(result)); \
    memset(scratch, 0x5a, sizeof(scratch)); \
    assert((call) == (status)); \
    assert(filled(&result, sizeof(result), 0xa5)); \
    assert(filled(scratch, sizeof(scratch), 0x5a)); \
} while (0)
    FAIL(hs_work_build(NULL, scratch, sizeof(scratch), &result), HS_WORK_INVALID);
    FAIL(hs_work_build(&job, NULL, sizeof(scratch), &result), HS_WORK_INVALID);
    FAIL(hs_work_build(&job, scratch, sizeof(coinbase) - 1U, &result), HS_WORK_SCRATCH_TOO_SMALL);
    job.branch_count = HS_WORK_MAX_BRANCHES + 1U;
    FAIL(hs_work_build(&job, scratch, sizeof(scratch), &result), HS_WORK_LIMIT);
    job.branch_count = 1U;
    FAIL(hs_work_build(&job, scratch, sizeof(scratch), &result), HS_WORK_INVALID);
    job.branch_count = 0U;
    job.coinbase1.length = SIZE_MAX;
    FAIL(hs_work_build(&job, scratch, sizeof(scratch), &result), HS_WORK_LIMIT);
    job.coinbase1.length = 23U;
    job.nbits = 0;
    FAIL(hs_work_build(&job, scratch, sizeof(scratch), &result), HS_WORK_INVALID_NBITS);
    job.nbits = UINT32_C(0x1d00ffff);
    job.coinbase1.data = scratch;
    FAIL(hs_work_build(&job, scratch, sizeof(scratch), &result), HS_WORK_OVERLAP);
    job.coinbase1.data = coinbase;
    FAIL(hs_work_build(&job, (uint8_t *)&result, sizeof(result), &result), HS_WORK_SCRATCH_TOO_SMALL);
    FAIL(hs_work_build(&job, (uint8_t *)&job, sizeof(scratch), &result), HS_WORK_OVERLAP);
    job.coinbase1 = (hs_bytes){NULL, 0}; job.coinbase2 = job.coinbase1;
    job.extranonce1 = job.coinbase1; job.extranonce2 = job.coinbase1;
    FAIL(hs_work_build(&job, scratch, sizeof(scratch), &result), HS_WORK_INVALID);
    job.coinbase1 = (hs_bytes){coinbase, 1U};
    FAIL(hs_work_build(&job, (uint8_t *)&result, sizeof(result), &result), HS_WORK_OVERLAP);

    union { hs_work_result result; uint8_t bytes[sizeof(hs_work_result)]; } alias;
    memset(alias.bytes, 0x11, sizeof(alias.bytes));
    job.coinbase1 = (hs_bytes){alias.bytes, sizeof(alias.bytes)};
    assert(hs_work_build(&job, scratch, sizeof(scratch), &alias.result) == HS_WORK_OK);
    assert(filled(scratch, sizeof(alias.bytes), 0x11));

    static uint8_t large_coinbase[65536], large_scratch[65536], branches[1024];
    job.coinbase1 = (hs_bytes){large_coinbase, sizeof(large_coinbase)};
    job.merkle_branches = branches; job.branch_count = HS_WORK_MAX_BRANCHES;
    assert(hs_work_build(&job, large_scratch, sizeof(large_scratch), &result) == HS_WORK_OK);
    memset(digest, 0x77, sizeof(digest)); meets = 8;
    const uint8_t zero_target[32] = {0};
    assert(hs_work_check_nonce(saved.header, 80U, 0, zero_target, digest, &meets) == HS_WORK_INVALID);
    assert(meets == 8 && filled(digest, sizeof(digest), 0x77));
    assert(hs_work_check_nonce(saved.header, 79U, 0, saved.network_target_le, digest, &meets) == HS_WORK_INVALID);
    assert(meets == 8 && filled(digest, sizeof(digest), 0x77));
    union { int aligned; uint8_t bytes[40]; } invalid_outputs;
    memset(invalid_outputs.bytes, 0x77, sizeof(invalid_outputs.bytes));
    assert(hs_work_check_nonce(saved.header, 80U, 0, saved.network_target_le,
           invalid_outputs.bytes, &invalid_outputs.aligned) == HS_WORK_OVERLAP);
    assert(filled(invalid_outputs.bytes, sizeof(invalid_outputs.bytes), 0x77));
    puts("work: genesis pipeline, nonce, representation, alias/resource/error guards PASS");
    return 0;
}
