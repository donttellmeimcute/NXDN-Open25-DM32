/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Actual native board glue; hardware callees are bounded host doubles. */
#include "config.h"
#include "hal_radio.h"
#include "hrc_native_port.h"
#include <assert.h>
#include <stdio.h>
static uint32_t timers[7],clock_us=1000,depth;
#define TMR_CTRL(n) timers[n]
#define TIMER_PA 3
#define DMR_PA_DELAY_US 28000
enum { PIN_SPK_SOURCE, PIN_MIC_PATH };
static radio_mode_t mode=RADIO_MODE_FM;
static bool transmitting,dmr_keyed,cal_valid,rx_ok=true;
static unsigned holds,tx_calls,safe_calls;
static unsigned mic_route_writes, speaker_restores;
static hrc7000_t modem;
uint32_t hal_micros(void) { return clock_us; }
uint32_t firstboot_irq_save(void) { return depth++; }
void firstboot_irq_restore(uint32_t n) { assert(depth==n+1);depth=n; }
static bool bf_cal_valid(void) { return cal_valid; }
static void hrc_audio_pcm_hold(bool on) { (void)on;holds++; }
static void audio(bool on) { assert(!on); }
static void hrc_pin_write(unsigned pin, bool on) { assert(pin<=PIN_MIC_PATH && !on);mic_route_writes++; }
static void speaker_source(void) { speaker_restores++; }
static void set_mode(radio_mode_t m) { mode=m; }
static void hrc_dmr_leave(void) { }
static bool rx(uint32_t hz,radio_bw_t bw,const subtone_t *tone)
{ assert(hz==145500000 && bw==RADIO_BW_12K5 && !tone);return rx_ok; }
static bool tx(uint32_t hz,radio_bw_t bw,uint8_t power,const subtone_t *tone)
{ assert(hz==145500000 && bw==RADIO_BW_12K5 && power==0 && !tone);assert(firstboot_tx_allowed(FB_BOARD_TX));tx_calls++;return true; }
static void board_rf_safe(void) { assert(!firstboot_tx_allowed(FB_PA_GPIO));safe_calls++; }
static void dmr_rf_switch(bool on) { assert(on && firstboot_tx_allowed(FB_RF_SWITCH)); }
static void hrc_timer_oneshot(unsigned n,uint32_t us) { assert(n==3 && us==28000); }
static void dmr_pa_on(void) { assert(firstboot_tx_allowed(FB_PA_KEY)); }
static uint32_t hrc_irq_save(void) { return firstboot_irq_save(); }
static void hrc_irq_restore(uint32_t n) { firstboot_irq_restore(n); }
void hn_init(hn_chip *chip,hrc7000_t *m,const hn_hooks *hooks,nt_ops *ops)
{ chip->modem=m;chip->hooks=hooks;(void)ops; }
int hn_port_on(hn_chip *chip,nt_test *t) { (void)chip;(void)t;return 1; }
void hn_port_quiet(void) { }
void hn_port_restore(void) { }
#include "dm32_native.inc"
int main(void)
{
    dm32_native_mic_route(true);dm32_native_mic_route(false);
    assert(mic_route_writes==2 && speaker_restores==1);
    native_init();nt_test t;nt_init(&t,NULL);
    nt_config c={.frequency_hz=145500000,.blocks=4,.duration_ms=1000,.lease_ms=250,.mode=NT_TRANSMIT,.p25_sync=1};
    assert(nt_arm(&t,&c,1));t.owned=1;t.started_us=t.ping_us=clock_us;native_chip.test=&t;
    cal_valid=false;assert(!native_prepare(NULL,&c));
    assert(!holds && !tx_calls && !firstboot_tx_allowed(FB_BOARD_TX));
    cal_valid=true;timers[6]=1;assert(!native_prepare(NULL,&c));timers[6]=0;
    assert(!holds && !firstboot_tx_allowed(FB_BOARD_TX));
    assert(native_prepare(NULL,&c) && tx_calls==1 && firstboot_tx_allowed(FB_BOARD_TX));
    native_off(NULL);assert(safe_calls==1 && !firstboot_tx_allowed(FB_BOARD_TX));
    assert(native_restore(NULL) && mode==RADIO_MODE_FM);
    rx_ok=false;assert(!native_prepare(NULL,&c));native_off(NULL);
    assert(!firstboot_tx_allowed(FB_BOARD_TX) && tx_calls==1);assert(native_restore(NULL));
    /* Receive remains available without calibration and cannot open a grant. */
    c.mode=NT_RECEIVE;cal_valid=false;rx_ok=true;assert(native_prepare(NULL,&c));
    assert(tx_calls==1 && !firstboot_tx_allowed(FB_BOARD_TX));native_off(NULL);assert(native_restore(NULL));
    assert(!depth);puts("Native board prepare: valid calibration required, timer conflicts denied, lowest power fixed, fast-off revokes, RX unchanged PASS");
}
