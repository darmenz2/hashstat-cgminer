/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef HS_AML_READONLY_H
#define HS_AML_READONLY_H
#include "hs_aml_observe.h"

struct hs_aml_text hs_aml_readonly_attribute(void *context, unsigned chain,
                                           enum hs_aml_attribute attribute);
#endif
