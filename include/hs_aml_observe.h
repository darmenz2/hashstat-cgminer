/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef HS_AML_OBSERVE_H
#define HS_AML_OBSERVE_H
#include "hs_aml_presence.h"

#define HS_AML_OBSERVE_TEXT_CAPACITY 4U
enum hs_aml_attribute { HS_AML_ATTRIBUTE_DIRECTION = 0, HS_AML_ATTRIBUTE_ACTIVE_LOW,
                        HS_AML_ATTRIBUTE_VALUE };
enum hs_aml_text_status { HS_AML_TEXT_UNAVAILABLE = 0, HS_AML_TEXT_OK,
                         HS_AML_TEXT_READ_ERROR, HS_AML_TEXT_TOO_LONG,
                         HS_AML_TEXT_UNSAFE_TYPE };
struct hs_aml_text {
    enum hs_aml_text_status status;
    size_t length;
    uint8_t bytes[HS_AML_OBSERVE_TEXT_CAPACITY];
};
enum hs_aml_observation_reason { HS_AML_OBSERVATION_NOT_BOUND = 0,
    HS_AML_OBSERVATION_READ_ERROR, HS_AML_OBSERVATION_CHANGED,
    HS_AML_OBSERVATION_NOT_INPUT, HS_AML_OBSERVATION_POLARITY_UNVERIFIED,
    HS_AML_OBSERVATION_BAD_VALUE, HS_AML_OBSERVATION_VALID_PLUG_LEVEL };
struct hs_aml_chain_observation {
    uint32_t gpio;
    enum hs_aml_presence_state plug;
    enum hs_aml_observation_reason reason;
};
struct hs_aml_observation {
    struct hs_aml_chain_observation chains[3];
    unsigned read_calls;
    bool bound_profile;
};

typedef struct hs_aml_text (*hs_aml_attribute_reader)(void *context,
    unsigned chain, enum hs_aml_attribute attribute);
struct hs_aml_observation hs_aml_observe_presence(
    bool exact_profile_verified, hs_aml_attribute_reader reader, void *context);
#endif
