/* SPDX-License-Identifier: GPL-3.0-only */

#include "config.h"
#include "miner.h"
#include "hashstat-core.h"
#include "hashstat-aml88-core.h"
#include <string.h>

static struct work *clone_work(void *opaque, const struct work *source)
{
    (void)opaque;

    return source ? copy_work((struct work *)source) : NULL;
}
static void discard_work_copy(void *opaque, struct work *work)
{
    (void)opaque;
    if (work) free_work(work);
}
static bool current_work(void *opaque, struct work *work)
{
    (void)opaque;
    return work && work->stratum && hashstat_work_current(work);
}
static bool export_work(void *opaque, struct work *work, struct hs_ab_export *out)
{
    struct hs_ab_export result = {0};
    if (!out || !current_work(opaque, work)) return false;

    if (work->hs_snapshot.rolling && !hashstat_roll_work_version(work, 0)) return false;
    if (!hs_core_flip_header80(work->data, 80, result.header)) return false;
    memcpy(result.target_le, work->target, sizeof(result.target_le));
    result.scope.pool_identity = work->pool;
    result.scope.session_epoch = work->hs_snapshot.session_epoch;
    result.scope.clean_epoch = work->hs_snapshot.clean_epoch;
    result.scope.mask_epoch = work->hs_snapshot.mask_epoch;
    result.job_sequence = work->hs_snapshot.job_sequence;
    result.version_mask = work->hs_snapshot.rolling ? work->hs_snapshot.mask : 0;
    if (!current_work(opaque, work)) return false;
    *out = result;
    return true;
}
static bool submit_share(void *opaque, const struct work *stored,
                         const struct hs_checked_share *share)
{
    struct hs_ab_core_context *context = opaque;
    struct work *candidate;
    bool result = false;
    uint32_t version;
    if (!context || !context->thread || !stored || !share) return false;
    candidate = copy_work((struct work *)stored);
    if (!candidate) return false;
    if (!current_work(opaque, candidate)) goto done;
    if (candidate->hs_snapshot.rolling) {
        if (!hashstat_roll_work_version(candidate, share->version_bits)) goto done;
    } else if (share->version_bits != 0) goto done;
    version = candidate->hs_snapshot.actual_version;
    if (version != share->full_version) goto done;

    candidate->data[76] = (uint8_t)(share->nonce >> 24);
    candidate->data[77] = (uint8_t)(share->nonce >> 16);
    candidate->data[78] = (uint8_t)(share->nonce >> 8);
    candidate->data[79] = (uint8_t)share->nonce;
    if (!hs_core_hash_work80(candidate->data, 80, candidate->hash) ||
        memcmp(candidate->hash, share->digest, 32) ||
        !fulltest(candidate->hash, candidate->target) ||
        !current_work(opaque, candidate)) goto done;

    result = context->submit_tested ? context->submit_tested(context->thread, candidate) :
                                    submit_tested_work(context->thread, candidate);
done:
    free_work(candidate);
    return result;
}
const struct hs_ab_ops hashstat_aml88_core_ops = {
    clone_work, discard_work_copy, export_work, current_work, submit_share
};
