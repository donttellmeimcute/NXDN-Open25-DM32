/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pcm_clock.h"
int p25_pcm_clock_due(const p25_pcm_clock *c,uint32_t now)
{ return !c->armed || (int32_t)(now-c->next)>=0; }
void p25_pcm_clock_sent(p25_pcm_clock *c,uint32_t now)
{
    if(!c->armed) { c->armed=1;c->next=now+4000u;return; }
    if(now-c->next>=4000u) { c->late++;c->next=now+4000u; }
    else c->next+=4000u;
}
