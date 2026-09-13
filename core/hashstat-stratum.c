/* SPDX-License-Identifier: GPL-3.0-only
 * Original HashStat source integration of documented BIP310 semantics.
 * No device/pool connections, hardware operations or fabricated telemetry.
 */
#include "config.h"
#include "miner.h"
#include "hashstat-core.h"
#include "hashstat-stratum.h"
#include <jansson.h>
#include <string.h>
#ifdef USE_HASHSTAT_AML88
static bool next_epoch(struct hs_stratum_state *state, uint64_t *value)
{
    if (state->exhausted || *value == UINT64_MAX) {
        state->exhausted = true;
        state->ready = false;
        return false;
    }
    ++*value;
    return true;
}
static uint32_t read_be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static bool bounded_string(const char *s, size_t maximum, size_t *length)
{
    size_t n;
    if (!s) return false;
    for (n = 0; n <= maximum; ++n) {
        if (!s[n]) { if (length) *length = n; return n != 0; }
        if ((unsigned char)s[n] < 0x20 || (unsigned char)s[n] > 0x7e) return false;
    }
    return false;
}
bool hashstat_parse_mask_text(const char *text, size_t length, uint32_t *mask)
{
    if (!text || !mask || length != 8) return false;
    if (!memcmp(text, "00000000", 8)) { *mask = 0; return true; }
    return hs_core_parse_vmask(text, length, HS_CORE_BM1362_MASK, mask);
}
void hashstat_session_reset_locked(struct pool *pool)
{
    struct hs_stratum_state *s = &pool->hs_stratum;
    (void)next_epoch(s, &s->session_epoch);
    s->ready = s->authorized = s->configured = s->rolling = false;
    s->mask = 0;
    s->current_job_sequence = 0;
    s->next_slot = 0;
    memset(s->jobs, 0, sizeof(s->jobs));
}
bool hashstat_apply_configure(struct pool *pool, bool rolling, uint32_t mask)
{
    bool ok = false;
    if (!pool || (mask & ~HS_CORE_BM1362_MASK) || (!rolling && mask)) return false;
    cg_wlock(&pool->data_lock);
    struct hs_stratum_state *s = &pool->hs_stratum;
    if (!s->ready && s->session_epoch && next_epoch(s, &s->mask_epoch)) {
        s->rolling = rolling; s->mask = mask; s->configured = true; ok = true;
    }
    cg_wunlock(&pool->data_lock);
    return ok;
}
bool hashstat_apply_mask(struct pool *pool, uint32_t mask)
{
    bool ok = false;
    if (!pool || (mask & ~HS_CORE_BM1362_MASK)) return false;
    cg_wlock(&pool->data_lock);
    struct hs_stratum_state *s = &pool->hs_stratum;
    if (s->configured && s->rolling && !s->exhausted) {
        if (s->mask == mask || next_epoch(s, &s->mask_epoch)) {
            s->mask = mask; ok = true;
        }
    }
    cg_wunlock(&pool->data_lock);
    return ok;
}
bool hashstat_note_job_locked(struct pool *pool, const char *job_id,
                             uint32_t base_version, bool clean)
{
    struct hs_stratum_state *s = &pool->hs_stratum;
    size_t length, slot = HS_STRATUM_JOB_SLOTS;
    if (!bounded_string(job_id, HS_STRATUM_JOB_ID_MAX, &length) ||
        !s->ready || !s->configured || s->next_slot >= HS_STRATUM_JOB_SLOTS || s->exhausted)
        return false;
    if (s->next_job_sequence == UINT64_MAX || (clean && s->clean_epoch == UINT64_MAX)) {
        s->exhausted = true; s->ready = false; return false;
    }
    if (clean) {
        ++s->clean_epoch;
        memset(s->jobs, 0, sizeof(s->jobs));
        s->next_slot = 0;
    }
    for (size_t i = 0; i < HS_STRATUM_JOB_SLOTS; ++i)
        if (s->jobs[i].sequence && !strcmp(s->jobs[i].id, job_id)) { slot = i; break; }
    if (slot == HS_STRATUM_JOB_SLOTS) {
        slot = s->next_slot;
        s->next_slot = (slot + 1) % HS_STRATUM_JOB_SLOTS;
    }
    struct hs_stratum_job *job = &s->jobs[slot];
    job->sequence = ++s->next_job_sequence;
    job->base_version = base_version;
    memcpy(job->id, job_id, length + 1);
    s->current_job_sequence = job->sequence;
    return true;
}
bool hashstat_capture_work_locked(struct pool *pool, struct work *work)
{
    const struct hs_stratum_state *s = &pool->hs_stratum;
    size_t worker_length;
    if (!s->ready || !s->authorized || !s->configured || s->exhausted || !work->job_id ||
        !bounded_string(pool->rpc_user, 128, &worker_length)) return false;
    for (size_t i = 0; i < HS_STRATUM_JOB_SLOTS; ++i) {
        const struct hs_stratum_job *j = &s->jobs[i];
        if (j->sequence == s->current_job_sequence && !strcmp(j->id, work->job_id) &&
            j->base_version == read_be32(work->data)) {
            struct hs_work_snapshot snapshot = {0};
            snapshot.session_epoch = s->session_epoch; snapshot.clean_epoch = s->clean_epoch;
            snapshot.mask_epoch = s->mask_epoch; snapshot.job_sequence = j->sequence;
            snapshot.base_version = snapshot.actual_version = j->base_version;
            snapshot.mask = s->mask; snapshot.rolling = s->rolling; snapshot.valid = true;
            memcpy(snapshot.worker, pool->rpc_user, worker_length + 1);
            work->hs_snapshot = snapshot;
            return true;
        }
    }
    return false;
}
bool hashstat_work_current_locked(const struct pool *pool, const struct work *work)
{
    if (!pool || !work || work->pool != pool || !work->stratum || !work->job_id) return false;
    const struct hs_stratum_state *s = &pool->hs_stratum;
    const struct hs_work_snapshot *w = &work->hs_snapshot;
    if (!s->ready || !s->authorized || s->exhausted || !w->valid || !w->session_epoch ||
        s->session_epoch != w->session_epoch || s->clean_epoch != w->clean_epoch ||
        s->mask_epoch != w->mask_epoch || s->mask != w->mask || s->rolling != w->rolling ||
        read_be32(work->data) != w->actual_version ||
        ((w->actual_version ^ w->base_version) & ~w->mask) ||
        (!w->rolling && w->actual_version != w->base_version)) return false;
    for (size_t i = 0; i < HS_STRATUM_JOB_SLOTS; ++i) {
        const struct hs_stratum_job *j = &s->jobs[i];
        if (j->sequence == w->job_sequence && j->base_version == w->base_version &&
            !strcmp(j->id, work->job_id)) return true;
    }
    return false;
}
bool hashstat_work_current(struct work *work)
{
    bool ok;
    if (!work || !work->pool) return false;
    cg_rlock(&work->pool->data_lock);
    ok = hashstat_work_current_locked(work->pool, work);
    cg_runlock(&work->pool->data_lock);
    return ok;
}
bool hashstat_submit_json_locked(const struct pool *pool, const struct work *work,
                                int request_id, char *output, size_t capacity)
{
    char n2[17], nonce[9], ntime[9], version[9];
    unsigned char n2bytes[8];
    if (!output || !capacity || request_id < 0 || !hashstat_work_current_locked(pool, work) ||
        !bounded_string(work->hs_snapshot.worker, 128, NULL) ||
        !bounded_string(work->job_id, HS_STRATUM_JOB_ID_MAX, NULL) ||
        !work->nonce2_len || work->nonce2_len > 8 || !work->ntime || strlen(work->ntime) != 8)
        return false;
    if (work->nonce2_len < 8 && work->nonce2 >= (UINT64_C(1) << (8 * work->nonce2_len))) return false;
    for (size_t i = 0; i < 8; ++i) n2bytes[i] = (unsigned char)(work->nonce2 >> (8 * i));
    __bin2hex(n2, n2bytes, work->nonce2_len);
    __bin2hex(nonce, work->data + 76, 4);
    __bin2hex(ntime, work->data + 68, 4);
    if (strcmp(ntime, work->ntime)) return false;
    snprintf(version, sizeof(version), "%08" PRIx32, work->hs_snapshot.actual_version & work->hs_snapshot.mask);
    json_t *params = work->hs_snapshot.rolling ?
        json_pack("[s,s,s,s,s,s]", work->hs_snapshot.worker, work->job_id, n2, ntime, nonce, version) :
        json_pack("[s,s,s,s,s]", work->hs_snapshot.worker, work->job_id, n2, ntime, nonce);
    if (!params) return false;
    json_t *request = json_pack("{s:s,s:i,s:o}", "method", "mining.submit", "id", request_id, "params", params);
    if (!request) return false;
    char *encoded = json_dumps(request, JSON_COMPACT);
    json_decref(request);
    if (!encoded) return false;
    size_t length = strlen(encoded);
    bool ok = length < capacity;
    if (ok) memcpy(output, encoded, length + 1);
    free(encoded);
    return ok;
}
#endif
