/* SPDX-License-Identifier: GPL-3.0-only */

#ifndef HS_MEASUREMENT_H
#define HS_MEASUREMENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum hs_status {
    HS_OK = 0,
    HS_NO_DATA = 1,
    HS_INVALID = 2,
    HS_NONFINITE = 3
} hs_status;

typedef struct hs_sweep_chip {
    int32_t counter24;
    double denominator;
    uint8_t exclusion_flag;
} hs_sweep_chip;

typedef struct hs_flagged_value {
    double value;
    uint8_t exclusion_flag;
} hs_flagged_value;

hs_status hs_sweep_chip_percent(int32_t counter24, double denominator,
                                double *out);

hs_status hs_sweep_chain_percent(const hs_sweep_chip *chips, size_t count,
                                 double *out);

hs_status hs_nonce_chip_measured(uint32_t multiplier_d, int32_t counter24,
                                 int32_t passes_n, double *out);

hs_status hs_nonce_chain_measured(double previous_chain_measured,
                                  int32_t passes_n,
                                  const hs_flagged_value *chip_measurements,
                                  size_t count, double *out);

hs_status hs_nonce_chip_percent(double measured, double expected_e, double *out);

hs_status hs_nonce_chain_percent(double measured_chain,
                                 const hs_flagged_value *chip_expectations,
                                 size_t count, double *out);

#ifdef __cplusplus
}
#endif
#endif
