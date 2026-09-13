/* SPDX-License-Identifier: GPL-3.0-only */
#include "hs_job_cache.h"
#include "hs_span.h"
#include "hs_work.h"

#define CACHE_MARKER UINT32_C(0x48534a42)

static int valid(const struct hs_job_cache *cache)
{
    return hs_span_nonempty(cache, sizeof(*cache)) && cache->initialized == CACHE_MARKER &&
           cache->session_tag != 0;
}
static void zero_bytes(void *p, size_t n)
{
    uint8_t *bytes = p;
    for (size_t i = 0; i < n; ++i) bytes[i] = 0;
}
static uint32_t read_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

enum hs_job_status hs_job_cache_init(struct hs_job_cache *cache, uint64_t session_tag)
{
    if (!hs_span_nonempty(cache, sizeof(*cache)) || session_tag == 0) return HS_JOB_INVALID;
    zero_bytes(cache, sizeof(*cache));
    cache->initialized = CACHE_MARKER;
    cache->session_tag = session_tag;
    return HS_JOB_OK;
}
enum hs_job_status hs_job_cache_clear(struct hs_job_cache *cache, uint64_t session_tag)
{
    if (!valid(cache)) return HS_JOB_INVALID;
    if (session_tag != cache->session_tag) return HS_JOB_WRONG_SESSION;
    zero_bytes(cache->slots, sizeof(cache->slots));
    return HS_JOB_OK;
}
enum hs_job_status hs_job_cache_retire(struct hs_job_cache *cache, unsigned slot)
{
    if (!valid(cache) || slot >= HS_JOB_CACHE_SLOTS) return HS_JOB_INVALID;
    zero_bytes(&cache->slots[slot], sizeof(cache->slots[slot]));
    return HS_JOB_OK;
}
enum hs_job_status hs_job_snapshot_validate(const struct hs_job_snapshot *snapshot,
                                          uint64_t session_tag)
{
    if (session_tag == 0 || !hs_span_nonempty(snapshot, sizeof(*snapshot)))
        return HS_JOB_INVALID;
    if (snapshot->session_tag != session_tag) return HS_JOB_WRONG_SESSION;
    if (snapshot->job_tag == 0 || snapshot->work_tag == 0 || snapshot->max_age_ms == 0)
        return HS_JOB_INVALID;
    if ((snapshot->version_mask & ~HS_BM1362_RX_VERSION_BITS_MASK) != 0 ||
        (read_le(snapshot->header) & snapshot->version_mask) != 0)
        return HS_JOB_VERSION_REJECTED;
    uint8_t target_any = 0;
    for (size_t i = 0; i < 32U; ++i) target_any |= snapshot->share_target_le[i];
    if (target_any == 0) return HS_JOB_INVALID;
    return HS_JOB_OK;
}

enum hs_job_status hs_job_cache_publish(struct hs_job_cache *cache, unsigned slot,
                                      const struct hs_job_snapshot *snapshot)
{
    if (!valid(cache) || slot >= HS_JOB_CACHE_SLOTS || !hs_span_nonempty(snapshot, sizeof(*snapshot)))
        return HS_JOB_INVALID;
    if (cache->slots[slot].active != 0) return HS_JOB_SLOT_ACTIVE;
    enum hs_job_status validation = hs_job_snapshot_validate(snapshot, cache->session_tag);
    if (validation != HS_JOB_OK) return validation;
    const struct hs_job_snapshot copy = *snapshot;
    zero_bytes(&cache->slots[slot], sizeof(cache->slots[slot]));
    cache->slots[slot].work = copy;
    cache->slots[slot].active = 1;
    return HS_JOB_OK;
}

enum hs_job_status hs_job_cache_check(struct hs_job_cache *cache,
                                    const struct hs_bm1362_rx_result *reply,
                                    uint64_t now_ms, struct hs_checked_share *out)
{
    if (!valid(cache) || !hs_span_nonempty(reply, sizeof(*reply)) || !hs_span_nonempty(out, sizeof(*out)))
        return HS_JOB_INVALID;
    if (hs_spans_overlap(out, sizeof(*out), cache, sizeof(*cache)) ||
        hs_spans_overlap(out, sizeof(*out), reply, sizeof(*reply)) ||
        hs_spans_overlap(reply, sizeof(*reply), cache, sizeof(*cache))) return HS_JOB_OVERLAP;
    if (reply->status != HS_BM1362_RX_DECODED_UNVERIFIED ||
        reply->slot >= HS_JOB_CACHE_SLOTS ||
        reply->chip_index >= HS_BM1362_S19_88_CHIPS ||
        reply->internal_nonce != ((reply->nonce << 24) |
            ((reply->nonce & UINT32_C(0xff00)) << 8) |
            ((reply->nonce >> 8) & UINT32_C(0xff00)) | (reply->nonce >> 24)) ||
        reply->chip_index != ((reply->internal_nonce >> 17) & 255U) / 2U ||
        reply->core_index != (uint8_t)(reply->internal_nonce >> 25)) return HS_JOB_INVALID;
    struct hs_job_slot *entry = &cache->slots[reply->slot];
    if (entry->active == 0) return HS_JOB_EMPTY;
    if (entry->active != 1 || entry->recent_count > HS_JOB_RECENT_NONCES ||
        entry->recent_next >= HS_JOB_RECENT_NONCES) return HS_JOB_INVALID;
    const struct hs_job_snapshot *work = &entry->work;
    if (work->session_tag != cache->session_tag) return HS_JOB_WRONG_SESSION;
    if (now_ms < work->issued_ms || now_ms - work->issued_ms >= work->max_age_ms)
        return HS_JOB_STALE;
    if ((reply->version_bits & ~work->version_mask) != 0) return HS_JOB_VERSION_REJECTED;
    struct hs_checked_share checked = {0};
    uint8_t header[80];
    int meets;
    for (size_t i = 0; i < 80U; ++i) header[i] = work->header[i];
    checked.full_version = read_le(header) | reply->version_bits;
    for (unsigned i = 0; i < 4U; ++i) header[i] = (uint8_t)(checked.full_version >> (i * 8U));
    if (hs_work_check_nonce(header, sizeof(header), reply->nonce, work->share_target_le,
                            checked.digest, &meets) != HS_WORK_OK) return HS_JOB_INVALID;
    if (!meets) return HS_JOB_ABOVE_TARGET;
    for (unsigned i = 0; i < entry->recent_count; ++i)
        if (entry->recent[i].nonce == reply->nonce &&
            entry->recent[i].version == checked.full_version) return HS_JOB_DUPLICATE;
    checked.session_tag = work->session_tag; checked.job_tag = work->job_tag;
    checked.work_tag = work->work_tag; checked.nonce = reply->nonce;
    checked.version_bits = reply->version_bits; checked.slot = reply->slot;
    checked.chip_index = reply->chip_index; checked.core_index = reply->core_index;
    entry->recent[entry->recent_next] = (struct hs_job_recent_nonce){reply->nonce, checked.full_version};
    entry->recent_next = (uint8_t)((entry->recent_next + 1U) % HS_JOB_RECENT_NONCES);
    if (entry->recent_count < HS_JOB_RECENT_NONCES) ++entry->recent_count;
    *out = checked;
    return HS_JOB_OK;
}
