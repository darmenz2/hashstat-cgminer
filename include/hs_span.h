/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef HS_SPAN_H
#define HS_SPAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Address arithmetic only; callers must establish object bounds and access. */
static inline bool hs_span_overflows(const void *pointer, size_t length)
{
    return length != 0U && (uintmax_t)(length - 1U) >
           (uintmax_t)(UINTPTR_MAX - (uintptr_t)pointer);
}

/* Empty spans are valid, including {NULL, 0}. */
static inline bool hs_span_valid(const void *pointer, size_t length)
{
    return length == 0U || (pointer != NULL && !hs_span_overflows(pointer, length));
}

/* For required objects and nonempty buffers. */
static inline bool hs_span_nonempty(const void *pointer, size_t length)
{
    return length != 0U && pointer != NULL && !hs_span_overflows(pointer, length);
}

/* Call after validating both spans. Empty spans never overlap. */
static inline bool hs_spans_overlap(const void *first, size_t first_size,
                                    const void *second, size_t second_size)
{
    const uintptr_t a = (uintptr_t)first;
    const uintptr_t b = (uintptr_t)second;

    if (first_size == 0U || second_size == 0U)
        return false;
    return a <= b ? (uintmax_t)(b - a) < (uintmax_t)first_size :
                    (uintmax_t)(a - b) < (uintmax_t)second_size;
}

#endif
