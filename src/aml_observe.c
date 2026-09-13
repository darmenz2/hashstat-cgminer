/* SPDX-License-Identifier: GPL-3.0-only */
#include "hs_aml_observe.h"

static bool text_valid(const struct hs_aml_text *text)
{
    return text->status == HS_AML_TEXT_OK && text->length != 0 &&
           text->length <= HS_AML_OBSERVE_TEXT_CAPACITY;
}
static bool equal(const struct hs_aml_text *a, const struct hs_aml_text *b)
{
    if (a->length != b->length) return false;
    for (size_t i = 0; i < a->length; ++i)
        if (a->bytes[i] != b->bytes[i]) return false;
    return true;
}
struct hs_aml_observation hs_aml_observe_presence(
    bool exact_profile_verified, hs_aml_attribute_reader reader, void *context)
{
    struct hs_aml_observation result = {0};
    result.bound_profile = exact_profile_verified;
    for (unsigned chain = 0; chain < 3U; ++chain) {
        struct hs_aml_chain_observation *observed = &result.chains[chain];
        (void)hs_aml_presence_gpio_for_chain(chain, &observed->gpio);
        if (!exact_profile_verified) continue;
        if (reader == NULL) { observed->reason = HS_AML_OBSERVATION_READ_ERROR; continue; }
        struct hs_aml_text first[3], second[3];
        for (unsigned field = 0; field < 3U; ++field) {
            first[field] = reader(context, chain, (enum hs_aml_attribute)field);
            ++result.read_calls;
        }
        for (unsigned field = 3U; field != 0; --field) {
            second[field - 1U] = reader(context, chain, (enum hs_aml_attribute)(field - 1U));
            ++result.read_calls;
        }
        observed->reason = HS_AML_OBSERVATION_VALID_PLUG_LEVEL;
        for (unsigned field = 0; field < 3U; ++field) {
            if (!text_valid(&first[field]) || !text_valid(&second[field])) {
                observed->reason = HS_AML_OBSERVATION_READ_ERROR; break;
            }
            if (!equal(&first[field], &second[field])) {
                observed->reason = HS_AML_OBSERVATION_CHANGED; break;
            }
        }
        if (observed->reason != HS_AML_OBSERVATION_VALID_PLUG_LEVEL) continue;
        const bool input = hs_aml_presence_direction_is_input(first[0].bytes, first[0].length, true);
        if (!input) { observed->reason = HS_AML_OBSERVATION_NOT_INPUT; continue; }
        const bool high = hs_aml_presence_active_low_is_disabled(first[1].bytes, first[1].length, true);
        if (!high) { observed->reason = HS_AML_OBSERVATION_POLARITY_UNVERIFIED; continue; }
        observed->plug = hs_aml_presence_decode(first[2].bytes, first[2].length, true, input, high);
        if (observed->plug == HS_AML_PRESENCE_UNKNOWN)
            observed->reason = HS_AML_OBSERVATION_BAD_VALUE;
    }
    return result;
}
