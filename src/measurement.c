/* SPDX-License-Identifier: GPL-3.0-only */

#include "hs_measurement.h"

#include <float.h>

#if defined(__FAST_MATH__)
#error "HashStat reference arithmetic requires fast-math to be disabled"
#endif
#if !defined(__GNUC__) && !defined(__clang__)
#error "This freestanding implementation requires compiler builtin memcpy"
#endif
#if defined(__FLOAT_WORD_ORDER__) && defined(__BYTE_ORDER__) && \
    __FLOAT_WORD_ORDER__ != __BYTE_ORDER__
#error "Mixed integer/double endianness is not supported"
#endif
_Static_assert(sizeof(double) == sizeof(uint64_t), "binary64 double required");
_Static_assert(DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024 && DBL_MIN_EXP == -1021,
               "IEEE-754 binary64 range and precision required");

static int hs_finite(double value)
{
    uint64_t bits;

    __builtin_memcpy(&bits, &value, sizeof(bits));
    return (bits & UINT64_C(0x7ff0000000000000)) !=
           UINT64_C(0x7ff0000000000000);
}

static int hs_sweep_input_valid(const hs_sweep_chip *items, size_t count)
{
    return (items != NULL || count == 0) &&
           count <= SIZE_MAX / sizeof(hs_sweep_chip);
}

static int hs_values_input_valid(const hs_flagged_value *items, size_t count)
{
    return (items != NULL || count == 0) &&
           count <= SIZE_MAX / sizeof(hs_flagged_value);
}

hs_status hs_sweep_chip_percent(int32_t counter24, double denominator,
                                double *out)
{
    double result;
    if (out == NULL) {
        return HS_INVALID;
    }
    if (!hs_finite(denominator)) {
        return HS_NONFINITE;
    }
    if (denominator < 0.001) {
        denominator = 6794.4;
    }
    result = ((double)counter24 / denominator) * 100.0;
    if (!hs_finite(result)) {
        return HS_NONFINITE;
    }
    *out = result;
    return HS_OK;
}

hs_status hs_sweep_chain_percent(const hs_sweep_chip *chips, size_t count,
                                 double *out)
{
    size_t i;
    size_t eligible = 0;
    double sum = 0.0;
    double result;
    if (out == NULL || !hs_sweep_input_valid(chips, count)) {
        return HS_INVALID;
    }
    for (i = 0; i < count; ++i) {
        double percent;
        hs_status status;
        if (chips[i].exclusion_flag > 1) {
            return HS_INVALID;
        }
        status = hs_sweep_chip_percent(chips[i].counter24,
                                       chips[i].denominator, &percent);
        if (status != HS_OK) {
            return status;
        }
        eligible += (size_t)(chips[i].exclusion_flag ^ 1u);
        if (chips[i].exclusion_flag == 0) {
            sum = sum + percent;
            if (!hs_finite(sum)) {
                return HS_NONFINITE;
            }
        }
    }
    if (eligible == 0) {
        return HS_NO_DATA;
    }
    result = sum / (double)eligible;
    if (!hs_finite(result)) {
        return HS_NONFINITE;
    }
    *out = result;
    return HS_OK;
}

hs_status hs_nonce_chip_measured(uint32_t multiplier_d, int32_t counter24,
                                 int32_t passes_n, double *out)
{
    double result;
    if (out == NULL || passes_n <= 0) {
        return HS_INVALID;
    }
    result = (double)multiplier_d * 4294967295.0;
    result = result * (double)counter24;
    result = result / (double)passes_n;
    result = result / 1000000000.0;
    if (!hs_finite(result)) {
        return HS_NONFINITE;
    }
    *out = result;
    return HS_OK;
}

hs_status hs_nonce_chain_measured(double previous_chain_measured,
                                  int32_t passes_n,
                                  const hs_flagged_value *chip_measurements,
                                  size_t count, double *out)
{
    size_t i;
    double result;
    if (out == NULL || !hs_values_input_valid(chip_measurements, count)) {
        return HS_INVALID;
    }
    if (!hs_finite(previous_chain_measured)) {
        return HS_NONFINITE;
    }
    if (passes_n <= 0) {
        return HS_INVALID;
    }
    result = previous_chain_measured / (double)passes_n;
    for (i = 0; i < count; ++i) {
        const hs_flagged_value item = chip_measurements[i];
        if (!hs_finite(item.value)) {
            return HS_NONFINITE;
        }
        if (item.exclusion_flag > 1) {
            return HS_INVALID;
        }
        if (item.exclusion_flag == 0) {
            result = result + item.value;
            if (!hs_finite(result)) {
                return HS_NONFINITE;
            }
        }
    }
    *out = result;
    return HS_OK;
}

hs_status hs_nonce_chip_percent(double measured, double expected_e, double *out)
{
    double result;
    if (out == NULL) {
        return HS_INVALID;
    }
    if (!hs_finite(measured) || !hs_finite(expected_e)) {
        return HS_NONFINITE;
    }
    if (expected_e == 0.0) {
        result = 0.0;
    } else {
        result = measured * 100.0;
        result = result / expected_e;
        if (!hs_finite(result)) {
            return HS_NONFINITE;
        }
        if (!(result > 0.0)) {
            result = 0.0;
        } else if (result > 100.0) {
            result = 100.0;
        }
    }
    *out = result;
    return HS_OK;
}

hs_status hs_nonce_chain_percent(double measured_chain,
                                 const hs_flagged_value *chip_expectations,
                                 size_t count, double *out)
{
    size_t i;
    double expected_sum = 0.0;
    double result;
    hs_status status;
    if (out == NULL || !hs_values_input_valid(chip_expectations, count)) {
        return HS_INVALID;
    }
    for (i = 0; i < count; ++i) {
        const hs_flagged_value item = chip_expectations[i];
        if (!hs_finite(item.value)) {
            return HS_NONFINITE;
        }
        if (item.exclusion_flag > 1) {
            return HS_INVALID;
        }
        if (item.exclusion_flag == 0) {
            expected_sum = expected_sum + item.value;
            if (!hs_finite(expected_sum)) {
                return HS_NONFINITE;
            }
        }
    }
    status = hs_nonce_chip_percent(measured_chain, expected_sum, &result);
    if (status == HS_OK) {
        *out = result;
    }
    return status;
}
