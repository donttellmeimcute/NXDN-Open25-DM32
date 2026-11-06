/* SPDX-License-Identifier: GPL-3.0-or-later
 * Small freestanding ISO C memory runtime. Build with -fno-builtin and
 * -fno-tree-loop-distribute-patterns to prevent compiler libc recursion.
 */
#include <stddef.h>
#include <stdint.h>

void *memcpy(void *restrict destination, const void *restrict source, size_t count)
{
    unsigned char *out = destination;
    const unsigned char *in = source;
    for (size_t i = 0; i < count; ++i) out[i] = in[i];
    return destination;
}

void *memmove(void *destination, const void *source, size_t count)
{
    unsigned char *out = destination;
    const unsigned char *in = source;
    if ((uintptr_t)out < (uintptr_t)in) {
        for (size_t i = 0; i < count; ++i) out[i] = in[i];
    } else {
        for (size_t i = count; i > 0; --i) out[i - 1] = in[i - 1];
    }
    return destination;
}

void *memset(void *destination, int value, size_t count)
{
    unsigned char *out = destination;
    for (size_t i = 0; i < count; ++i) out[i] = (unsigned char)value;
    return destination;
}
