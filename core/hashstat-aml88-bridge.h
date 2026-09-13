/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef HASHSTAT_AML88_BRIDGE_H
#define HASHSTAT_AML88_BRIDGE_H
#include <stdbool.h>
#include "hs_aml88_profile.h"
#include "hs_aml_chain.h"

struct work;
struct hs_ab_scope {
    const void *pool_identity;
    uint64_t session_epoch, clean_epoch, mask_epoch;
};
struct hs_ab_export {
    struct hs_ab_scope scope;
    uint64_t job_sequence;
    uint32_t version_mask;
    uint8_t header[80], target_le[32];
};
struct hs_ab_ops {
    struct work *(*clone)(void *, const struct work *);
    void (*discard)(void *, struct work *);
    bool (*export_work)(void *, struct work *, struct hs_ab_export *);
    bool (*current)(void *, struct work *);

    bool (*submit)(void *, const struct work *, const struct hs_checked_share *);
};
enum hs_ab_status {
    HS_AB_ARGUMENT = 0, HS_AB_OK, HS_AB_NOT_READY, HS_AB_STALE,
    HS_AB_WORK_REJECTED, HS_AB_TRANSFER_FAILED, HS_AB_EVENT,
    HS_AB_SHARE_HANDOFF, HS_AB_SHARE_REJECTED, HS_AB_EXHAUSTED
};
struct hs_ab_event {
    enum hs_ab_status status;
    struct hs_aml_chain_event chain;
};
struct hs_ab_slot { struct work *work; uint64_t work_tag, job_tag; };
struct hs_ab_chain {
    struct hs_aml_chain *chain;
    struct hs_ab_scope scope;
    uint64_t generation;
    unsigned scope_valid;
    struct hs_ab_slot slots[HS_JOB_CACHE_SLOTS];
};
struct hs_aml88_bridge {
    const struct hs_aml88_bridge *self;
    const struct hs_aml88_profile *profile;
    const struct hs_miner_lifecycle *lifecycle;
    struct hs_ab_ops ops;
    void *opaque;
    uint64_t next_work_tag;
    unsigned cleanup_errors, exhausted;
    struct hs_ab_chain chains[HS_AML88_CHAIN_COUNT];
};

enum hs_ab_status hs_aml88_bridge_init(struct hs_aml88_bridge *,
    const struct hs_aml88_profile *, const struct hs_miner_lifecycle *,
    const struct hs_ab_ops *, void *opaque);

enum hs_ab_status hs_aml88_bridge_attach(struct hs_aml88_bridge *, unsigned,
                                        struct hs_aml_chain *);
struct hs_ab_event hs_aml88_bridge_send(struct hs_aml88_bridge *, unsigned chain,
    unsigned slot, const struct work *, uint64_t now_ms,
    uint32_t max_age_ms, uint32_t timeout_ms);
struct hs_ab_event hs_aml88_bridge_push(struct hs_aml88_bridge *, unsigned chain,
                                       uint8_t byte, uint64_t now_ms);

enum hs_ab_status hs_aml88_bridge_flush(struct hs_aml88_bridge *);
unsigned hs_aml88_bridge_stop(struct hs_aml88_bridge *);
#endif
