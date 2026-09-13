/* SPDX-License-Identifier: GPL-3.0-only */
#include "hs_job_cache.h"
#include "hs_work.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned hex_digit(char c)
{
    if (c >= '0' && c <= '9') return (unsigned)(c - '0');
    assert(c >= 'a' && c <= 'f'); return (unsigned)(c - 'a') + 10U;
}
static void decode(const char *s, uint8_t *out, size_t n)
{
    assert(strlen(s) == n * 2U);
    for (size_t i = 0; i < n; ++i)
        out[i] = (uint8_t)((hex_digit(s[i * 2U]) << 4U) | hex_digit(s[i * 2U + 1U]));
}
static struct hs_bm1362_rx_result reply_for(uint32_t nonce, uint8_t slot)
{
    const uint8_t frame[11] = {0xaa, 0x55, (uint8_t)nonce,
        (uint8_t)(nonce >> 8), (uint8_t)(nonce >> 16), (uint8_t)(nonce >> 24),
        0, (uint8_t)(slot << 3), 0, 0, 0x80};
    return hs_bm1362_rx_decode_long(frame, sizeof(frame));
}
static unsigned checks;
#define CHECK(x) do { assert(x); ++checks; } while (0)

int main(void)
{
    struct hs_job_cache cache, saved;
    struct hs_job_snapshot job = {0};
    struct hs_checked_share checked, before;

    uint8_t capture_hash[32];
    decode("0040b420fd55646bc162b96dfcd4f201a3f4670d1d3996bc965201000000000000000000"
           "06ddf5f08c36414b95ea54db71a0c28761a98bcf6355919e044f88725519a7cb"
           "d7685468043a02174c035275", job.header, sizeof(job.header));
    decode("fe27887d7a685806d8516ce9da43ea948638f7bc97e9accf0437020000000000", capture_hash, 32U);
    decode("000000000000000000000000000000000000000000000000f8ff070000000000", job.share_target_le, 32U);
    job.session_tag = 7; job.job_tag = 8; job.work_tag = 9;
    job.issued_ms = 1000; job.max_age_ms = 100;
    struct hs_bm1362_rx_result rx = reply_for(UINT32_C(0x7552034c), 3);
    CHECK(rx.status == HS_BM1362_RX_DECODED_UNVERIFIED);
    CHECK(hs_job_cache_init(&cache, 7) == HS_JOB_OK);
    memset(&checked, 0xa5, sizeof(checked)); before = checked;
    CHECK(hs_job_cache_check(&cache, &rx, 1001, &checked) == HS_JOB_EMPTY);
    CHECK(memcmp(&checked, &before, sizeof(checked)) == 0);
    CHECK(hs_job_cache_publish(&cache, 3, &job) == HS_JOB_OK);
    saved = cache;
    CHECK(hs_job_cache_publish(&cache, 3, &job) == HS_JOB_SLOT_ACTIVE);
    CHECK(memcmp(&cache, &saved, sizeof(cache)) == 0);
    CHECK(hs_job_cache_check(&cache, &rx, 1001, &checked) == HS_JOB_OK);
    CHECK(checked.session_tag == 7 && checked.job_tag == 8 && checked.work_tag == 9);
    CHECK(checked.nonce == rx.nonce && checked.full_version == UINT32_C(0x20b44000) && checked.version_bits == 0);
    CHECK(memcmp(checked.digest, capture_hash, 32U) == 0);
    saved = cache; before = checked;
    CHECK(hs_job_cache_check(&cache, &rx, 1001, &checked) == HS_JOB_DUPLICATE);
    CHECK(memcmp(&checked, &before, sizeof(checked)) == 0 && memcmp(&cache, &saved, sizeof(cache)) == 0);
    rx = reply_for(UINT32_C(0x7552034d), 3);
    CHECK(hs_job_cache_check(&cache, &rx, 1001, &checked) == HS_JOB_ABOVE_TARGET);
    CHECK(memcmp(&checked, &before, sizeof(checked)) == 0 && memcmp(&cache, &saved, sizeof(cache)) == 0);
    rx = reply_for(UINT32_C(0x7552034c), 3);
    CHECK(hs_job_cache_check(&cache, &rx, 999, &checked) == HS_JOB_STALE);
    CHECK(hs_job_cache_check(&cache, &rx, 1100, &checked) == HS_JOB_STALE);
    CHECK(memcmp(&checked, &before, sizeof(checked)) == 0 && memcmp(&cache, &saved, sizeof(cache)) == 0);
    rx.version_bits = 0x2000;
    CHECK(hs_job_cache_check(&cache, &rx, 1001, &checked) == HS_JOB_VERSION_REJECTED);
    rx.version_bits = 0;
    CHECK(hs_job_cache_clear(&cache, 99) == HS_JOB_WRONG_SESSION);
    CHECK(memcmp(&cache, &saved, sizeof(cache)) == 0);
    CHECK(hs_job_cache_clear(&cache, 7) == HS_JOB_OK);
    CHECK(hs_job_cache_check(&cache, &rx, 1001, &checked) == HS_JOB_EMPTY);
    CHECK(hs_job_cache_init(&cache, 10) == HS_JOB_OK);
    CHECK(hs_job_cache_publish(&cache, 3, &job) == HS_JOB_WRONG_SESSION);
    CHECK(hs_job_cache_init(&cache, 7) == HS_JOB_OK);

    for (unsigned slot = 0; slot < 32U; ++slot) {
        job.work_tag = slot + 1U;
        CHECK(hs_job_cache_publish(&cache, slot, &job) == HS_JOB_OK);
    }
    memset(job.header, 0, sizeof(job.header));
    for (unsigned slot = 0; slot < 32U; ++slot) {
        rx = reply_for(UINT32_C(0x7552034c), (uint8_t)slot);
        CHECK(hs_job_cache_check(&cache, &rx, 1000, &checked) == HS_JOB_OK);
        CHECK(checked.slot == slot && checked.work_tag == slot + 1U);
        CHECK(memcmp(checked.digest, capture_hash, 32U) == 0);
        CHECK(hs_job_cache_retire(&cache, slot) == HS_JOB_OK);
        CHECK(hs_job_cache_check(&cache, &rx, 1000, &checked) == HS_JOB_EMPTY);
    }

    memset(job.share_target_le, 0xff, sizeof(job.share_target_le));
    job.version_mask = HS_BM1362_RX_VERSION_BITS_MASK;
    CHECK(hs_job_cache_publish(&cache, 0, &job) == HS_JOB_OK);
    for (uint32_t i = 0; i < 100U; ++i) {
        rx = reply_for(i, 0); rx.version_bits = (i & 15U) << 13;
        CHECK(hs_job_cache_check(&cache, &rx, 1099, &checked) == HS_JOB_OK);
        CHECK(checked.full_version == rx.version_bits);
        CHECK(hs_job_cache_check(&cache, &rx, 1099, &checked) == HS_JOB_DUPLICATE);
    }
    rx = reply_for(0, 0);
    CHECK(hs_job_cache_check(&cache, &rx, 1099, &checked) == HS_JOB_OK);
    CHECK(hs_job_cache_clear(&cache, 7) == HS_JOB_OK);
    saved = cache;
    job.header[1] = 0x20;
    CHECK(hs_job_cache_publish(&cache, 0, &job) == HS_JOB_VERSION_REJECTED);
    job.header[1] = 0; job.version_mask = UINT32_MAX;
    CHECK(hs_job_cache_publish(&cache, 0, &job) == HS_JOB_VERSION_REJECTED);
    job.version_mask = 0; job.max_age_ms = 0;
    CHECK(hs_job_cache_publish(&cache, 0, &job) == HS_JOB_INVALID);
    job.max_age_ms = 100;
    memset(job.share_target_le, 0, 32U);
    CHECK(hs_job_cache_publish(&cache, 0, &job) == HS_JOB_INVALID);
    CHECK(memcmp(&cache, &saved, sizeof(cache)) == 0);
    CHECK(hs_job_cache_publish(&cache, 32U, &job) == HS_JOB_INVALID);
    CHECK(hs_job_cache_retire(&cache, 32U) == HS_JOB_INVALID);
    CHECK(hs_job_cache_init(&cache, 0) == HS_JOB_INVALID);
    CHECK(memcmp(&cache, &saved, sizeof(cache)) == 0);
    memset(job.share_target_le, 0xff, 32U);
    CHECK(hs_job_cache_publish(&cache, 0, &job) == HS_JOB_OK);
    CHECK(hs_job_cache_publish(&cache, 1, &cache.slots[0].work) == HS_JOB_OK);
    CHECK(hs_job_cache_check(&cache, &rx, 1000, (struct hs_checked_share *)&cache.slots[0]) == HS_JOB_OVERLAP);
    rx.status = HS_BM1362_RX_NOT_NONCE;
    CHECK(hs_job_cache_check(&cache, &rx, 1000, &checked) == HS_JOB_INVALID);
    rx = reply_for(0, 0); rx.chip_index = 87;
    CHECK(hs_job_cache_check(&cache, &rx, 1000, &checked) == HS_JOB_INVALID);
    rx = reply_for(0, 0); rx.core_index = 127;
    CHECK(hs_job_cache_check(&cache, &rx, 1000, &checked) == HS_JOB_INVALID);
    rx = reply_for(0, 0); rx.internal_nonce = 1;
    CHECK(hs_job_cache_check(&cache, &rx, 1000, &checked) == HS_JOB_INVALID);
    rx = reply_for(0, 0); cache.slots[0].recent_count = 17;
    CHECK(hs_job_cache_check(&cache, &rx, 1000, &checked) == HS_JOB_INVALID);
    cache.slots[0].recent_count = 0; cache.slots[0].recent_next = 16;
    CHECK(hs_job_cache_check(&cache, &rx, 1000, &checked) == HS_JOB_INVALID);
    CHECK(hs_job_cache_init(&cache, 7) == HS_JOB_OK);
    job.issued_ms = UINT64_MAX - 10U;
    CHECK(hs_job_cache_publish(&cache, 0, &job) == HS_JOB_OK);
    CHECK(hs_job_cache_check(&cache, &rx, UINT64_MAX, &checked) == HS_JOB_OK);
    CHECK(hs_job_cache_check(&cache, &rx, 1U, &checked) == HS_JOB_STALE);
    printf("job cache: %u checks PASS; software snapshots/PoW only, not upstream acceptance\n", checks);
    return 0;
}
