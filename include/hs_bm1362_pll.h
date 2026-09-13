/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef HS_BM1362_PLL_H
#define HS_BM1362_PLL_H

#include <stdint.h>

enum hs_bm1362_pll_status {
    HS_BM1362_PLL_OK = 0,
    HS_BM1362_PLL_INVALID_ARGUMENT,
    HS_BM1362_PLL_INVALID_FREQUENCY,
    HS_BM1362_PLL_NO_SOLUTION,
    HS_BM1362_PLL_ADDRESS_OVERFLOW
};

struct hs_bm1362_pll_result {
    uint32_t register_value;
    uint32_t reference_divider;
    uint32_t feedback_multiplier;
    uint32_t divider_a;
    uint32_t divider_b;
    double oscillator_units;
    double achieved_frequency_units;
    double absolute_error_units;
};

enum hs_bm1362_pll_status hs_bm1362_pll_for_frequency(
    int32_t frequency_units, struct hs_bm1362_pll_result *output);

#endif
