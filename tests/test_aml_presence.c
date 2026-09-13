/* SPDX-License-Identifier: GPL-3.0-only */
#include "hs_aml_presence.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    uint8_t text[8] = {0}, before[8];
    unsigned first, second, chain, flags;
    uint32_t gpio;
    unsigned long checks = 0UL;
    size_t length, offset;
    enum hs_aml_presence_state expected;

    assert(HS_AML_PRESENCE_UNKNOWN == 0);
    for (first = 0U; first < 256U; ++first) {
        for (second = 0U; second < 256U; ++second) {
            text[0] = (uint8_t)first;
            text[1] = (uint8_t)second;
            memcpy(before, text, sizeof(before));
            assert(hs_aml_presence_direction_is_input(text, 2U, true) ==
                   (first == 105U && second == 110U));
            assert(memcmp(text, before, sizeof(text)) == 0);
            ++checks;
        }
    }
    text[0] = UINT8_C(105);
    text[1] = UINT8_C(110);
    for (second = 0U; second < 256U; ++second) {
        text[2] = (uint8_t)second;
        assert(hs_aml_presence_direction_is_input(text, 3U, true) == (second == 10U));
        ++checks;
    }
    text[2] = UINT8_C(10);
    assert(!hs_aml_presence_direction_is_input(text, 2U, false));
    assert(!hs_aml_presence_direction_is_input(text, 3U, false));
    assert(!hs_aml_presence_direction_is_input(NULL, 2U, true));
    assert(!hs_aml_presence_direction_is_input(text, 0U, true));
    assert(!hs_aml_presence_direction_is_input(text, 1U, true));
    assert(!hs_aml_presence_direction_is_input(text, SIZE_MAX, true));
    for (length = 4U; length <= sizeof(text); ++length) {
        assert(!hs_aml_presence_direction_is_input(text, length, true));
        ++checks;
    }
    assert(!hs_aml_presence_direction_is_input((const uint8_t *)UINTPTR_MAX, 2U, true));
    assert(!hs_aml_presence_direction_is_input((const uint8_t *)(UINTPTR_MAX - 1U), 3U, true));
    checks += 8UL;
    for (first = 0U; first < 256U; ++first) {
        memset(text, 0x9a, sizeof(text));
        text[2] = (uint8_t)first;
        expected = first == 48U ? HS_AML_PRESENCE_ABSENT :
                   first == 49U ? HS_AML_PRESENCE_PRESENT : HS_AML_PRESENCE_UNKNOWN;
        memcpy(before, text, sizeof(before));
        assert(hs_aml_presence_decode(text + 2U, 1U, true, true, true) == expected);
        assert(hs_aml_presence_active_low_is_disabled(text + 2U, 1U, true) == (first == 48U));
        assert(memcmp(text, before, sizeof(text)) == 0);
        checks += 2UL;
        for (second = 0U; second < 256U; ++second) {
            text[3] = (uint8_t)second;
            memcpy(before, text, sizeof(before));
            assert(hs_aml_presence_decode(text + 2U, 2U, true, true, true) ==
                   (second == 10U ? expected : HS_AML_PRESENCE_UNKNOWN));
            assert(hs_aml_presence_active_low_is_disabled(text + 2U, 2U, true) ==
                   (first == 48U && second == 10U));
            assert(memcmp(text, before, sizeof(text)) == 0);
            checks += 2UL;
        }
    }

    text[0] = UINT8_C(49);
    text[1] = UINT8_C(10);
    for (flags = 0U; flags < 7U; ++flags) {
        bool read_ok = (flags & 1U) != 0U;
        bool input_ok = (flags & 2U) != 0U;
        bool polarity_ok = (flags & 4U) != 0U;
        assert(hs_aml_presence_decode(text, 1U, read_ok, input_ok, polarity_ok) == HS_AML_PRESENCE_UNKNOWN);
        assert(hs_aml_presence_decode(text, 2U, read_ok, input_ok, polarity_ok) == HS_AML_PRESENCE_UNKNOWN);
        checks += 2UL;
    }
    assert(hs_aml_presence_decode(NULL, 1U, true, true, true) == HS_AML_PRESENCE_UNKNOWN);
    assert(hs_aml_presence_decode(text, 0U, true, true, true) == HS_AML_PRESENCE_UNKNOWN);
    assert(hs_aml_presence_decode(text, SIZE_MAX, true, true, true) == HS_AML_PRESENCE_UNKNOWN);
    for (length = 3U; length <= sizeof(text); ++length) {
        assert(hs_aml_presence_decode(text, length, true, true, true) == HS_AML_PRESENCE_UNKNOWN);
        ++checks;
    }
    assert(hs_aml_presence_decode((const uint8_t *)UINTPTR_MAX, 2U, true, true, true) ==
           HS_AML_PRESENCE_UNKNOWN);
    checks += 4UL;

    {
        static const uint8_t value[] = {'1', '\n'};
        static const uint8_t direction[][4] = {
            {'i','n','\n',0}, {'o','u','t',0}, {'i','n',0,0}, {'i','n','\r',0}
        };
        for (offset = 0U; offset < 4U; ++offset) {
            bool input = hs_aml_presence_direction_is_input(direction[offset], 3U, true);
            assert(hs_aml_presence_decode(value, 2U, true, input, true) ==
                   (offset == 0U ? HS_AML_PRESENCE_PRESENT : HS_AML_PRESENCE_UNKNOWN));
            ++checks;
        }
    }
    {
        static const uint8_t value[] = {'1', '\n'};
        static const uint8_t inversion[][3] = {{'0','\n',0}, {'1','\n',0}, {'0',0,0}, {'0',' ',0}};
        for (offset = 0U; offset < 4U; ++offset) {
            bool active_high = hs_aml_presence_active_low_is_disabled(inversion[offset], 2U, true);
            assert(hs_aml_presence_decode(value, 2U, true, true, active_high) ==
                   (offset == 0U ? HS_AML_PRESENCE_PRESENT : HS_AML_PRESENCE_UNKNOWN));
            ++checks;
        }
        assert(!hs_aml_presence_active_low_is_disabled(inversion[0], 2U, false));
        assert(!hs_aml_presence_active_low_is_disabled(NULL, 2U, true));
        assert(!hs_aml_presence_active_low_is_disabled(value, 0U, true));
        assert(!hs_aml_presence_active_low_is_disabled(value, SIZE_MAX, true));
        assert(!hs_aml_presence_active_low_is_disabled((const uint8_t *)UINTPTR_MAX, 2U, true));
        checks += 5UL;
    }

    for (chain = 0U; chain < 1024U; ++chain) {
        gpio = UINT32_C(0xaaaaaaaa);
        if (chain < 3U) {
            assert(hs_aml_presence_gpio_for_chain(chain, &gpio));
            assert(gpio == UINT32_C(439) + (uint32_t)chain);
        } else {
            assert(!hs_aml_presence_gpio_for_chain(chain, &gpio));
            assert(gpio == UINT32_C(0xaaaaaaaa));
        }
        ++checks;
    }
    for (gpio = UINT32_C(0); gpio < UINT32_C(1024); ++gpio) {
        chain = UINT_MAX;
        if (gpio >= UINT32_C(439) && gpio <= UINT32_C(441)) {
            assert(hs_aml_presence_chain_for_gpio(gpio, &chain));
            assert(chain == (unsigned)(gpio - UINT32_C(439)));
        } else {
            assert(!hs_aml_presence_chain_for_gpio(gpio, &chain));
            assert(chain == UINT_MAX);
        }
        ++checks;
    }
    gpio = 123U;
    chain = 456U;
    assert(!hs_aml_presence_gpio_for_chain(UINT_MAX, &gpio) && gpio == 123U);
    assert(!hs_aml_presence_chain_for_gpio(UINT32_MAX, &chain) && chain == 456U);
    assert(!hs_aml_presence_gpio_for_chain(0U, NULL));
    assert(!hs_aml_presence_chain_for_gpio(439U, NULL));
    checks += 4UL;
    for (offset = 0U; offset + 1U < sizeof(gpio); ++offset) {
        assert(!hs_aml_presence_gpio_for_chain(0U, (uint32_t *)(UINTPTR_MAX - offset)));
        ++checks;
    }
    for (offset = 0U; offset + 1U < sizeof(chain); ++offset) {
        assert(!hs_aml_presence_chain_for_gpio(439U, (unsigned *)(UINTPTR_MAX - offset)));
        ++checks;
    }
    printf("AML pure presence decoder/map: %lu cases passed; no files/devices opened\n", checks);
    return 0;
}
