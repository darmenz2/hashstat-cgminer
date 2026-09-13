/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef HS_JOB_CACHE_H
#define HS_JOB_CACHE_H
#include <stdint.h>
#include "hs_bm1362_rx.h"

#define HS_JOB_CACHE_SLOTS 32U
#define HS_JOB_RECENT_NONCES 16U

enum hs_job_status {
    HS_JOB_OK = 0, HS_JOB_INVALID, HS_JOB_WRONG_SESSION, HS_JOB_EMPTY,
    HS_JOB_STALE, HS_JOB_VERSION_REJECTED, HS_JOB_ABOVE_TARGET,
    HS_JOB_DUPLICATE, HS_JOB_OVERLAP, HS_JOB_SLOT_ACTIVE
};
struct hs_job_recent_nonce { uint32_t nonce, version; };
struct hs_job_snapshot {
    uint8_t header[80], share_target_le[32];
    uint64_t session_tag, job_tag, work_tag, issued_ms;
    uint32_t version_mask, max_age_ms;
};
struct hs_job_slot {
    struct hs_job_snapshot work;
    struct hs_job_recent_nonce recent[HS_JOB_RECENT_NONCES];
    uint8_t active, recent_count, recent_next;
};
struct hs_job_cache {
    uint32_t initialized;
    uint64_t session_tag;
    struct hs_job_slot slots[HS_JOB_CACHE_SLOTS];
};
struct hs_checked_share {
    uint64_t session_tag, job_tag, work_tag;
    uint32_t nonce, version_bits, full_version;
    uint8_t digest[32], slot, chip_index, core_index;
};

enum hs_job_status hs_job_cache_init(struct hs_job_cache *cache, uint64_t session_tag);
enum hs_job_status hs_job_cache_clear(struct hs_job_cache *cache, uint64_t session_tag);

enum hs_job_status hs_job_cache_retire(struct hs_job_cache *cache, unsigned slot);

enum hs_job_status hs_job_snapshot_validate(const struct hs_job_snapshot *snapshot,
                                          uint64_t session_tag);
enum hs_job_status hs_job_cache_publish(struct hs_job_cache *cache, unsigned slot,
                                      const struct hs_job_snapshot *snapshot);

enum hs_job_status hs_job_cache_check(struct hs_job_cache *cache,
                                    const struct hs_bm1362_rx_result *reply,
                                    uint64_t now_ms, struct hs_checked_share *out);
#endif
