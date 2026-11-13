/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "hrc_native_chip.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned keys,quiets;
uint32_t firstboot_irq_save(void) { return 0; }
void firstboot_irq_restore(uint32_t irq) { (void)irq; }
static int prepare(void *ctx,const nt_config *c) { (void)ctx;return c->mode==NT_RECEIVE; }
static void off(void *ctx) { (void)ctx;quiets++; }
static void key(void *ctx) { (void)ctx;keys++; }
static void noop(void *ctx) { (void)ctx; }
static int irq_on(void *ctx,nt_test *t) { (void)ctx;(void)t;return 1; }
static int restore(void *ctx) { (void)ctx;return 1; }
int main(void)
{
    uint32_t regs[0x600/4]={0};uint8_t ram[0x1000]={0};
    hrc7000_t modem={.regs=regs,.buf=ram};hn_chip chip;nt_ops ops;nt_test test;
    hn_hooks hooks={NULL,prepare,off,key,key,irq_on,noop,noop,restore,NULL,NULL};
    hn_init(&chip,&modem,&hooks,&ops);nt_init(&test,&ops);
    nt_config cfg={145500000,2,2000,250,NT_RECEIVE,1,0,0};
    assert(nt_arm(&test,&cfg,0) && nt_begin(&test,test.session,0));
    for(unsigned i=0;i<36;i++)ram[C7_RX_RAM+0x30+i]=(uint8_t)(0x40+i);
    regs[C7_RX_TYPE_INFO/4]=0x12345000;
    regs[C7_SYS_INTERP_LIST/4]=(1u<<16)|(1u<<11)|(1u<<12);
    hn_sys(&chip,1000);hn_rftx(&chip,2000);nt_pa_ready(&test,3000);
    assert(test.state==NT_RX && test.stats.rx_saved==1 && !test.stats.faults && !keys);
    assert(test.stats.abnormal_irqs==1 && test.stats.rf_irqs==1 && test.stats.phy_irqs==1);
    nt_record r;assert(nt_pop(&test,test.session,&r) && r.sys_flags==regs[C7_SYS_INTERP_LIST/4]);
    for(unsigned i=0;i<36;i++)assert(r.raw[i]==(uint8_t)(0x40+i));
    nt_tick(&test,251000);assert(test.state==NT_FAULT && test.reason==NT_LEASE && quiets==1);
    assert(nt_cleanup(&test) && !keys && !test.stats.pa_keys);
    puts("firstboot RX: warning+data retained, RF notifications never key, lease still stops PASS");
    return 0;
}
