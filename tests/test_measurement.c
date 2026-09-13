/* SPDX-License-Identifier: GPL-3.0-only */

#include "hs_measurement.h"

#include <float.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 0; \
    } \
} while (0)

static double number_from_bits(uint64_t bits)
{
    double result;
    memcpy(&result, &bits, sizeof(result));
    return result;
}

static uint64_t number_bits(double value)
{
    uint64_t result;
    memcpy(&result, &value, sizeof(result));
    return result;
}

static int test_status_values(void)
{
    CHECK(HS_OK == 0 && HS_NO_DATA == 1 && HS_INVALID == 2 && HS_NONFINITE == 3);
    return 1;
}

static int test_sweep_normal(void)
{
    double out = -123.0;
    CHECK(hs_sweep_chip_percent(50, 100.0, &out) == HS_OK);
    CHECK(out == 50.0);
    return 1;
}

static int test_sweep_fallback(void)
{
    const double denominators[] = {0.0, -0.0, -10.0, 0.0009, -DBL_MAX};
    size_t i;
    for (i = 0; i < sizeof(denominators) / sizeof(denominators[0]); ++i) {
        double out = 1.0;
        CHECK(hs_sweep_chip_percent(100, denominators[i], &out) == HS_OK);
        CHECK(out == (100.0 / 6794.4) * 100.0);
    }
    return 1;
}

static int test_sweep_threshold_and_preceding_float(void)
{
    double out = 0.0;
    double preceding = number_from_bits(number_bits(0.001) - 1);
    CHECK(hs_sweep_chip_percent(1, 0.001, &out) == HS_OK && out == 100000.0);
    CHECK(hs_sweep_chip_percent(1, preceding, &out) == HS_OK);
    CHECK(out == (1.0 / 6794.4) * 100.0);
    return 1;
}

static int test_sweep_not_clamped(void)
{
    double out = 0.0;
    CHECK(hs_sweep_chip_percent(-5, 10.0, &out) == HS_OK && out == -50.0);
    CHECK(hs_sweep_chip_percent(20, 10.0, &out) == HS_OK && out == 200.0);
    return 1;
}

static int test_sweep_int32_endpoints(void)
{
    double out = 0.0;
    CHECK(hs_sweep_chip_percent(INT32_MIN, 1.0, &out) == HS_OK);
    CHECK(out == (double)INT32_MIN * 100.0);
    CHECK(hs_sweep_chip_percent(INT32_MAX, 1.0, &out) == HS_OK);
    CHECK(out == (double)INT32_MAX * 100.0);
    return 1;
}

static int test_sweep_eligible_average(void)
{
    const hs_sweep_chip chips[] = {{50, 100.0, 0}, {100, 100.0, 0}, {999, 1.0, 1}};
    double out = 0.0;
    CHECK(hs_sweep_chain_percent(chips, 3, &out) == HS_OK && out == 75.0);
    return 1;
}

static int test_sweep_no_data_keeps_output(void)
{
    const hs_sweep_chip chip = {5, 10.0, 1};
    double out = -1234.25;
    CHECK(hs_sweep_chain_percent(NULL, 0, &out) == HS_NO_DATA && out == -1234.25);
    CHECK(hs_sweep_chain_percent(&chip, 1, &out) == HS_NO_DATA && out == -1234.25);
    return 1;
}

static int test_sweep_bad_flag_atomic_failure(void)
{
    const hs_sweep_chip chips[] = {{50, 100.0, 0}, {100, 100.0, 2}};
    double out = -1234.25;
    CHECK(hs_sweep_chain_percent(chips, 2, &out) == HS_INVALID && out == -1234.25);
    return 1;
}

static int test_sweep_nonfinite_atomic_failure(void)
{
    const uint64_t values[] = {UINT64_C(0x7ff8000000000001), UINT64_C(0x7ff0000000000000),
                               UINT64_C(0xfff0000000000000), UINT64_C(0x7ff0000000000001)};
    size_t i;
    for (i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        hs_sweep_chip chips[] = {{1, 1.0, 0}, {1, number_from_bits(values[i]), 1}};
        double out = -1234.25;
        CHECK(hs_sweep_chip_percent(1, chips[1].denominator, &out) == HS_NONFINITE);
        CHECK(out == -1234.25);
        CHECK(hs_sweep_chain_percent(chips, 2, &out) == HS_NONFINITE && out == -1234.25);
    }
    return 1;
}

