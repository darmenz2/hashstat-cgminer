/* SPDX-License-Identifier: GPL-3.0-only */
#include "hs_aml_observe.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(c) do { assert(c); ++checks; } while (0)
struct fixture { unsigned calls; struct hs_aml_text texts[18]; };
static struct hs_aml_text read_fixture(void *opaque, unsigned chain, enum hs_aml_attribute attr)
{
    struct fixture *f = opaque;
    static const unsigned fields[6] = {0,1,2,2,1,0};
    CHECK(f->calls < 18U);
    CHECK(chain == f->calls / 6U);
    CHECK((unsigned)attr == fields[f->calls % 6U]);
    return f->texts[f->calls++];
}
static struct fixture good(void)
{
    struct fixture result = {0};
    static const char *const texts[6] = {"in\n", "0\n", "1\n", "1\n", "0\n", "in\n"};
    for (unsigned i = 0; i < 18U; ++i) {
        struct hs_aml_text *t = &result.texts[i];
        t->status = HS_AML_TEXT_OK; t->length = strlen(texts[i % 6U]);
        memcpy(t->bytes, texts[i % 6U], t->length);
    }
    return result;
}
static void unknown_chain(const struct hs_aml_observation *result, unsigned chain)
{
    CHECK(result->chains[chain].plug == HS_AML_PRESENCE_UNKNOWN);
    CHECK(result->chains[chain].reason != HS_AML_OBSERVATION_VALID_PLUG_LEVEL);
}
int main(void)
{
    struct fixture fixture = good();
    struct hs_aml_observation r = hs_aml_observe_presence(false, read_fixture, &fixture);
    CHECK(r.read_calls == 0 && fixture.calls == 0 && !r.bound_profile);
    for (unsigned i = 0; i < 3U; ++i) unknown_chain(&r, i);
    r = hs_aml_observe_presence(true, NULL, NULL);
    CHECK(r.read_calls == 0);
    for (unsigned i = 0; i < 3U; ++i) unknown_chain(&r, i);
    r = hs_aml_observe_presence(true, read_fixture, &fixture);
    CHECK(r.read_calls == 18U && fixture.calls == 18U && r.bound_profile);
    for (unsigned i = 0; i < 3U; ++i) {
        CHECK(r.chains[i].gpio == 439U + i);
        CHECK(r.chains[i].plug == HS_AML_PRESENCE_PRESENT);
        CHECK(r.chains[i].reason == HS_AML_OBSERVATION_VALID_PLUG_LEVEL);
    }
    for (unsigned damaged = 0; damaged < 18U; ++damaged) {
        for (unsigned length = 0; length <= 5U; ++length) {
            fixture = good(); fixture.texts[damaged].length = length;
            r = hs_aml_observe_presence(true, read_fixture, &fixture);
            if (length != good().texts[damaged].length) unknown_chain(&r, damaged / 6U);
            CHECK(r.read_calls == 18U);
        }
        for (unsigned status = 0; status < 8U; ++status) {
            if (status == HS_AML_TEXT_OK) continue;
            fixture = good(); fixture.texts[damaged].status = (enum hs_aml_text_status)status;
            r = hs_aml_observe_presence(true, read_fixture, &fixture);
            unknown_chain(&r, damaged / 6U);
            CHECK(r.chains[damaged / 6U].reason == HS_AML_OBSERVATION_READ_ERROR);
        }
        for (unsigned byte = 0; byte < 256U; ++byte) {
            fixture = good(); fixture.texts[damaged].bytes[0] = (uint8_t)byte;
            r = hs_aml_observe_presence(true, read_fixture, &fixture);
            if (byte != good().texts[damaged].bytes[0]) unknown_chain(&r, damaged / 6U);
            CHECK(r.read_calls == 18U);
        }
    }
    fixture = good(); fixture.texts[8].bytes[0] = fixture.texts[9].bytes[0] = '0';
    r = hs_aml_observe_presence(true, read_fixture, &fixture);
    CHECK(r.chains[1].plug == HS_AML_PRESENCE_ABSENT);
    CHECK(r.chains[0].plug == HS_AML_PRESENCE_PRESENT && r.chains[2].plug == HS_AML_PRESENCE_PRESENT);
    fixture = good();
    fixture.texts[7].bytes[0] = fixture.texts[10].bytes[0] = '1';
    r = hs_aml_observe_presence(true, read_fixture, &fixture);
    unknown_chain(&r, 1);
    CHECK(r.chains[1].reason == HS_AML_OBSERVATION_POLARITY_UNVERIFIED);
    fixture = good();
    memcpy(fixture.texts[6].bytes, "out", 3); memcpy(fixture.texts[11].bytes, "out", 3);
    r = hs_aml_observe_presence(true, read_fixture, &fixture);
    unknown_chain(&r, 1);
    CHECK(r.chains[1].reason == HS_AML_OBSERVATION_NOT_INPUT);
    fixture = good(); fixture.texts[2].bytes[0] = fixture.texts[3].bytes[0] = 'x';
    r = hs_aml_observe_presence(true, read_fixture, &fixture);
    unknown_chain(&r, 0);
    CHECK(r.chains[0].reason == HS_AML_OBSERVATION_BAD_VALUE);
    for (unsigned i = 0; i < 18U; ++i) {
        fixture = good(); fixture.texts[i].length = SIZE_MAX;
        r = hs_aml_observe_presence(true, read_fixture, &fixture);
        unknown_chain(&r, i / 6U);
        CHECK(r.chains[i / 6U].reason == HS_AML_OBSERVATION_READ_ERROR);
    }
    fixture = good();
    for (unsigned i = 0; i < 18U; ++i) fixture.texts[i].status = HS_AML_TEXT_UNAVAILABLE;
    r = hs_aml_observe_presence(true, read_fixture, &fixture);
    CHECK(r.read_calls == 18U);
    for (unsigned i = 0; i < 3U; ++i) unknown_chain(&r, i);
    fixture = good();
    r = hs_aml_observe_presence(true, read_fixture, &fixture);
    for (unsigned i = 0; i < 3U; ++i) CHECK(r.chains[i].plug == HS_AML_PRESENCE_PRESENT);
    r = hs_aml_observe_presence(false, NULL, NULL);
    for (unsigned i = 0; i < 3U; ++i) unknown_chain(&r, i);
    printf("AML bounded presence collector: %u checks PASS; fake read callbacks only\n", checks);
    return 0;
}
