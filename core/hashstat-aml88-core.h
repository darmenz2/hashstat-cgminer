/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef HASHSTAT_AML88_CORE_H
#define HASHSTAT_AML88_CORE_H
#include "hashstat-aml88-bridge.h"
struct thr_info;
struct hs_ab_core_context {
    struct thr_info *thread;

    bool (*submit_tested)(struct thr_info *, struct work *);
};
extern const struct hs_ab_ops hashstat_aml88_core_ops;
#endif
