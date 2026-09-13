/* SPDX-License-Identifier: GPL-3.0-only */

#include <stddef.h>

void *memset(void *destination, int value, size_t length);
void *memset(void *destination, int value, size_t length)
{
    volatile unsigned char *bytes = destination;
    for (size_t index = 0; index < length; ++index) {
        bytes[index] = (unsigned char)value;
    }
    return destination;
}

void *memcpy(void *destination, const void *source, size_t length);
void *memcpy(void *destination, const void *source, size_t length)
{
    volatile unsigned char *dst = destination;
    const volatile unsigned char *src = source;
    for (size_t index = 0; index < length; ++index) dst[index] = src[index];
    return destination;
}
