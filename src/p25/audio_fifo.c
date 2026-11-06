/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "audio_fifo.h"
#include "p25_runtime.h"
void p25_audio_fifo_reset(p25_audio_fifo *q) { memset(q,0,sizeof *q); }
int p25_audio_fifo_push(p25_audio_fifo *q,const int16_t pcm[160])
{
    if (q->count==P25_AUDIO_BLOCKS) { q->overflows++;return 0; }
    memcpy(q->pcm[q->head],pcm,320);q->head=(uint8_t)((q->head+1)%P25_AUDIO_BLOCKS);
    q->count++;q->pushed++;if(q->count>q->high_water)q->high_water=q->count;
    return 1;
}
int p25_audio_fifo_take(p25_audio_fifo *q,int16_t pcm[160])
{
    if (!q->count)return 0;
    memcpy(pcm,q->pcm[q->tail],320);q->tail=(uint8_t)((q->tail+1)%P25_AUDIO_BLOCKS);
    q->count--;q->popped++;return 1;
}
int p25_audio_fifo_pop(p25_audio_fifo *q,int16_t pcm[160])
{
    memset(pcm,0,320);
    if (!q->playing && (q->count>=P25_AUDIO_PREFILL || (q->draining && q->count)))q->playing=1;
    if (!q->playing)return 0;
    if (!q->count) {
        if(!q->draining)q->underruns++;
        q->playing=0;return 0;
    }
    return p25_audio_fifo_take(q,pcm);
}
