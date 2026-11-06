/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Actual generated GPIO/DAC/SPI HAL, NOR, FD6818, storage and native controller.
 * Only the SoC registers, scheduler and external buses are host doubles. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "firstboot/firstboot.h"
#include "native_test.h"
#include "debuglink/debuglink_proto.h"
#include "storage/storage.h"
#include "rf/fd6818/fd6818.h"
#include "pins.h"
/* Suppress the C-SKY register/assembly header in the included actual drivers. */
#define PLATFORM_HRC7000_H
static uint32_t regs[128],dac_pd,dac_mode,dac_a,dac_b,reg_dac_c,io2;
#define REG32(a) regs[(a)/4]
#define GPIO_DR 0u
#define GPIO_DDR 4u
#define GPIO_INTEN 8u
#define GPIO_INTTYPE 12u
#define GPIO_BOTHEDGE 16u
#define GPIO_POLARITY 20u
#define GPIO_INTMASK 24u
#define GPIO_EOI 28u
#define GPIO_INTSTATUS 32u
#define DAC_PD_CTRL dac_pd
#define DAC_PD_MODE_EN dac_mode
#define DAC_DATA_A dac_a
#define DAC_DATA_B dac_b
#define DAC_DATA_C reg_dac_c
#define DAC_A_OFF 1u
#define DAC_B_OFF 2u
#define IO_DIPLEX2 io2
#define IO_DIPLEX2_DAC_B 8u
#define ADC_BASE 400u
#define SPI0_BASE 0u
static unsigned irq_depth,transactions,forbidden_spi,fd_tx_frames;
static bool selected,header;
static bool bus_failure;
static uint8_t opcode;
static uint32_t hrc_gpio_bank(uint8_t p) { return (p/32u)*128u; }
static uint32_t hrc_gpio_bit(uint8_t p) { return 1u<<(p&31u); }
static uint32_t hrc_irq_save(void) { return irq_depth++; }
static void hrc_irq_restore(uint32_t saved) { assert(irq_depth==saved+1);irq_depth=saved; }
void hrc_pin_out(uint8_t,bool);
void hrc_pin_write(uint8_t,bool);
typedef struct { uint8_t cs_pin;uint32_t write_floor;const hal_flash_map_t *map; } hrc_flash_cfg_t;
extern hal_flash_t hrc_flash_spi0;
static void hrc_ssi_select(uint32_t b,bool on) { (void)b;selected=on;header=on; }
static void hrc_ssi_init(uint32_t b,uint16_t d) { (void)b;(void)d; }
static bool hrc_ssi_xfer(uint32_t b,const uint8_t *tx,uint8_t *rx,uint32_t n)
{
    (void)b;assert(selected);
    if (bus_failure) return false;
    if (header) {
        assert(tx && n);opcode=tx[0];transactions++;header=false;
        if (opcode==0x06 || opcode==0x02 || opcode==0x20) forbidden_spi++;
    } else if (rx) {
        memset(rx,0xa5,n);
        if (opcode==0x9f) { assert(n==3);rx[0]=0xef;rx[1]=0x40;rx[2]=0x18; }
    }
    return true;
}
static void hrc_chip_set_uid(const uint8_t *id,uint32_t n) { (void)id;(void)n; }
void hal_delay_us(uint32_t n) { (void)n; }
void hal_delay_ms(uint32_t n) { (void)n; }
uint32_t hal_millis(void) { return 1; }
/* Include real translation units so static HAL entry points remain testable. */
#include "boards/dm32/dm32_firstboot.inc"
#include "platform/hrc7000/firstboot_irq.c"
#include "platform/hrc7000/gpio.c"
#include "platform/hrc7000/dac.c"
#include "platform/hrc7000/flash_spi0.c"
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
int xSemaphoreTake(SemaphoreHandle_t s,unsigned n) { (void)s;(void)n;return 1; }
int xSemaphoreGive(SemaphoreHandle_t s) { (void)s;return 1; }
int xTaskGetSchedulerState(void) { return taskSCHEDULER_RUNNING; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (void *)1; }
const char *pcTaskGetName(TaskHandle_t t) { (void)t;return "host"; }
const board_t board={.flash=&hrc_flash_spi0};
void storage_lock(void) {} void storage_unlock(void) {}
void storage_quick_lock(void) {} void storage_quick_unlock(void) {}
static uint16_t rfregs[256];
static void rf_write(rfbus_t *b,uint8_t r,uint16_t v)
{ (void)b;rfregs[r]=v;if(r==FD6818_REG_STATE && v==FD6818_STATE_TX)fd_tx_frames++; }
static uint16_t rf_read(rfbus_t *b,uint8_t r) { (void)b;return rfregs[r]; }
static uint32_t word(const uint8_t *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static unsigned unexpected_keys;
static int rx_start(void *ctx,nt_test *t) { (void)ctx;(void)t;return 1; }
static void no_key(void *ctx) { (void)ctx;unexpected_keys++; }
static void rx_quiet(void *ctx) { (void)ctx; }
static int rx_restore(void *ctx) { (void)ctx;return 1; }
int main(void)
{
    /* Even a stale high PA output is actively cleared by an attempted key. */
    for (unsigned p=0;p<96;p++) {
        bool pa=p==PIN_UHF_PA || p==PIN_VHF_PA;
        regs[hrc_gpio_bank(p)/4+GPIO_DR/4]=0xffffffffu;
        hrc_pin_out((uint8_t)p,true);
        assert(!!(REG32(hrc_gpio_bank(p)+GPIO_DR)&hrc_gpio_bit(p))==!pa);
        hrc_pin_write((uint8_t)p,false);
        assert(!(REG32(hrc_gpio_bank(p)+GPIO_DR)&hrc_gpio_bit(p)));
    }
    hrc_modem_analog_init(1762);assert(dac_b==0 && reg_dac_c==1762 && (dac_pd&DAC_B_OFF));
    hrc_dac_b_power(true);assert(dac_pd&DAC_B_OFF);
    hrc_dac_b_power(false);assert(dac_pd&DAC_B_OFF);
    const hal_flash_map_t map={.settings_addr=0x711000,.settings_size=0x8000,.journal_addr=0x719000};
    const hrc_flash_cfg_t cfg0={.cs_pin=PIN_FLASH_CS,.write_floor=0x700000,.map=&map};
    hrc_flash_spi0_configure(&cfg0);assert(hrc_flash_spi0.init());
    assert(hrc_flash_spi0.size==0x1000000);
    uint8_t b[512]={0};assert(hrc_flash_spi0.read(0x711000,b,sizeof b));
    bus_failure=true;
    assert(!hrc_flash_spi0.read(0x711000,b,sizeof b) && !selected);
    assert(!hrc_flash_spi0.init() && hrc_flash_spi0.size==0 && !selected);
    bus_failure=false;assert(hrc_flash_spi0.init());
    unsigned before=transactions;
    for (unsigned a=0;a<=0x1000000;a+=4096) {
        assert(!hrc_flash_spi0.write(a,b,sizeof b));assert(!hrc_flash_spi0.erase(a));
    }
    assert(!spi_nor_program(&nor,0x711000,b,sizeof b));
    assert(!spi_nor_erase_sector(&nor,0x711000));
    assert(!storage_claim_request());assert(!storage_claim());assert(!storage_claim_pending());
    assert(before==transactions && !forbidden_spi && !selected);
    rfbus_t bus={.write=rf_write,.read=rf_read};fd6818_t fd0;
    fd6818_attach(&fd0,&bus);fd6818_init(&fd0);fd6818_set_mode(&fd0,FD6818_MODE_DMR);
    for (unsigned i=0;i<100;i++) {
        fd6818_write(&fd0,FD6818_REG_STATE,FD6818_STATE_TX);
        fd6818_set_state(&fd0,FD6818_STATE_TX);
        fd6818_dmr_burst_at(&fd0,true,FD6818_BW_DMR,446050000);
        fd6818_dmr_burst_at(&fd0,false,FD6818_BW_DMR,446050000);
    }
    assert(!fd_tx_frames && fd0.state==FD6818_STATE_RX && bus.writes);
    nt_test t;nt_init(&t,NULL);
    nt_config c={.mode=NT_TRANSMIT,.frequency_hz=446050000,.blocks=1,.duration_ms=200,.lease_ms=100};
    assert(!nt_arm(&t,&c,1));assert(!t.owned && t.state==NT_IDLE);
    c.mode=NT_RECEIVE;assert(nt_arm(&t,&c,0));assert(t.state==NT_ARMED);
    const nt_ops rx_ops={.start=rx_start,.quiet=rx_quiet,.restore=rx_restore,.request_key=no_key,.pa_on=no_key};
    nt_init(&t,&rx_ops);c.blocks=2;c.lease_ms=250;
    assert(nt_arm(&t,&c,0));assert(nt_begin(&t,t.session,1000));
    nt_rf_tx(&t,2000);nt_pa_ready(&t,3000);
    nt_abnormal(&t);
    assert(t.state==NT_RX && !t.key_requested && !t.keyed && !t.stats.pa_keys && !unexpected_keys);
    uint8_t sample[36]={0x55};nt_rx(&t,4000,0x800,0,sample);
    assert(t.rx_count==1 && t.stats.rf_irqs==1 && t.stats.abnormal_irqs==1 && t.stats.ignored_irqs==3 && !t.stats.faults);
    assert(nt_stop(&t,t.session,NT_USER_STOP));assert(nt_cleanup(&t));
    nt_record received;assert(nt_pop(&t,t.session,&received) && received.raw[0]==0x55);
    assert(nt_arm(&t,&c,0));assert(nt_begin(&t,t.session,1000));
    nt_rf_tx(&t,252000);
    assert(t.state==NT_FAULT && t.reason==NT_LEASE && !unexpected_keys && !t.stats.pa_keys);
    assert(nt_cleanup(&t));
    for (unsigned cmd=0;cmd<256;cmd++) {
        bool expected=cmd==1 || cmd==2 || cmd==3 || cmd==5 || cmd==6 || cmd==8 || cmd==9 ||
            cmd==10 || cmd==11 || cmd==13 || cmd==0x20 || cmd==0x21 || cmd==0x22;
        assert(firstboot_debug_allowed((uint8_t)cmd)==expected);
    }
    uint8_t status[1+4*FIRSTBOOT_WORDS],req=0;
    assert(firstboot_command(&req,1,status,sizeof status)==sizeof status);
    assert(status[0]==0 && word(status+1)==1 && word(status+5)==31);
    assert(word(status+9+4*FB_PA_GPIO)==2);
    assert(word(status+9+4*FB_FLASH_WRITE)==4097);
    assert(word(status+9+4*FB_FLASH_ERASE)==4097);
    assert(word(status+9+4*FB_NOR_WRITE_ENABLE)==2);
    assert(word(status+9+4*FB_FD_TX)==300);
    assert(word(status+9+4*FB_STORAGE_CLAIM)==2);
    assert(word(status+9+4*FB_DEBUG_COMMAND)==243);
    assert(word(status+9+4*FB_NATIVE_TX)==1);
    for (unsigned cap=0;cap<sizeof status;cap++) {
        memset(status,0xee,sizeof status);
        assert(firstboot_command(&req,1,status,cap)==(cap?1u:0u));
        for (unsigned i=cap?1:0;i<sizeof status;i++)assert(status[i]==0xee);
    }
    req=1;assert(firstboot_command(&req,1,status,sizeof status)==1 && status[0]==DL_ST_BAD_ARG);
    uint8_t restart[5]={0};assert(firstboot_reboot_allowed(restart,sizeof restart));
    assert(!firstboot_reboot_allowed(NULL,5));assert(!firstboot_reboot_allowed(restart,4));
    restart[0]=1;assert(!firstboot_reboot_allowed(restart,5));
    assert(!irq_depth && !forbidden_spi && !fd_tx_frames);
    puts("firstboot guards: actual driver paths, 8194 HAL write/erase refusals, cable filter and counters PASS");
    return 0;
}
