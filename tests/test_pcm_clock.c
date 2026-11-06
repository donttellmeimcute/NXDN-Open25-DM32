/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pcm_clock.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    p25_pcm_clock c={0};unsigned count=0;
    /* A 2 ms service loop with jitter still supplies 8000 samples/second. */
    for(uint32_t t=0;t<1000000;t+=2000) {
        uint32_t now=t+(t%6000==2000?700:0);
        if(p25_pcm_clock_due(&c,now)) { p25_pcm_clock_sent(&c,now);count++;assert(!p25_pcm_clock_due(&c,now)); }
    }
    assert(count==250 && !c.late);
    assert(p25_pcm_clock_due(&c,1020000));p25_pcm_clock_sent(&c,1020000);
    assert(c.late==1 && !p25_pcm_clock_due(&c,1023999) && p25_pcm_clock_due(&c,1024000));
    /* A full FIFO causes no commit: the same chunk stays due. */
    assert(p25_pcm_clock_due(&c,1025000));assert(p25_pcm_clock_due(&c,1026000));
    c=(p25_pcm_clock){0};p25_pcm_clock_sent(&c,UINT32_MAX-1999);
    assert(!p25_pcm_clock_due(&c,1999) && p25_pcm_clock_due(&c,2000));
    p25_pcm_clock_sent(&c,2000);assert(c.next==6000 && !c.late);
    puts("PCM chunk clock: 8 kHz cadence, jitter, full-FIFO retry, no catch-up bursts, wrap PASS");
}