static int test_nonce_formula_and_exact_constant(void)
{
    double out = 0.0;
    CHECK(hs_nonce_chip_measured(2, 3, 4, &out) == HS_OK);
    CHECK(out == ((2.0 * 4294967295.0 * 3.0) / 4.0) / 1000000000.0);
    CHECK(hs_nonce_chip_measured(1, 1, 1, &out) == HS_OK);
    CHECK(out == 4.294967295 && out != 4.294967296);
    return 1;
}

static int test_nonce_zero_and_signed_counter(void)
{
    double out = 0.0;
    CHECK(hs_nonce_chip_measured(0, 3, 4, &out) == HS_OK && out == 0.0);
    CHECK(hs_nonce_chip_measured(2, 0, 4, &out) == HS_OK && out == 0.0);
    CHECK(hs_nonce_chip_measured(1, -1, 1, &out) == HS_OK && out == -4.294967295);
    return 1;
}

static int test_nonce_fixed_width_endpoints(void)
{
    double out = 0.0;
    double expected = ((double)UINT32_MAX * 4294967295.0 * (double)INT32_MAX) / 1000000000.0;
    CHECK(hs_nonce_chip_measured(UINT32_MAX, INT32_MAX, 1, &out) == HS_OK && out == expected);
    CHECK(hs_nonce_chip_measured(1, INT32_MIN, INT32_MAX, &out) == HS_OK && out < 0.0);
    return 1;
}

static int test_nonce_invalid_n_keeps_output(void)
{
    const int32_t values[] = {0, -1, INT32_MIN};
    size_t i;
    for (i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        double out = -1234.25;
        CHECK(hs_nonce_chip_measured(1, 1, values[i], &out) == HS_INVALID && out == -1234.25);
        CHECK(hs_nonce_chain_measured(40.0, values[i], NULL, 0, &out) == HS_INVALID);
        CHECK(out == -1234.25);
    }
    return 1;
}

static int test_nonce_chain_preserves_previous(void)
{
    const hs_flagged_value samples[] = {{3.0, 0}, {7.0, 0}, {999.0, 1}};
    double out = 0.0;
    CHECK(hs_nonce_chain_measured(40.0, 4, samples, 3, &out) == HS_OK && out == 20.0);
    CHECK(hs_nonce_chain_measured(40.0, 4, NULL, 0, &out) == HS_OK && out == 10.0);
    return 1;
}

static int test_nonce_chain_ordered_sum(void)
{
    const hs_flagged_value samples[] = {{-1e16, 0}, {1.0, 0}};
    double out = 0.0;
    CHECK(hs_nonce_chain_measured(1e16, 1, samples, 2, &out) == HS_OK && out == 1.0);
    return 1;
}

static int test_normalization_ordinary_and_zero(void)
{
    double out = 0.0;
    CHECK(hs_nonce_chip_percent(50.0, 100.0, &out) == HS_OK && out == 50.0);
    CHECK(hs_nonce_chip_percent(50.0, 0.0, &out) == HS_OK && out == 0.0);
    CHECK(hs_nonce_chip_percent(DBL_MAX, -0.0, &out) == HS_OK && out == 0.0);
    CHECK(number_bits(out) == 0);
    return 1;
}

static int test_normalization_clamps(void)
{
    double out = 0.0;
    CHECK(hs_nonce_chip_percent(-1.0, 100.0, &out) == HS_OK && out == 0.0);
    CHECK(hs_nonce_chip_percent(101.0, 100.0, &out) == HS_OK && out == 100.0);
    CHECK(hs_nonce_chip_percent(100.0, 100.0, &out) == HS_OK && out == 100.0);
    return 1;
}

static int test_normalization_negative_denominator(void)
{
    double out = 0.0;
    CHECK(hs_nonce_chip_percent(5.0, -10.0, &out) == HS_OK && out == 0.0);
    CHECK(hs_nonce_chip_percent(-5.0, -10.0, &out) == HS_OK && out == 50.0);
    return 1;
}

static int test_normalization_canonical_positive_zero(void)
{
    double out = 0.0;
    CHECK(hs_nonce_chip_percent(-0.0, 1.0, &out) == HS_OK && number_bits(out) == 0);
    CHECK(hs_nonce_chip_percent(0.0, -1.0, &out) == HS_OK && number_bits(out) == 0);
    return 1;
}

