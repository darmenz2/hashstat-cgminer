/* SPDX-License-Identifier: GPL-3.0-only */
#include "hashstat-aml88-bridge.h"
#include <string.h>

static bool valid(const struct hs_aml88_bridge *b) { return b && b->self == b; }
static bool running(const struct hs_aml88_bridge *b)
{
    const struct hs_miner_lifecycle *l = b->lifecycle;
    return l && l->abi == HS_MINER_LIFECYCLE_ABI &&
        l->phase == HS_MINER_RUNNING && l->intent == HS_MINER_COMMAND_RUN &&
        !l->owner_quiescent && l->attempt_generation &&
        !b->cleanup_errors && !b->exhausted;
}
static struct hs_ab_event event(enum hs_ab_status status)
{
    struct hs_ab_event result = {0}; result.status = status; return result;
}
static void discard_slot(struct hs_aml88_bridge *b, struct hs_ab_slot *s)
{
    struct work *w = s->work;
    memset(s, 0, sizeof(*s));
    if (w) b->ops.discard(b->opaque, w);
}
static void discard_jobs(struct hs_aml88_bridge *b, struct hs_ab_chain *c)
{
    for (unsigned i = 0; i < HS_JOB_CACHE_SLOTS; ++i) discard_slot(b, &c->slots[i]);
    c->scope_valid = 0;
    memset(&c->scope, 0, sizeof(c->scope));
}
unsigned hs_aml88_bridge_stop(struct hs_aml88_bridge *b)
{
    if (!valid(b)) return 0;
    for (unsigned i = 0; i < HS_AML88_CHAIN_COUNT; ++i) {
        struct hs_ab_chain *c = &b->chains[i];
        if (c->chain) b->cleanup_errors |= hs_aml_chain_stop(c->chain);
        c->chain = NULL;
        discard_jobs(b, c);
    }
    return b->cleanup_errors;
}
static enum hs_ab_status gate(struct hs_aml88_bridge *b, unsigned index)
{
    if (!valid(b) || index >= HS_AML88_CHAIN_COUNT) return HS_AB_ARGUMENT;
    struct hs_ab_chain *c = &b->chains[index];
    if (!running(b) || !c->chain || c->chain->self != c->chain ||
        !c->chain->active || c->chain->lifecycle != b->lifecycle ||
        c->generation != b->lifecycle->attempt_generation ||
        c->chain->generation != c->generation || !c->chain->uart ||
        !c->chain->uart->active || !c->chain->uart->writes_authorized ||
        c->chain->uart->chain != index) {
        (void)hs_aml88_bridge_stop(b);
        return b->exhausted ? HS_AB_EXHAUSTED : HS_AB_NOT_READY;
    }
    return HS_AB_OK;
}
enum hs_ab_status hs_aml88_bridge_init(struct hs_aml88_bridge *b,
    const struct hs_aml88_profile *profile, const struct hs_miner_lifecycle *life,
    const struct hs_ab_ops *ops, void *opaque)
{
    if (!b || b->self || !hs_aml88_profile_is_known(profile) || !life ||
        life->abi != HS_MINER_LIFECYCLE_ABI || !ops || !ops->clone ||
        !ops->discard || !ops->export_work || !ops->current || !ops->submit)
        return HS_AB_ARGUMENT;
    const struct hs_ab_ops saved_ops = *ops;
    memset(b, 0, sizeof(*b));
    b->self = b; b->profile = profile; b->lifecycle = life;
    b->ops = saved_ops; b->opaque = opaque; b->next_work_tag = 1;
    return HS_AB_OK;
}
enum hs_ab_status hs_aml88_bridge_attach(struct hs_aml88_bridge *b, unsigned index,
                                        struct hs_aml_chain *chain)
{
    if (!valid(b) || index >= HS_AML88_CHAIN_COUNT || !chain ||
        chain->self != chain || b->chains[index].chain) return HS_AB_ARGUMENT;
    if (!running(b) || !chain->active || chain->cleanup_errors ||
        chain->lifecycle != b->lifecycle || !chain->uart ||
        !chain->uart->active || !chain->uart->writes_authorized ||
        chain->uart->chain != index ||
        chain->generation != b->lifecycle->attempt_generation ||
        chain->generation <= b->chains[index].generation) return HS_AB_NOT_READY;
    if (hs_aml_chain_reset_jobs(chain, chain->jobs.session_tag) != HS_CHAIN_OK)
        return HS_AB_NOT_READY;
    b->chains[index].chain = chain;
    b->chains[index].generation = chain->generation;
    return HS_AB_OK;
}
static enum hs_ab_status flush_chain(struct hs_aml88_bridge *b, unsigned index)
{
    struct hs_ab_chain *c = &b->chains[index];
    uint64_t tag = c->chain->jobs.session_tag;
    discard_jobs(b, c);
    if (tag == UINT64_MAX) {
        b->exhausted = 1;
        (void)hs_aml88_bridge_stop(b);
        return HS_AB_EXHAUSTED;
    }
    if (hs_aml_chain_reset_jobs(c->chain, tag + 1U) != HS_CHAIN_OK) {
        (void)hs_aml88_bridge_stop(b);
        return HS_AB_NOT_READY;
    }
    return HS_AB_OK;
}
enum hs_ab_status hs_aml88_bridge_flush(struct hs_aml88_bridge *b)
{
    if (!valid(b)) return HS_AB_ARGUMENT;
    for (unsigned i = 0; i < HS_AML88_CHAIN_COUNT; ++i) {
        if (b->chains[i].chain) {
            enum hs_ab_status status = gate(b, i);
            if (status != HS_AB_OK) return status;
            status = flush_chain(b, i);
            if (status != HS_AB_OK) return status;
        }
    }
    return HS_AB_OK;
}
static bool same_scope(const struct hs_ab_scope *a, const struct hs_ab_scope *b)
{
    return a->pool_identity == b->pool_identity && a->session_epoch == b->session_epoch &&
        a->clean_epoch == b->clean_epoch && a->mask_epoch == b->mask_epoch;
}
struct hs_ab_event hs_aml88_bridge_send(struct hs_aml88_bridge *b, unsigned index,
    unsigned slot, const struct work *source, uint64_t now,
    uint32_t max_age_ms, uint32_t timeout_ms)
{
    enum hs_ab_status status = gate(b, index);
    struct hs_ab_export exported = {0};
    struct hs_job_snapshot job = {0};
    struct hs_ab_event result;
    struct work *copy;
    if (status != HS_AB_OK) return event(status);
    if (!source || slot >= HS_JOB_CACHE_SLOTS || !max_age_ms || !timeout_ms ||
        timeout_ms > HS_AML_UART_MAX_TIMEOUT_MS) return event(HS_AB_ARGUMENT);
    if (!b->next_work_tag) {
        b->exhausted = 1; (void)hs_aml88_bridge_stop(b); return event(HS_AB_EXHAUSTED);
    }
    copy = b->ops.clone(b->opaque, source);
    if (!copy) return event(HS_AB_WORK_REJECTED);
    if (!b->ops.export_work(b->opaque, copy, &exported) ||
        !exported.scope.pool_identity || !exported.scope.session_epoch ||
        !exported.job_sequence || !b->ops.current(b->opaque, copy)) {
        b->ops.discard(b->opaque, copy);
        (void)hs_aml88_bridge_flush(b);
        return event(HS_AB_STALE);
    }
    struct hs_ab_chain *c = &b->chains[index];
    if (c->scope_valid && !same_scope(&c->scope, &exported.scope)) {
        status = flush_chain(b, index);
        if (status != HS_AB_OK) {
            b->ops.discard(b->opaque, copy); return event(status);
        }
    }
    job.session_tag = c->chain->jobs.session_tag;
    job.job_tag = exported.job_sequence; job.work_tag = b->next_work_tag;
    job.issued_ms = now; job.max_age_ms = max_age_ms;
    job.version_mask = exported.version_mask;
    memcpy(job.header, exported.header, sizeof(job.header));
    memcpy(job.share_target_le, exported.target_le, sizeof(job.share_target_le));
    if (hs_job_snapshot_validate(&job, job.session_tag) != HS_JOB_OK) {
        b->ops.discard(b->opaque, copy); return event(HS_AB_WORK_REJECTED);
    }
    b->next_work_tag = b->next_work_tag == UINT64_MAX ? 0 : b->next_work_tag + 1U;

    discard_slot(b, &c->slots[slot]);
    result = event(HS_AB_TRANSFER_FAILED);
    result.chain = hs_aml_chain_send(c->chain, slot, &job, now, timeout_ms);
    if (result.chain.status != HS_CHAIN_OK) {
        b->ops.discard(b->opaque, copy);
        (void)hs_aml88_bridge_stop(b);
        return result;
    }
    if (!b->ops.current(b->opaque, copy)) {
        b->ops.discard(b->opaque, copy);
        (void)hs_aml88_bridge_flush(b); result.status = HS_AB_STALE; return result;
    }
    c->slots[slot].work = copy;
    c->slots[slot].job_tag = job.job_tag; c->slots[slot].work_tag = job.work_tag;
    c->scope = exported.scope; c->scope_valid = 1;
    result.status = HS_AB_OK;
    return result;
}
struct hs_ab_event hs_aml88_bridge_push(struct hs_aml88_bridge *b, unsigned index,
                                       uint8_t byte, uint64_t now)
{
    enum hs_ab_status status = gate(b, index);
    struct hs_ab_event result;
    if (status != HS_AB_OK) return event(status);
    struct hs_ab_chain *c = &b->chains[index];
    result = event(HS_AB_EVENT);
    result.chain = hs_aml_chain_push(c->chain, byte, now);
    if (!c->chain->active) {
        (void)hs_aml88_bridge_stop(b); result.status = HS_AB_NOT_READY; return result;
    }
    if (result.chain.status != HS_CHAIN_SHARE) return result;
    const struct hs_checked_share *share = &result.chain.share;
    if (share->slot >= HS_JOB_CACHE_SLOTS) {
        (void)hs_aml88_bridge_stop(b); result.status = HS_AB_SHARE_REJECTED; return result;
    }
    struct hs_ab_slot *s = &c->slots[share->slot];
    if (!s->work || share->work_tag != s->work_tag || share->job_tag != s->job_tag ||
        share->session_tag != c->chain->jobs.session_tag ||
        !b->ops.current(b->opaque, s->work)) {
        (void)hs_aml88_bridge_flush(b); result.status = HS_AB_STALE; return result;
    }
    result.status = b->ops.submit(b->opaque, s->work, share) ?
        HS_AB_SHARE_HANDOFF : HS_AB_SHARE_REJECTED;
    return result;
}
