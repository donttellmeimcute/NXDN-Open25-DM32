/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DM32_P25_RUNTIME_H
#define DM32_P25_RUNTIME_H

/* ISO C's freestanding environment supplies stddef/stdint, but need not
 * supply string.h. The firmware integration must supply these three ordinary
 * byte-memory functions. These declarations are not a replacement libc/sysroot.
 */
#if __STDC_HOSTED__
#include <string.h>
#else
#include <stddef.h>
extern void *memcpy(void *restrict destination, const void *restrict source, size_t length);
extern void *memmove(void *destination, const void *source, size_t length);
extern void *memset(void *destination, int value, size_t length);
#endif
#endif
