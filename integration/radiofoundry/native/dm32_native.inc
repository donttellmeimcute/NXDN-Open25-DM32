/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Included only in the separate derivative's board radio translation unit. */
#if CONFIG_P25_NATIVE
#include "hrc_native_port.h"
static hn_chip native_chip;
nt_ops dm32_native_ops;
void dm32_native_mic_route(bool on)
{
    if (on) {
        audio(false);
        hrc_pin_write(PIN_SPK_SOURCE,false);
        hrc_pin_write(PIN_MIC_PATH,false);
    } else speaker_source();
}
static int native_prepare(void *ctx, const nt_config *c)
{
    (void)ctx;
    if (mode!=RADIO_MODE_FM || transmitting || (TMR_CTRL(6)&1u) || (TMR_CTRL(TIMER_PA)&1u)) return 0;
    /* TX needs the factory calibration: the board's power ramp reads it. */
    if (c->mode==NT_TRANSMIT && !bf_cal_valid()) return 0;
#if CONFIG_DEV_TX
    if(c->mode==NT_TRANSMIT && !firstboot_native_tx_open(native_chip.test))return 0;
#endif
    hrc_audio_pcm_hold(true);
    audio(false);
    set_mode(RADIO_MODE_DMR);
    hrc_dmr_leave(); /* dedicated PHY handlers replace the normal slot engine */
    if (!rx(c->frequency_hz,RADIO_BW_12K5,NULL)) return 0;
    /* Audio sessions may feed the speaker FIFO; raw PHY bench sessions hold it. */
    if (c->mode==NT_RECEIVE && c->audio) hrc_audio_pcm_hold(false);
    /* The board's own TX entry: band limits and the calibrated power level. */
    return c->mode!=NT_TRANSMIT || tx(c->frequency_hz,RADIO_BW_12K5,c->power,NULL);
}
static void native_off(void *ctx) {
    (void)ctx;
#if CONFIG_FIRSTBOOT
    firstboot_native_tx_close();
#endif
    board_rf_safe();
}
static void native_key(void *ctx)
{
    (void)ctx;
    dmr_rf_switch(true);
    hrc_timer_oneshot(TIMER_PA,DMR_PA_DELAY_US);
}
static void native_pa(void *ctx) { (void)ctx; dmr_pa_on(); }
static int native_irq_on(void *ctx, nt_test *t) { (void)ctx; return hn_port_on(&native_chip,t); }
static void native_irq_quiet(void *ctx) { (void)ctx; hn_port_quiet(); }
static void native_irq_restore(void *ctx) { (void)ctx; hn_port_restore(); }
static int native_restore(void *ctx)
{
    (void)ctx;
    dmr_keyed=false;
    set_mode(RADIO_MODE_FM);
    transmitting=false;
    hrc_audio_pcm_hold(false);
    /* The radio task retunes its untouched channel after releasing ownership. */
    return 1;
}
static uint32_t native_lock(void *ctx) { (void)ctx; return hrc_irq_save(); }
static void native_unlock(void *ctx, uint32_t irq) { (void)ctx; hrc_irq_restore(irq); }
static const hn_hooks native_hooks={NULL,native_prepare,native_off,native_key,native_pa,
    native_irq_on,native_irq_quiet,native_irq_restore,native_restore,native_lock,native_unlock};
static void native_init(void) { hn_init(&native_chip,&modem,&native_hooks,&dm32_native_ops); }
#endif
