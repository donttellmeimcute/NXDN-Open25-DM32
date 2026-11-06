/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "hrc_native_port.h"
#include "platform.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
hrc_irq_fn hrc_irq_table[64];
uint32_t mock_pic[2],mock_tmr[7];
uint32_t mock_pic_history;
static uint32_t clock_us,depth,period;
static unsigned acks[7],offs,keys;
uint32_t hrc_irq_save(void) { return depth++; }
void hrc_irq_restore(uint32_t prior) { assert(depth==prior+1);depth=prior; }
void hrc_irq_enable(uint8_t n,bool on) { uint32_t bit=1u<<(n&31); if(on)mock_pic[n/32]&=~bit;else mock_pic[n/32]|=bit; }
void hrc_irq_register(uint8_t n,hrc_irq_fn fn) { hrc_irq_enable(n,false);hrc_irq_table[n]=fn; }
void hrc_timer_ack(unsigned n) { acks[n]++; }
void hrc_timer_stop(unsigned n) { mock_tmr[n]=0; }
void hrc_timer_periodic(unsigned n,uint32_t us) { assert(n==6);period=us;mock_tmr[n]=3; }
uint32_t hrc_micros(void) { return clock_us; }
void hrc7000_audio_bits(hrc7000_t *m,uint32_t set,uint32_t clear) { (void)m;(void)set;(void)clear; }
static void prior(void) { assert(0); }
static void fast(void *p) { (void)p;offs++; }
static void noop(void *p) { (void)p; }
static void key(void *p) { (void)p;keys++; }
static int prepare(void *p,const nt_config *c) { (void)p;(void)c;return 1; }
static int restore(void *p) { (void)p;return 1; }
static int on(void *p,nt_test *t) { return hn_port_on(p,t); }
static void quiet(void *p) { (void)p;hn_port_quiet(); }
static void reset_irq(void *p) { (void)p;hn_port_restore(); }
static uint32_t lock(void *p) { (void)p;return hrc_irq_save(); }
static void unlock(void *p,uint32_t irq) { (void)p;hrc_irq_restore(irq); }
int main(void)
{
    uint32_t regs[0x600/4]={0};uint8_t buf[0x1000]={0};
    hrc7000_t modem={.regs=regs,.buf=buf};hn_chip chip;nt_test test;nt_ops ops;
    hn_hooks hooks={&chip,prepare,fast,noop,key,on,quiet,reset_irq,restore,lock,unlock};
    hn_init(&chip,&modem,&hooks,&ops);nt_init(&test,&ops);
    for(unsigned i=0;i<64;i++)hrc_irq_table[i]=prior;
    mock_pic[0]=0xa1234567;mock_pic[1]=0x13579bde;
    uint32_t masks[2]={mock_pic[0],mock_pic[1]};
    mock_tmr[3]=1;assert(!hn_port_on(&chip,&test));mock_tmr[3]=0;
    mock_tmr[6]=1;assert(!hn_port_on(&chip,&test));mock_tmr[6]=0;
    assert(mock_pic[0]==masks[0] && mock_pic[1]==masks[1] && hrc_irq_table[29]==prior);
    nt_config c={145500000,3,2000,250,NT_TRANSMIT,1,0};uint8_t raw[36]={0};
    assert(nt_arm(&test,&c,1));assert(nt_load(&test,test.session,0,raw));assert(nt_load(&test,test.session,1,raw));
    assert(nt_begin(&test,test.session,0));assert(period==1000 && mock_tmr[6]==3 && !depth);
    assert(!hn_port_on(&chip,&test));
    clock_us=1000;hrc_irq_table[IRQ_MODEM_TS_TX]();assert(test.stats.tx_sent==1);
    hrc_irq_table[IRQ_MODEM_RF_TX]();assert(test.key_requested);
    hrc_irq_fn delayed_pa=hrc_irq_table[IRQ_TIMER3];
    /* No cable command or radio-task service: the real timer ISR expires it. */
    clock_us=250000;hrc_irq_table[IRQ_TIMER6]();
    assert(test.state==NT_FAULT && test.reason==NT_LEASE && offs==1 && acks[6]==1);
    assert((mock_pic[0]&(1u<<IRQ_TIMER3)) && !(mock_pic[0]&(1u<<IRQ_TIMER6)));
    delayed_pa();assert(!keys && !mock_tmr[3] && acks[3]==1);
    assert(nt_cleanup(&test) && !test.owned && !mock_tmr[6]);
    assert(mock_pic[0]==masks[0] && mock_pic[1]==masks[1]);
    for(unsigned i=0;i<64;i++)assert(hrc_irq_table[i]==prior);
    hn_port_restore();delayed_pa();assert(!keys && !depth); /* stale callback after restored context */

    uint8_t next[36];memset(next,0xa5,sizeof next);
    clock_us=300000;
    assert(nt_reset(&test));
    assert(nt_arm_live(&test,145500000));
    assert(nt_load(&test,test.session,0,raw) && nt_load(&test,test.session,1,next));
    assert(nt_load(&test,test.session,2,raw) && nt_begin(&test,test.session,clock_us));
    regs[C7_SYS_INTERP_LIST/4]=1u<<12;
    clock_us=329200;hrc_irq_table[IRQ_MODEM_SYS]();
    assert(!test.stats.tx_sent && regs[C7_LAYER2_TXRX_CTRL/4]==0);
    clock_us=332800;hrc_irq_table[IRQ_MODEM_TS_TX]();
    assert(test.stats.tx_sent==1 && regs[C7_LAYER2_TXRX_CTRL/4]==0x80);
    hrc_irq_table[IRQ_MODEM_RF_TX]();assert(test.key_requested);
    regs[C7_LAYER2_TXRX_CTRL/4]=0; /* fake the hardware consuming the send strobe */
    clock_us=333000;hrc_irq_table[IRQ_MODEM_SYS]();
    assert(!memcmp(buf,raw,36)); /* same-slot event must not replace active payload */
    clock_us=359200;hrc_irq_table[IRQ_MODEM_SYS]();
    assert(test.stats.tx_sent==1 && regs[C7_LAYER2_TXRX_CTRL/4]==0);
    assert(!memcmp(buf,next,36) && !memcmp(buf+36,next,36));
    clock_us=362800;hrc_irq_table[IRQ_MODEM_TS_TX]();
    assert(test.stats.tx_sent==2 && regs[C7_LAYER2_TXRX_CTRL/4]==0x80);
    clock_us=392800;hrc_irq_table[IRQ_MODEM_TS_TX](); /* no PHY advance */
    assert(test.state==NT_FAULT && test.reason==NT_IO && test.stats.tx_sent==2);
    assert(regs[C7_LAYER2_TXRX_CTRL/4]==0 && nt_cleanup(&test));
    uint32_t trace[NT_TRACE_WORDS];nt_trace_summary(trace);
    assert(trace[0]==2 && trace[24]==359200 && trace[25]==1);
    puts("PASS real native interrupt port: timer conflicts, independent lease shutdown, delayed PA, exact handler/mask restoration");
    return 0;
}
