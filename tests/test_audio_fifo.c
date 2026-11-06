/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "audio_fifo.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    p25_audio_fifo q;p25_audio_fifo_reset(&q);int16_t in[160],out[160];
    for(unsigned i=0;i<160;i++)in[i]=(int16_t)(i-80);
    for(unsigned b=0;b<8;b++)assert(p25_audio_fifo_push(&q,in));
    assert(!p25_audio_fifo_pop(&q,out) && out[0]==0 && q.count==8);
    assert(p25_audio_fifo_push(&q,in));
    for(unsigned b=0;b<9;b++) {
        assert(p25_audio_fifo_pop(&q,out));
        for(unsigned i=0;i<160;i++)assert(out[i]==in[i]);
    }
    assert(!p25_audio_fifo_pop(&q,out) && q.underruns==1 && !q.playing);
    for(unsigned b=0;b<18;b++)assert(p25_audio_fifo_push(&q,in));
    assert(!p25_audio_fifo_push(&q,in) && q.overflows==1 && q.high_water==18);
    for(unsigned b=0;b<18;b++)assert(p25_audio_fifo_pop(&q,out));
    assert(p25_audio_fifo_push(&q,in));q.playing=0;q.draining=1;
    assert(p25_audio_fifo_pop(&q,out));assert(!p25_audio_fifo_pop(&q,out));
    assert(q.underruns==1 && !q.count);
    for(unsigned i=0;i<5000;i++) {
        assert(p25_audio_fifo_push(&q,in));assert(p25_audio_fifo_pop(&q,out));
        assert(out[159]==in[159]);
    }
    p25_audio_fifo_reset(&q);assert(!q.count && !q.pushed && !q.underruns);
    puts("audio FIFO: prefill, wrap, starvation silence/rebuffer, overflow, finite drain PASS");
}
