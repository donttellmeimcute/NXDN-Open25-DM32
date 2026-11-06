/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Reuse actual GPIO, DAC, flash/NOR, FD6818 and IRQ fixtures. */
#define main guard_fixture_main
#include "test_guards.c"
#undef main
static uint32_t clock_us;
uint32_t hal_micros(void) { return clock_us; }
static fd6818_t radio_fd;
static unsigned keys,pa_calls,quiets;
static int begin_tx(void *ctx,nt_test *t)
{
    (void)ctx;assert(t->owned && t->state==NT_ARMED);
    if(!firstboot_native_tx_open(t))return 0;
    hrc_dac_b_power(true);assert(!(dac_pd&DAC_B_OFF));return 1;
}
static void off_tx(void *ctx)
{
    (void)ctx;firstboot_native_tx_close();quiets++;
    hrc_pin_write(PIN_VHF_PA,false);dac_b=0;hrc_dac_b_power(false);
}
static int restore_tx(void *ctx) { (void)ctx;return 1; }
static int write_tx(void *ctx,const uint8_t data[36]) { (void)ctx;(void)data;return 1; }
static void key_tx(void *ctx)
{
    (void)ctx;assert(firstboot_tx_allowed(FB_RF_SWITCH));keys++;
    fd6818_set_state(&radio_fd,FD6818_STATE_TX);
}
static void pa_tx(void *ctx)
{
    (void)ctx;assert(firstboot_tx_allowed(FB_PA_KEY));pa_calls++;
    hrc_pin_write(PIN_VHF_PA,true);dac_b=100;
}
static void cancel_tx(void *ctx) { (void)ctx; }
static const nt_ops tx_ops={.start=begin_tx,.write=write_tx,.cancel_send=cancel_tx,
    .request_key=key_tx,.pa_on=pa_tx,.quiet=off_tx,.restore=restore_tx};
static nt_test test;
static nt_config config={.frequency_hz=145500000,.blocks=3,.duration_ms=500,
    .lease_ms=100,.mode=NT_TRANSMIT,.p25_sync=1};
