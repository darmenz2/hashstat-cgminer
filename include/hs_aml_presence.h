/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef HS_AML_PRESENCE_H
#define HS_AML_PRESENCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HS_AML_PRESENCE_CHAIN_COUNT ((unsigned)3)
#define HS_AML_PRESENCE_MAX_TEXT_BYTES ((size_t)2)

enum hs_aml_presence_state {
    HS_AML_PRESENCE_UNKNOWN = 0,
    HS_AML_PRESENCE_ABSENT = 1,
    HS_AML_PRESENCE_PRESENT = 2
};

bool hs_aml_presence_direction_is_input(
    const uint8_t *text, size_t length, bool read_succeeded);

bool hs_aml_presence_active_low_is_disabled(
    const uint8_t *text, size_t length, bool read_succeeded);

enum hs_aml_presence_state hs_aml_presence_decode(
    const uint8_t *text, size_t length, bool read_succeeded,
    bool input_direction_verified, bool active_high_verified);

bool hs_aml_presence_gpio_for_chain(unsigned chain_index, uint32_t *gpio);
bool hs_aml_presence_chain_for_gpio(uint32_t gpio, unsigned *chain_index);

#endif
