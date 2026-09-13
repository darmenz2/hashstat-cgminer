/* SPDX-License-Identifier: GPL-3.0-only */
#include "hs_bm1362_pll.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static int oracle(int32_t requested, struct hs_bm1362_pll_result *out)
{
    static const unsigned pairs[28][2] = {
        {1,1},{2,1},{3,1},{4,1},{5,1},{6,1},{7,1},
        {2,2},{3,2},{4,2},{5,2},{6,2},{7,2},
        {3,3},{4,3},{5,3},{6,3},{7,3},
        {4,4},{5,4},{6,4},{7,4},{5,5},{6,5},{7,5},{6,6},{7,6},{7,7}
    };
    unsigned step, multiplier;
    double previous_error = 2.5;
    int have = 0;
    if (requested <= 0) return 0;
    for (step = 0U; step < 56U; ++step) {
        unsigned reference_divider = step < 28U ? 2U : 1U;
        unsigned a = pairs[step % 28U][0];
        unsigned b = pairs[step % 28U][1];
        uint64_t numerator = (uint64_t)(uint32_t)requested * reference_divider * a * b;
        for (multiplier = 1U; multiplier <= 250U; ++multiplier) {
            double vco, actual, error;
            if (numerator < (uint64_t)multiplier * 25U ||
                numerator >= (uint64_t)(multiplier + 1U) * 25U) continue;
            vco = (double)(25U * multiplier) / (double)reference_divider;
            if (vco < 1999.9 || vco > 3200.1) continue;
            if (reference_divider != 1U && vco > 3125.1) continue;
            actual = vco / (double)(a * b);
            error = (double)requested - actual;
            if (error < 0.0) error = -error;
            if (have != 0 && error >= previous_error + 0.1) continue;
            out->reference_divider = (uint32_t)reference_divider;
            out->feedback_multiplier = (uint32_t)multiplier;
            out->divider_a = (uint32_t)a;
            out->divider_b = (uint32_t)b;
            out->oscillator_units = vco;
            out->achieved_frequency_units = actual;
            out->absolute_error_units = error;
            out->register_value = (vco < 2400.0 ? UINT32_C(0x40000000) : UINT32_C(0x50000000)) +
                (uint32_t)multiplier * UINT32_C(65536) +
                (uint32_t)reference_divider * UINT32_C(256) +
                (uint32_t)(a - 1U) * UINT32_C(16) + (uint32_t)(b - 1U);
            previous_error = error;
            have = 1;
            if (error < 0.1) return 1;
        }
    }
    return have;
}

static void compare(int32_t frequency)
{
    struct hs_bm1362_pll_result actual, before, expected = {0};
    enum hs_bm1362_pll_status status;
    int exists;
    memset(&actual, 0xa6, sizeof(actual));
    memcpy(&before, &actual, sizeof(before));
    exists = oracle(frequency, &expected);
    status = hs_bm1362_pll_for_frequency(frequency, &actual);
    if (frequency <= 0) {
        assert(status == HS_BM1362_PLL_INVALID_FREQUENCY);
        assert(memcmp(&actual, &before, sizeof(actual)) == 0);
    } else if (exists == 0) {
        assert(status == HS_BM1362_PLL_NO_SOLUTION);
        assert(memcmp(&actual, &before, sizeof(actual)) == 0);
    } else {
        assert(status == HS_BM1362_PLL_OK);
        assert(actual.register_value == expected.register_value);
        assert(actual.reference_divider == expected.reference_divider);
        assert(actual.feedback_multiplier == expected.feedback_multiplier);
        assert(actual.divider_a == expected.divider_a && actual.divider_b == expected.divider_b);
        assert(actual.oscillator_units == expected.oscillator_units);
        assert(actual.achieved_frequency_units == expected.achieved_frequency_units);
        assert(actual.absolute_error_units == expected.absolute_error_units);
        assert(actual.oscillator_units >= 2000.0 && actual.oscillator_units <= 3200.0);
        assert(actual.reference_divider >= 1U && actual.reference_divider <= 2U);
        assert(actual.feedback_multiplier > 0U && actual.feedback_multiplier <= 250U);
        assert(actual.divider_b >= 1U && actual.divider_b <= actual.divider_a && actual.divider_a <= 7U);
        assert(actual.achieved_frequency_units <= (double)frequency);
    }
}

int main(void)
{
    static const struct { int32_t frequency; uint32_t value; } fixed[] = {
        {41,UINT32_C(0x40500166)}, {49,UINT32_C(0x50c00266)},
        {50,UINT32_C(0x40a80265)}, {51,UINT32_C(0x40ab0265)},
        {100,UINT32_C(0x40a80262)}, {200,UINT32_C(0x40a00241)},
        {400,UINT32_C(0x40a00240)}, {500,UINT32_C(0x40a00230)},
        {545,UINT32_C(0x50da0240)}, {550,UINT32_C(0x40b00230)},
        {600,UINT32_C(0x50c00230)}, {645,UINT32_C(0x50670111)},
        {650,UINT32_C(0x50d00230)}, {667,UINT32_C(0x40500120)},
        {670,UINT32_C(0x506b0111)}, {700,UINT32_C(0x40a80220)},
        {750,UINT32_C(0x40b40220)}, {830,UINT32_C(0x50c70220)},
        {999,UINT32_C(0x50ef0220)}, {1000,UINT32_C(0x40a00210)},
        {1200,UINT32_C(0x50c00210)}, {1600,UINT32_C(0x50800110)},
        {2000,UINT32_C(0x40a00200)}, {2399,UINT32_C(0x40bf0200)},
        {2400,UINT32_C(0x50c00200)}, {2401,UINT32_C(0x50600100)},
        {3125,UINT32_C(0x50fa0200)}, {3200,UINT32_C(0x50800100)},
        {3201,UINT32_C(0x50800100)}, {3224,UINT32_C(0x50800100)}
    };
    struct hs_bm1362_pll_result result;
    uint32_t state = UINT32_C(0x13628801);
    unsigned long checks = 0UL;
    int32_t frequency;
    size_t index;

    for (frequency = -2; frequency <= 5000; ++frequency) {
        compare(frequency);
        ++checks;
    }
    for (index = 0U; index < 1024U; ++index) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        compare((int32_t)(state & UINT32_C(0x7fffffff)));
        ++checks;
    }
    compare(INT32_MIN);
    compare(INT32_MAX);
    checks += 2UL;
    for (index = 0U; index < sizeof(fixed) / sizeof(fixed[0]); ++index) {
        assert(hs_bm1362_pll_for_frequency(fixed[index].frequency, &result) == HS_BM1362_PLL_OK);
        assert(result.register_value == fixed[index].value);
        ++checks;
    }
    assert(hs_bm1362_pll_for_frequency(645, NULL) == HS_BM1362_PLL_INVALID_ARGUMENT);
    ++checks;
    for (index = 0U; index + 1U < sizeof(result); ++index) {
        struct hs_bm1362_pll_result *invalid =
            (struct hs_bm1362_pll_result *)(UINTPTR_MAX - index);
        assert(hs_bm1362_pll_for_frequency(645, invalid) == HS_BM1362_PLL_ADDRESS_OVERFLOW);
        ++checks;
    }
    printf("BM1362 pure PLL: %lu cases passed; no hardware\n", checks);
    return 0;
}