static void start_tx(uint32_t now)
{
    clock_us=now;nt_init(&test,&tx_ops);assert(nt_arm(&test,&config,1));
    uint8_t raw[36]={0x55};
    for(unsigned i=0;i<config.blocks;i++)assert(nt_load(&test,test.session,i,raw));
    assert(!firstboot_tx_allowed(FB_BOARD_TX));
    assert(nt_begin(&test,test.session,now));
}
static bool pa_high(void) { return !!(REG32(hrc_gpio_bank(PIN_VHF_PA)+GPIO_DR)&hrc_gpio_bit(PIN_VHF_PA)); }
int main(void)
{
    assert(FIRSTBOOT_FLAGS==47);
    nt_config bad=config;
    assert(firstboot_native_tx_config(&config));
    bad.frequency_hz++;assert(!firstboot_native_tx_config(&bad));bad=config;
    bad.lease_ms=251;assert(!firstboot_native_tx_config(&bad));bad=config;
    bad.duration_ms=2001;assert(!firstboot_native_tx_config(&bad));bad=config;
    bad.duration_ms=239;assert(!firstboot_native_tx_config(&bad));bad=config;
    bad.p25_sync=0;assert(!firstboot_native_tx_config(&bad));bad=config;
    bad.audio=1;assert(!firstboot_native_tx_config(&bad));
    nt_init(&test,&tx_ops);assert(!nt_arm(&test,&config,0));
    assert(!firstboot_native_tx_open(&test));
    hrc_modem_analog_init(1762);assert(dac_pd&DAC_B_OFF);
    hrc_dac_b_power(true);assert(dac_pd&DAC_B_OFF);
    hrc_pin_write(PIN_VHF_PA,true);assert(!pa_high());
    rfbus_t bus={.write=rf_write,.read=rf_read};fd6818_attach(&radio_fd,&bus);
    fd6818_init(&radio_fd);
    fd6818_set_state(&radio_fd,FD6818_STATE_TX);assert(!fd_tx_frames);
    start_tx(1000);
    assert(firstboot_tx_allowed(FB_BOARD_TX) && !firstboot_tx_allowed(FB_RF_SWITCH));
    assert(!firstboot_tx_allowed(FB_PA_KEY) && !firstboot_tx_allowed(FB_PA_GPIO));
    nt_pa_ready(&test,clock_us);assert(!pa_calls && !pa_high());
    nt_rf_tx(&test,clock_us);assert(!keys);
    nt_boundary(&test,NT_IRQ_PHY,clock_us);
    clock_us++;nt_rf_tx(&test,clock_us);assert(keys==1 && fd_tx_frames==1);
    assert(!firstboot_tx_allowed(FB_PA_GPIO));
    clock_us=29001;nt_pa_ready(&test,clock_us);assert(pa_calls==1 && pa_high());
    /* Gate independently rejects expired time even before the 1-ms IRQ runs. */
    clock_us=101000;assert(!firstboot_tx_allowed(FB_PA_KEY));
    hrc_pin_write(PIN_VHF_PA,true);hrc_dac_b_power(true);
    assert(!pa_high() && (dac_pd&DAC_B_OFF));
    nt_pa_ready(&test,clock_us);assert(test.state==NT_FAULT && test.reason==NT_LEASE);
    assert(quiets==1 && !test.keyed && !firstboot_tx_allowed(FB_FD_TX));
    clock_us++;nt_rf_tx(&test,clock_us);nt_pa_ready(&test,clock_us);
    assert(keys==1 && pa_calls==1 && !pa_high());assert(nt_cleanup(&test));
    /* Wrap-safe expiry and STOP during setup cannot leave a reusable grant. */
    start_tx(UINT32_MAX-50000u);clock_us+=99999u;assert(firstboot_tx_allowed(FB_BOARD_TX));
    clock_us++;assert(!firstboot_tx_allowed(FB_BOARD_TX));nt_tick(&test,clock_us);
    assert(test.reason==NT_LEASE && nt_cleanup(&test));
    start_tx(200000);assert(nt_stop(&test,test.session,NT_USER_STOP));
    assert(!firstboot_tx_allowed(FB_BOARD_TX) && nt_cleanup(&test));
    /* Generation changes invalidate an old grant even if its pointer is reused. */
    start_tx(300000);test.session++;assert(!firstboot_tx_allowed(FB_BOARD_TX));
    test.session--;assert(nt_stop(&test,test.session,NT_USER_STOP));assert(nt_cleanup(&test));
    start_tx(400000);test.started_us=0;test.ping_us=clock_us;
    clock_us=500000;test.ping_us=clock_us;
    assert(!firstboot_tx_allowed(FB_PA_GPIO));nt_tick(&test,clock_us);
    assert(test.reason==NT_DEADLINE && nt_cleanup(&test));
    /* Storage writes stay denied while a fresh native grant exists. */
    start_tx(600000);uint8_t bytes[256]={0};unsigned before=transactions;
    assert(!storage_claim() && !storage_claim_request());
    assert(!hrc_flash_spi0.write(0x711000,bytes,sizeof bytes));
    assert(!hrc_flash_spi0.erase(0x711000));
    assert(transactions==before && !forbidden_spi);
    assert(nt_stop(&test,test.session,NT_USER_STOP));assert(nt_cleanup(&test));
    /* A stored sample cannot start until every block has been accepted. */
    nt_config sample=config;
    sample.duration_ms=sample.lease_ms=NT_TX_PRELOAD_MS;
    sample.blocks=25;
    assert(firstboot_native_tx_config(&sample));
    nt_init(&test,&tx_ops);assert(nt_arm(&test,&sample,1));
    for(unsigned i=0;i<24;i++)assert(nt_load(&test,test.session,i,bytes));
    assert(!nt_begin(&test,test.session,clock_us) && !test.owned);
    assert(nt_load(&test,test.session,24,bytes));
    assert(nt_begin(&test,test.session,clock_us));
    uint32_t start=clock_us;
    for(unsigned i=0;i<25;i++) {
        clock_us=start+10000+i*30000;
        nt_boundary(&test,NT_IRQ_PHY,clock_us);
    }
    assert(test.state==NT_DRAIN && test.stats.tx_sent==25);
    clock_us+=NT_DRAIN_US;nt_tick(&test,clock_us);
    assert(test.reason==NT_TX_COMPLETE && nt_cleanup(&test));
    assert(!irq_depth && !pa_high());
    puts("Native TX gate: exact config, actual PA/DAC/FD paths, expiry/wrap, late callbacks, generations and storage denial PASS");
}
