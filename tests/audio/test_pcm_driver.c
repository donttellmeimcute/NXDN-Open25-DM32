/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Real generated driver and prompt writer, with RAM-backed hardware registers. */
#define PLATFORM_HRC7000_H
#define HRC_DMR_GLUE_H
#define pdFALSE 0
#define portYIELD_FROM_ISR(x) ((void)(x))
#define IRQ_PCM_WR 28
#include <assert.h>
#include <stdio.h>
#include "modem/hrc7000/hrc7000.h"
#include "FreeRTOS.h"
static uint32_t now_us;
static uint32_t hrc_micros(void) { return now_us; }
static uint32_t hrc_irq_save(void) { return 0; }
static void hrc_irq_restore(uint32_t s) { (void)s; }
static void hrc_irq_enable(unsigned irq,bool on) { (void)irq;(void)on; }
static void hrc_irq_register(unsigned irq,void (*fn)(void)) { (void)irq;(void)fn; }
static void hrc_barrier(void) { }
static bool hrc_dmr_tx_active(void) { return false; }
void hrc7000_set_crit(hrc7000_t *d,uint32_t (*a)(void),void (*b)(uint32_t)) { d->crit_enter=a;d->crit_exit=b; }
void hrc7000_audio_bits(hrc7000_t *d,uint32_t set,uint32_t clear) { d->audio_ctl=(d->audio_ctl&~clear)|set; }
void vTaskNotifyGiveFromISR(TaskHandle_t t,BaseType_t *w) { (void)t;(void)w; }
BaseType_t xTaskCreate(void (*fn)(void *),const char *n,unsigned s,void *c,unsigned p,TaskHandle_t *h)
{ (void)fn;(void)n;(void)s;(void)c;(void)p;*h=(void *)1;return pdPASS; }
uint32_t ulTaskNotifyTake(BaseType_t c,TickType_t t) { (void)c;(void)t;return 0; }
void xTaskNotifyGive(TaskHandle_t t) { (void)t; }
#include "../../build/radiofoundry-p25/platform/hrc7000/audio_pcm.c"
static void enqueue(unsigned serial)
{
    uint8_t next=(uint8_t)((spk_head+1u)%SPK_BLOCKS);assert(next!=spk_tail);
    for(unsigned i=0;i<160;i++)spk_q[spk_head][i]=(int16_t)(serial*160+i);
    spk_loud[spk_head]=true;spk_head=next;
}
int main(void)
{
    uint32_t regs[1024]={0},diag[16];uint8_t buf[4096]={0};
    hrc7000_t chip={.regs=regs,.buf=buf};hrc_audio_pcm_attach(&chip,NULL);
    speaker_pcm(true);playback_pace(true);
    for(now_us=0;now_us<1000000;now_us+=2000) {
        if(now_us%20000==0)enqueue(now_us/20000);
        fifo_feed();fifo_feed(); /* before/after callback cannot burst */
    }
    playback_stats(diag);
    assert(diag[2]==250 && diag[5]==4000 && diag[6]==4000 && !diag[7] && !diag[10]);
    assert(!play_clock.late);
    enqueue(50);regs[C7_AUDIO_CONTROL/4]=C7_AUDIO_RING_FULL;
    unsigned pos=spk_pos;fifo_feed();assert(spk_pos==pos && play_diag[3]==1);
    regs[C7_AUDIO_CONTROL/4]=0;fifo_feed();assert(spk_pos==32);
    /* Exact little-endian samples, with no skipped chunk on a full refusal. */
    assert(buf[C7_PROMPT_FIFO]==(8000&255) && buf[C7_PROMPT_FIFO+1]==(8000>>8));
    fifo_held=true;now_us+=4000;fifo_feed();assert(spk_pos==32);fifo_held=false;
    tx_gated=true;fifo_feed();assert(spk_pos==32);tx_gated=false;
    fifo_feed();assert(spk_pos==64);
    now_us+=40000;fifo_feed();assert(play_clock.late==1);
    unsigned n=play_diag[2];fifo_feed();assert(play_diag[2]==n);
    spk_drop();playback_pace(false);enqueue(0);enqueue(1);fifo_feed();
    playback_stats(diag);assert(diag[2]==10 && diag[7]==9 && !diag[10]);
    puts("Real PCM driver: continuous 8 kHz, full retry/sample order, hold/TX gates, missed deadline, legacy A/B PASS");
}
