/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef P25_PCM_CLOCK_H
#define P25_PCM_CLOCK_H
#include <stdint.h>
/* 32 samples at 8 kHz. Never burst to catch up after a missed deadline. */
typedef struct { uint32_t next, late; uint8_t armed; } p25_pcm_clock;
int p25_pcm_clock_due(const p25_pcm_clock *, uint32_t now);
void p25_pcm_clock_sent(p25_pcm_clock *, uint32_t now);
#endif
