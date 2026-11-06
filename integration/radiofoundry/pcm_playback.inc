/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Included in the derivative audio driver; no RF or arbitrary register access. */
#if CONFIG_P25
#include "pcm_clock.h"
static p25_pcm_clock play_clock;
static bool play_paced;
static uint32_t play_diag[16];
static void playback_pace(bool on)
{
    uint32_t s=hrc_irq_save();
    memset(&play_clock,0,sizeof play_clock);
    memset(play_diag,0,sizeof play_diag);
    play_paced=on;play_diag[0]=1;play_diag[1]=on;play_diag[5]=UINT32_MAX;
    hrc_irq_restore(s);
}
static void playback_stats(uint32_t out[16])
{
    uint32_t s=hrc_irq_save();
    play_diag[4]=play_clock.late;
    unsigned count=(spk_head+SPK_BLOCKS-spk_tail)%SPK_BLOCKS;
    play_diag[10]=count*AUDIO_BLOCK_SAMPLES-(count?spk_pos:0);
    play_diag[13]=(uint32_t)running|((uint32_t)spk_routed<<1)|((uint32_t)fifo_held<<2)|((uint32_t)tx_gated<<3);
    play_diag[14]=modem?hrc7000_rd(modem,C7_AUDIO_CONTROL):0;
    play_diag[15]=blocks;
    memcpy(out,play_diag,sizeof play_diag);
    hrc_irq_restore(s);
}
static bool playback_push(const int16_t *pcm)
{
    uint32_t now=hrc_micros();
    if(play_paced && !p25_pcm_clock_due(&play_clock,now))return false;
    if(!hrc7000_prompt_push(modem,pcm)) { play_diag[3]++;return false; }
    if(play_diag[2]) {
        uint32_t dt=now-play_diag[12];
        if(dt<play_diag[5])play_diag[5]=dt;
        if(dt>play_diag[6])play_diag[6]=dt;
        if(dt<500u)play_diag[7]++;
    } else play_diag[11]=now;
    play_diag[12]=now;play_diag[2]++;
    if(play_paced)p25_pcm_clock_sent(&play_clock,now);
    return true;
}
#else
#define playback_push(pcm) hrc7000_prompt_push(modem,pcm)
#endif
