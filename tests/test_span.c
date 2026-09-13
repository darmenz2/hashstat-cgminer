/* SPDX-License-Identifier: GPL-3.0-only */
#include "hs_span.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static void test_empty(void)
{
    unsigned char buffer[8];

    CHECK(hs_span_valid(NULL, 0));
    CHECK(hs_span_valid(buffer, 0));
    CHECK(!hs_span_valid(NULL, 1));
    CHECK(!hs_span_nonempty(NULL, 0));
    CHECK(!hs_span_nonempty(buffer, 0));
    CHECK(hs_span_nonempty(buffer, sizeof(buffer)));
    CHECK(!hs_span_overflows(NULL, 0));
    CHECK(!hs_spans_overlap(NULL, 0, buffer, sizeof(buffer)));
    CHECK(!hs_spans_overlap(buffer, sizeof(buffer), NULL, 0));
    CHECK(!hs_spans_overlap(buffer, 0, buffer, sizeof(buffer)));
    CHECK(!hs_spans_overlap(buffer, sizeof(buffer), buffer, 0));
}

static void test_address_limit(void)
{
    /* Synthetic addresses are used only for integer arithmetic, never accessed. */
    const void *last = (const void *)UINTPTR_MAX;
    const void *tail = (const void *)(UINTPTR_MAX - 7U);

    CHECK(hs_span_valid(last, 0));
    CHECK(hs_span_nonempty(last, 1));
    CHECK(!hs_span_overflows(last, 1));
    CHECK(hs_span_overflows(last, 2));
    CHECK(!hs_span_valid(last, 2));
    CHECK(hs_span_nonempty(tail, 8));
    CHECK(hs_span_overflows(tail, 9));
    CHECK(!hs_span_valid(tail, SIZE_MAX));
    CHECK(hs_spans_overlap(tail, 8, last, 1));
    CHECK(hs_spans_overlap(last, 1, tail, 8));
    CHECK(!hs_spans_overlap(tail, 7, last, 1));
    CHECK(!hs_spans_overlap(last, 1, tail, 7));
}

static void test_overlap(void)
{
    unsigned char buffer[24];

    for (size_t a = 0; a <= sizeof(buffer); ++a) {
        for (size_t b = 0; b <= sizeof(buffer); ++b) {
            for (size_t n = 0; n <= sizeof(buffer) - a; ++n) {
                for (size_t m = 0; m <= sizeof(buffer) - b; ++m) {
                    const bool expected = n != 0 && m != 0 && a < b + m && b < a + n;
                    CHECK(hs_spans_overlap(buffer + a, n, buffer + b, m) == expected);
                }
            }
        }
    }
}

int main(void)
{
    test_empty();
    test_address_limit();
    test_overlap();
    puts("span tests passed");
    return EXIT_SUCCESS;
}
