/* SPDX-License-Identifier: GPL-3.0-only */
#include "hs_bm1362_pll.h"
#include "hs_span.h"

#include <stddef.h>

enum hs_bm1362_pll_status hs_bm1362_pll_for_frequency(
    int32_t frequency_units, struct hs_bm1362_pll_result *output)
{
    struct hs_bm1362_pll_result selected = {0};
    unsigned reference_divider, divider_a, divider_b;
    double requested = (double)frequency_units;
    double best_error = 2.5;
    int found = 0;

    if (output == NULL) return HS_BM1362_PLL_INVALID_ARGUMENT;
    if (hs_span_overflows(output, sizeof(*output))) {
        return HS_BM1362_PLL_ADDRESS_OVERFLOW;
    }
    if (frequency_units <= 0) return HS_BM1362_PLL_INVALID_FREQUENCY;

    for (reference_divider = 2U; reference_divider != 0U; --reference_divider) {
        for (divider_b = 1U; divider_b <= 7U; ++divider_b) {
            for (divider_a = divider_b; divider_a <= 7U; ++divider_a) {
                double feedback_float = (double)divider_a * requested;
                uint32_t feedback;
                double oscillator, achieved, error;

                feedback_float *= (double)divider_b;
                feedback_float *= (double)reference_divider;
                feedback_float /= 25.0;

                if (!(feedback_float >= 1.0 && feedback_float < 251.0)) continue;
                feedback = (uint32_t)feedback_float;
                oscillator = (25.0 * (double)feedback) / (double)reference_divider;
                if (oscillator < 2000.0 - 0.1 || oscillator > 3200.0 + 0.1) continue;
                if (reference_divider != 1U && oscillator > 3125.1) continue;
                achieved = oscillator / (double)(divider_a * divider_b);
                error = requested - achieved;
                if (error < 0.0) error = -error;

                if (found != 0 && !(error < best_error + 0.1)) continue;
                selected.reference_divider = (uint32_t)reference_divider;
                selected.feedback_multiplier = feedback;
                selected.divider_a = (uint32_t)divider_a;
                selected.divider_b = (uint32_t)divider_b;
                selected.oscillator_units = oscillator;
                selected.achieved_frequency_units = achieved;
                selected.absolute_error_units = error;
                best_error = error;
                found = 1;
                if (error < 0.1) goto selection_finished;
            }
        }
    }

selection_finished:
    if (found == 0) return HS_BM1362_PLL_NO_SOLUTION;

    if (selected.oscillator_units < 2000.0 || selected.oscillator_units > 3200.0) {
        return HS_BM1362_PLL_NO_SOLUTION;
    }
    selected.register_value =
        (selected.oscillator_units < 2400.0 ? UINT32_C(0x40000000) : UINT32_C(0x50000000)) |
        ((selected.feedback_multiplier & UINT32_C(0xfff)) << 16U) |
        ((selected.reference_divider & UINT32_C(0x3f)) << 8U) |
        ((selected.divider_a * UINT32_C(16) + UINT32_C(0x70)) & UINT32_C(0x70)) |
        ((selected.divider_b - UINT32_C(1)) & UINT32_C(7));
    *output = selected;
    return HS_BM1362_PLL_OK;
}