static int test_normalization_nonfinite_keeps_output(void)
{
    const uint64_t values[] = {UINT64_C(0x7ff8000000000001), UINT64_C(0x7ff0000000000000),
                               UINT64_C(0xfff0000000000000)};
    size_t i;
    for (i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        double bad = number_from_bits(values[i]);
        double out = -1234.25;
        CHECK(hs_nonce_chip_percent(1.0, bad, &out) == HS_NONFINITE && out == -1234.25);
        CHECK(hs_nonce_chip_percent(bad, 0.0, &out) == HS_NONFINITE && out == -1234.25);
    }
    return 1;
}

static int test_normalization_intermediate_overflow(void)
{
    double out = -1234.25;
    CHECK(hs_nonce_chip_percent(DBL_MAX, DBL_MAX, &out) == HS_NONFINITE);
    CHECK(out == -1234.25);
    return 1;
}

static int test_nonce_chain_aggregate_ratio(void)
{
    const hs_flagged_value expected[] = {{100.0, 0}, {300.0, 0}, {9000.0, 1}};
    double out = 0.0;
    CHECK(hs_nonce_chain_percent(100.0, expected, 3, &out) == HS_OK && out == 25.0);
    CHECK(out != 50.0);
    return 1;
}

static int test_nonce_chain_zero_expected(void)
{
    const hs_flagged_value excluded = {100.0, 1};
    const hs_flagged_value cancel[] = {{100.0, 0}, {-100.0, 0}};
    double out = -1.0;
    CHECK(hs_nonce_chain_percent(50.0, NULL, 0, &out) == HS_OK && out == 0.0);
    CHECK(hs_nonce_chain_percent(50.0, &excluded, 1, &out) == HS_OK && out == 0.0);
    CHECK(hs_nonce_chain_percent(50.0, cancel, 2, &out) == HS_OK && out == 0.0);
    return 1;
}

static int test_nonce_chain_clamps(void)
{
    const hs_flagged_value expected = {100.0, 0};
    double out = -1.0;
    CHECK(hs_nonce_chain_percent(-1.0, &expected, 1, &out) == HS_OK && out == 0.0);
    CHECK(hs_nonce_chain_percent(1000.0, &expected, 1, &out) == HS_OK && out == 100.0);
    return 1;
}

static int test_nonce_aggregate_bad_flags_are_atomic(void)
{
    const hs_flagged_value samples[] = {{1.0, 0}, {2.0, 255}};
    double out = -1234.25;
    CHECK(hs_nonce_chain_measured(0.0, 1, samples, 2, &out) == HS_INVALID && out == -1234.25);
    CHECK(hs_nonce_chain_percent(1.0, samples, 2, &out) == HS_INVALID && out == -1234.25);
    return 1;
}

static int test_nonce_aggregate_nonfinite_excluded_is_rejected(void)
{
    hs_flagged_value samples[] = {{1.0, 0}, {number_from_bits(UINT64_C(0x7ff8000000000001)), 1}};
    double out = -1234.25;
    CHECK(hs_nonce_chain_measured(0.0, 1, samples, 2, &out) == HS_NONFINITE && out == -1234.25);
    CHECK(hs_nonce_chain_percent(1.0, samples, 2, &out) == HS_NONFINITE && out == -1234.25);
    CHECK(hs_nonce_chain_measured(samples[1].value, 1, NULL, 0, &out) == HS_NONFINITE);
    CHECK(out == -1234.25);
    return 1;
}

static int test_nonce_aggregate_overflow_is_atomic(void)
{
    const hs_flagged_value samples[] = {{DBL_MAX, 0}, {DBL_MAX, 0}};
    double out = -1234.25;
    CHECK(hs_nonce_chain_percent(1.0, samples, 2, &out) == HS_NONFINITE && out == -1234.25);
    CHECK(hs_nonce_chain_measured(DBL_MAX, 1, samples, 1, &out) == HS_NONFINITE && out == -1234.25);
    return 1;
}

