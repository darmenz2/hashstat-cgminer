/* SPDX-License-Identifier: GPL-3.0-only
 * Original HashStat BIP310/work ownership integration, not vendor source.
 */
#ifndef HASHSTAT_STRATUM_H
#define HASHSTAT_STRATUM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define HS_STRATUM_JOB_SLOTS 32
#define HS_STRATUM_JOB_ID_MAX 128
struct hs_stratum_job {
    uint64_t sequence;
    uint32_t base_version;
    char id[HS_STRATUM_JOB_ID_MAX + 1];
};
struct hs_stratum_state {
    uint64_t session_epoch, clean_epoch, mask_epoch, next_job_sequence;
    uint64_t current_job_sequence;
    uint32_t mask;
    bool configured, rolling, ready, authorized, exhausted;
    size_t next_slot;
    struct hs_stratum_job jobs[HS_STRATUM_JOB_SLOTS];
};
struct hs_work_snapshot {
    uint64_t session_epoch, clean_epoch, mask_epoch, job_sequence;
    uint32_t base_version, actual_version, mask;
    bool valid, rolling;
    char worker[129];
};
struct pool;
struct work;

void hashstat_session_reset_locked(struct pool *pool);
bool hashstat_note_job_locked(struct pool *pool, const char *job_id,
                             uint32_t base_version, bool clean);
bool hashstat_capture_work_locked(struct pool *pool, struct work *work);
bool hashstat_work_current_locked(const struct pool *pool, const struct work *work);
bool hashstat_work_current(struct work *work);
bool hashstat_submit_json_locked(const struct pool *pool, const struct work *work,
                                int request_id, char *output, size_t capacity);

bool hashstat_apply_configure(struct pool *pool, bool rolling, uint32_t mask);
bool hashstat_apply_mask(struct pool *pool, uint32_t mask);
bool hashstat_parse_mask_text(const char *text, size_t length, uint32_t *mask);
bool hashstat_roll_work_version(struct work *work, uint32_t version_bits);
bool hashstat_next_request_id(int *request_id);
enum hs_share_send_result { HS_SHARE_STALE, HS_SHARE_FAILED, HS_SHARE_SENT };

enum hs_share_send_result hashstat_send_share_locked(struct pool *pool,
                                                    const struct work *work,
                                                    int request_id);

void hashstat_suspend_stratum_locked(struct pool *pool);
#endif
