/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef P25_AUDIO_FIFO_H
#define P25_AUDIO_FIFO_H
#include <stdint.h>
enum { P25_AUDIO_BLOCKS=18, P25_AUDIO_PREFILL=9, P25_AUDIO_SAMPLES=160 };
/* Single producer/consumer with caller-provided exclusion. Fixed memory,
 * one block per 20 ms. Rebuffer after starvation; never repeat stale speech. */
typedef struct {
    int16_t pcm[P25_AUDIO_BLOCKS][P25_AUDIO_SAMPLES];
    uint8_t head,tail,count,playing,draining;
    uint32_t pushed,popped,underruns,overflows,high_water;
} p25_audio_fifo;
void p25_audio_fifo_reset(p25_audio_fifo *);
int p25_audio_fifo_push(p25_audio_fifo *,const int16_t pcm[160]);
int p25_audio_fifo_take(p25_audio_fifo *,int16_t pcm[160]);
int p25_audio_fifo_pop(p25_audio_fifo *,int16_t pcm[160]);
#endif