static int test_null_output_rejected(void)
{
    CHECK(hs_sweep_chip_percent(1, 1.0, NULL) == HS_INVALID);
    CHECK(hs_sweep_chain_percent(NULL, 0, NULL) == HS_INVALID);
    CHECK(hs_nonce_chip_measured(1, 1, 1, NULL) == HS_INVALID);
    CHECK(hs_nonce_chain_measured(0.0, 1, NULL, 0, NULL) == HS_INVALID);
    CHECK(hs_nonce_chip_percent(1.0, 1.0, NULL) == HS_INVALID);
    CHECK(hs_nonce_chain_percent(1.0, NULL, 0, NULL) == HS_INVALID);
    return 1;
}

static int test_null_nonempty_arrays_are_atomic(void)
{
    double out = -1234.25;
    CHECK(hs_sweep_chain_percent(NULL, 1, &out) == HS_INVALID && out == -1234.25);
    CHECK(hs_nonce_chain_measured(0.0, 1, NULL, 1, &out) == HS_INVALID && out == -1234.25);
    CHECK(hs_nonce_chain_percent(1.0, NULL, 1, &out) == HS_INVALID && out == -1234.25);
    return 1;
}

static int test_impossible_counts_are_rejected_without_read(void)
{
    const hs_sweep_chip chip = {1, 1.0, 0};
    const hs_flagged_value value = {1.0, 0};
    double out = -1234.25;
    CHECK(hs_sweep_chain_percent(&chip, SIZE_MAX / sizeof(chip) + 1, &out) == HS_INVALID);
    CHECK(out == -1234.25);
    CHECK(hs_nonce_chain_measured(0.0, 1, &value, SIZE_MAX / sizeof(value) + 1, &out) == HS_INVALID);
    CHECK(out == -1234.25);
    CHECK(hs_nonce_chain_percent(1.0, &value, SIZE_MAX / sizeof(value) + 1, &out) == HS_INVALID);
    CHECK(out == -1234.25);
    return 1;
}

int main(void)
{
    const struct { const char *name; int (*run)(void); } tests[] = {
        {"status values", test_status_values},
        {"sweep ordinary", test_sweep_normal},
        {"sweep fallback", test_sweep_fallback},
        {"sweep exact boundary", test_sweep_threshold_and_preceding_float},
        {"sweep unclamped", test_sweep_not_clamped},
        {"sweep s32 endpoints", test_sweep_int32_endpoints},
        {"sweep eligible average", test_sweep_eligible_average},
        {"sweep no-data atomic", test_sweep_no_data_keeps_output},
        {"sweep flag guard atomic", test_sweep_bad_flag_atomic_failure},
        {"sweep nonfinite atomic", test_sweep_nonfinite_atomic_failure},
        {"nonce exact formula", test_nonce_formula_and_exact_constant},
        {"nonce zero and signed counter", test_nonce_zero_and_signed_counter},
        {"nonce fixed-width endpoints", test_nonce_fixed_width_endpoints},
        {"nonce invalid N atomic", test_nonce_invalid_n_keeps_output},
        {"nonce previous term", test_nonce_chain_preserves_previous},
        {"nonce ordered sum", test_nonce_chain_ordered_sum},
        {"normalization ordinary and zero", test_normalization_ordinary_and_zero},
        {"normalization clamps", test_normalization_clamps},
        {"normalization negative E", test_normalization_negative_denominator},
        {"normalization signed zero", test_normalization_canonical_positive_zero},
        {"normalization nonfinite atomic", test_normalization_nonfinite_keeps_output},
        {"normalization intermediate overflow", test_normalization_intermediate_overflow},
        {"chain aggregate ratio", test_nonce_chain_aggregate_ratio},
        {"chain zero expected", test_nonce_chain_zero_expected},
        {"chain clamps", test_nonce_chain_clamps},
        {"chain flag guard atomic", test_nonce_aggregate_bad_flags_are_atomic},
        {"chain excluded nonfinite", test_nonce_aggregate_nonfinite_excluded_is_rejected},
        {"chain overflow atomic", test_nonce_aggregate_overflow_is_atomic},
        {"null out", test_null_output_rejected},
        {"null nonempty arrays", test_null_nonempty_arrays_are_atomic},
        {"impossible counts", test_impossible_counts_are_rejected_without_read}
    };
    size_t i;
    for (i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        if (!tests[i].run()) {
            fprintf(stderr, "FAILED: %s\n", tests[i].name);
            return 1;
        }
    }
    printf("%zu native measurement tests passed\n", sizeof(tests) / sizeof(tests[0]));
    return 0;
}
