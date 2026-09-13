/* SPDX-License-Identifier: GPL-3.0-only */
#include "hs_aml_presence.h"
#include "hs_span.h"

bool hs_aml_presence_direction_is_input(
    const uint8_t *text, size_t length, bool read_succeeded)
{
    if (!read_succeeded || text == NULL || (length != 2U && length != 3U)) return false;
    if (hs_span_overflows(text, length)) return false;
    if (text[0] != UINT8_C(105) || text[1] != UINT8_C(110)) return false;
    return length == 2U || text[2] == UINT8_C(10);
}

enum hs_aml_presence_state hs_aml_presence_decode(
    const uint8_t *text, size_t length, bool read_succeeded,
    bool input_direction_verified, bool active_high_verified)
{
    if (!read_succeeded || !input_direction_verified || !active_high_verified || text == NULL ||
        length == 0U || length > HS_AML_PRESENCE_MAX_TEXT_BYTES) {
        return HS_AML_PRESENCE_UNKNOWN;
    }
    if (hs_span_overflows(text, length)) {
        return HS_AML_PRESENCE_UNKNOWN;
    }
    if (length == 2U && text[1] != UINT8_C(10)) return HS_AML_PRESENCE_UNKNOWN;
    if (text[0] == UINT8_C(48)) return HS_AML_PRESENCE_ABSENT;
    if (text[0] == UINT8_C(49)) return HS_AML_PRESENCE_PRESENT;
    return HS_AML_PRESENCE_UNKNOWN;
}

bool hs_aml_presence_active_low_is_disabled(
    const uint8_t *text, size_t length, bool read_succeeded)
{
    return hs_aml_presence_decode(text, length, read_succeeded, true, true) ==
           HS_AML_PRESENCE_ABSENT;
}

bool hs_aml_presence_gpio_for_chain(unsigned chain_index, uint32_t *gpio)
{
    static const uint32_t known_gpio[HS_AML_PRESENCE_CHAIN_COUNT] = {
        UINT32_C(439), UINT32_C(440), UINT32_C(441)
    };
    if (chain_index >= HS_AML_PRESENCE_CHAIN_COUNT || gpio == NULL) return false;
    if (hs_span_overflows(gpio, sizeof(*gpio))) return false;
    *gpio = known_gpio[chain_index];
    return true;
}

bool hs_aml_presence_chain_for_gpio(uint32_t gpio, unsigned *chain_index)
{
    if (gpio < UINT32_C(439) || gpio > UINT32_C(441) || chain_index == NULL) return false;
    if (hs_span_overflows(chain_index, sizeof(*chain_index))) return false;
    *chain_index = (unsigned)(gpio - UINT32_C(439));
    return true;
}
