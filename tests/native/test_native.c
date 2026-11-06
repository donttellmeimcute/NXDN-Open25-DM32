/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "native_test.h"
#include "hrc_native_chip.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct { unsigned starts,writes,cancels,requests,keys,offs,restores; int bad_start,bad_write,bad_restore; uint8_t data[60][36]; } mock;
static mock m;
static nt_test t;
static int start(void *p,nt_test *v) { (void)v; mock *x=p; x->starts++; return !x->bad_start; }
static int write_block(void *p,const uint8_t raw[36]) { mock *x=p; assert(x->writes<60); memcpy(x->data[x->writes++],raw,36); return !x->bad_write; }
static void cancel(void *p) { ((mock*)p)->cancels++; }
static void request(void *p) { ((mock*)p)->requests++; }
static void key(void *p) { ((mock*)p)->keys++; }
static void off(void *p) { ((mock*)p)->offs++; }
static int restore(void *p) { mock *x=p; x->restores++; return !x->bad_restore; }
static nt_ops ops={&m,start,write_block,cancel,request,key,off,restore,NULL,NULL,NULL};
static nt_config cfg(unsigned mode,unsigned count) { return (nt_config){145500000,count,2000,250,mode,1,0}; }
static void reset(void) { memset(&m,0,sizeof m); nt_init(&t,&ops); }
static void load(unsigned index) { uint8_t b[36]; for(unsigned j=0;j<36;j++) b[j]=(uint8_t)(index*37+j); assert(nt_load(&t,t.session,index,b)); }
static void begin(unsigned mode,unsigned count,uint32_t now)
{
    reset(); nt_config c=cfg(mode,count); assert(nt_arm(&t,&c,1));
    if (mode==NT_TRANSMIT) for(unsigned i=0;i<(count<8?count:8);i++) load(i);
    assert(nt_begin(&t,t.session,now));
}
static void valid_tx(void)
{
    begin(NT_TRANSMIT,12,0xffff0000u);
    uint32_t now=0xffff0000u+5000;
    for(unsigned i=0;i<12;i++,now+=30000) {
        assert(nt_ping(&t,t.session,now));
        nt_boundary(&t,NT_IRQ_PHY,now);
        nt_boundary(&t,NT_IRQ_TX_SLOT,now+100);
        nt_boundary(&t,NT_IRQ_RX_SLOT,now+200);
        nt_rf_tx(&t,now+1000);
        nt_pa_ready(&t,now+2000);
        if (i<4) load(i+8);
    }
    assert(t.state==NT_DRAIN && t.stats.tx_sent==12 && m.writes==12);
    assert(m.requests==1 && m.keys==1 && t.stats.duplicate_irqs==24);
    for(unsigned i=0;i<12;i++) for(unsigned j=0;j<36;j++) assert(m.data[i][j]==(uint8_t)(i*37+j));
    nt_boundary(&t,NT_IRQ_PHY,now); assert(m.cancels==1 && m.writes==12);
    nt_tick(&t,now+15000); assert(t.state==NT_STOPPED && t.reason==NT_TX_COMPLETE && m.offs==1);
    assert(nt_cleanup(&t) && !nt_cleanup(&t) && m.restores==1);
    assert(nt_stop(&t,t.session,NT_USER_STOP) && m.offs==1); /* idempotent */
}
static void independent_boundary_phases(void)
{
    /* Actual first attempt: TX slot primed at 7,651 us; PHY_RDY at 34,006.
     * The 26,355-us cross-source gap must not advance or fault the TX clock. */
    begin(NT_TRANSMIT,4,0);
    nt_boundary(&t,NT_IRQ_TX_SLOT,7651);
    nt_boundary(&t,NT_IRQ_RX_SLOT,8000);
    nt_boundary(&t,NT_IRQ_PHY,34006);
    assert(t.state==NT_TX && t.stats.tx_sent==1 && !t.stats.faults);
    nt_boundary(&t,NT_IRQ_TX_SLOT,37651);
    assert(t.stats.tx_sent==2 && t.stats.last_gap_us==30000);
    /* Other sources do not hide a stalled selected source. */
    nt_boundary(&t,NT_IRQ_PHY,64006);
    nt_boundary(&t,NT_IRQ_RX_SLOT,67651);
    nt_boundary(&t,NT_IRQ_PHY,94006);
    nt_tick(&t,97651);
    assert(t.state==NT_FAULT && t.reason==NT_STALLED && t.stats.tx_sent==2);
    begin(NT_TRANSMIT,4,0);nt_boundary(&t,NT_IRQ_TX_SLOT,1000);
    nt_boundary(&t,NT_IRQ_PHY,1500);nt_boundary(&t,NT_IRQ_TX_SLOT,21000);
    assert(t.reason==NT_EARLY);
}
static void failures(void)
{
    uint8_t b[36]={0};
    reset(); nt_config c=cfg(NT_TRANSMIT,3);
    assert(!nt_arm(&t,&c,0) && !m.starts);
    nt_config invalid[]={c,c,c,c,c,c};
    invalid[0].blocks=0; invalid[1].blocks=61; invalid[2].duration_ms=2001;
    invalid[3].lease_ms=99; invalid[4].mode=3; invalid[5].frequency_hz=0;
    for(unsigned i=0;i<6;i++) assert(!nt_arm(&t,&invalid[i],1));
    assert(nt_arm(&t,&c,1)); uint32_t old=t.session;
    assert(!nt_begin(&t,t.session,0) && !m.starts);
    assert(!nt_load(&t,t.session+1,0,b)); assert(!nt_load(&t,t.session,1,b));
    load(0); assert(!nt_load(&t,t.session,0,b)); load(1);
    assert(nt_begin(&t,t.session,0));
    nt_boundary(&t,NT_IRQ_PHY,1000); nt_rf_tx(&t,1500);
    nt_boundary(&t,NT_IRQ_PHY,31000); nt_boundary(&t,NT_IRQ_PHY,61000);
    assert(t.state==NT_FAULT && t.reason==NT_UNDERRUN && m.offs==1 && t.stats.tx_sent==2);
    nt_pa_ready(&t,62000); assert(!m.keys);
    assert(!nt_arm(&t,&c,1) && !nt_reset(&t));
    assert(nt_cleanup(&t) && nt_reset(&t) && nt_arm(&t,&c,1) && t.session!=old);
    assert(!nt_stop(&t,old,NT_USER_STOP) && t.state==NT_ARMED);
    nt_deny(&t); assert(t.reason==NT_DENIED && !t.owned && m.offs==1);

    begin(NT_TRANSMIT,3,100); nt_rf_tx(&t,500); nt_pa_ready(&t,600); assert(!m.requests && !m.keys);
    nt_boundary(&t,NT_IRQ_PHY,1000); nt_rf_tx(&t,2000);
    nt_tick(&t,250100); nt_pa_ready(&t,250101);
    assert(t.reason==NT_LEASE && !m.keys && m.offs==1);
    assert(!nt_ping(&t,t.session,250102)); nt_abnormal(&t); assert(t.reason==NT_LEASE && t.stats.faults==1);
    nt_cleanup(&t); assert(m.restores==1);

    begin(NT_TRANSMIT,3,0); nt_tick(&t,100000); assert(t.reason==NT_STALLED);
    begin(NT_TRANSMIT,3,0); nt_boundary(&t,NT_IRQ_PHY,1000); nt_tick(&t,61000); assert(t.reason==NT_STALLED);
    begin(NT_TRANSMIT,3,0); nt_boundary(&t,NT_IRQ_PHY,1000); nt_boundary(&t,NT_IRQ_PHY,35000); assert(t.reason==NT_LATE);
    begin(NT_TRANSMIT,3,0); nt_boundary(&t,NT_IRQ_PHY,1000); nt_boundary(&t,NT_IRQ_PHY,21000); assert(t.reason==NT_EARLY);
    begin(NT_TRANSMIT,3,0); m.bad_write=1; nt_boundary(&t,NT_IRQ_PHY,1000); assert(t.reason==NT_IO && !t.stats.tx_sent);
    begin(NT_TRANSMIT,60,0); /* Valid but slow boundaries cannot extend the absolute deadline. */
    for(unsigned i=0;i<58;i++) {
        uint32_t now=99000+i*33000;
        assert(nt_ping(&t,t.session,now));nt_boundary(&t,NT_IRQ_PHY,now);
        if(t.stats.tx_loaded<60)load(t.stats.tx_loaded);
    }
    nt_tick(&t,2000000);
    assert(t.state==NT_FAULT && t.reason==NT_DEADLINE && m.offs==1 && t.stats.tx_sent==58);
    begin(NT_RECEIVE,3,0); nt_rf_tx(&t,1000); assert(t.reason==NT_UNEXPECTED_RF && !m.requests);
    begin(NT_RECEIVE,3,0); nt_abnormal(&t); assert(t.reason==NT_ABNORMAL && m.offs==1);
    begin(NT_RECEIVE,3,0); nt_stop(&t,t.session,NT_USER_STOP); m.bad_restore=1;
    assert(nt_cleanup(&t) && t.reason==NT_RESTORE && t.stats.restore_failures==1 && m.offs==2);
    reset(); assert(nt_arm(&t,&c,1)); load(0);load(1);m.bad_start=1;
    assert(!nt_begin(&t,t.session,0) && t.reason==NT_IO && m.offs==1 && t.owned);
    assert(nt_cleanup(&t) && m.restores==1);
}
static void receive(void)
{
    uint8_t b[36]; for(unsigned i=0;i<36;i++) b[i]=(uint8_t)(i^0xa5);
    begin(NT_RECEIVE,32,0);
    for(unsigned i=0;i<17;i++) nt_rx(&t,i*1000,0x800,3,b);
    assert(t.reason==NT_RX_OVERFLOW && t.stats.rx_seen==17 && t.stats.rx_dropped==1 && t.rx_count==16);
    assert(!nt_reset(&t)); nt_cleanup(&t); assert(!nt_reset(&t));
    for(unsigned i=0;i<16;i++) { nt_record r; assert(nt_pop(&t,t.session,&r)); assert(r.sequence==i && r.rx_info==3 && !memcmp(r.raw,b,36)); }
    assert(t.stats.rx_read==16 && nt_reset(&t));
    begin(NT_RECEIVE,1,0); nt_rx(&t,1000,0x800,0x1234,b); assert(t.reason==NT_RX_LIMIT && t.rx_count==1 && m.offs==1);
    begin(NT_RECEIVE,3,0xfffffff0u);
    for(unsigned i=1;i<20;i++) { uint32_t now=0xfffffff0u+i*100000u; assert(nt_ping(&t,t.session,now)); }
    nt_tick(&t,0xfffffff0u+2000000u); assert(t.reason==NT_RX_DURATION && t.state==NT_STOPPED);
}
static unsigned prep_count,fast_count,irq_count,restore_count;
static int hprepare(void *p,const nt_config *c) { (void)p;(void)c;prep_count++;return 1; }
static void hfast(void *p) { (void)p;fast_count++; }
static void hnoop(void *p) { (void)p; }
static int hirqs(void *p,nt_test *v) { (void)p;(void)v;irq_count++;return 1; }
static int hrestore(void *p) { (void)p;restore_count++;return 1; }
static void chip_tests(void)
{
    uint32_t regs[0x600/4],before[0x600/4]; uint8_t buf[0x1000];
    memset(buf,0xcc,sizeof buf);
    for(unsigned i=0;i<sizeof regs/sizeof *regs;i++) regs[i]=0x80000000u+i;
    regs[C7_WORK_MODE/4]=0x6e; regs[C7_LAYER2_TXRX_CTRL/4]=0; memcpy(before,regs,sizeof regs);
    hrc7000_t dev={.regs=regs,.buf=buf}; hn_chip chip; nt_ops hw;
    hn_hooks hooks={NULL,hprepare,hfast,hnoop,hnoop,hirqs,hnoop,hnoop,hrestore,NULL,NULL};
    hn_init(&chip,&dev,&hooks,&hw); nt_init(&t,&hw); nt_config c=cfg(NT_RECEIVE,2);
    assert(nt_arm(&t,&c,0) && nt_begin(&t,t.session,0));
    assert(regs[C7_WORK_MODE/4]==2 && regs[C7_LAYER2_CONTROL/4]==0x40 && regs[C7_LAYER2_TXRX_CTRL/4]==0x41);
    /* Reconstruct the canonical CAI sync from the hardware's 24-bit fields,
     * rather than repeating the writer's split constants. */
    uint64_t sync=((uint64_t)(regs[C7_RECV_MS_SYNC_H/4]&0xffffffu)<<24)|(regs[C7_RECV_MS_SYNC_L/4]&0xffffffu);
    assert(sync==UINT64_C(0x5575f5ff77ff) && (regs[C7_RECV_MS_SYNC_H/4]&0x80000000u));
    assert(!(regs[C7_RECV_MS_SYNC_L/4]&0xff000000u));
    assert(regs[C7_RECV_BS_SYNC_H/4]==regs[C7_RECV_MS_SYNC_H/4] && regs[C7_RECV_BS_SYNC_L/4]==regs[C7_RECV_MS_SYNC_L/4]);
    assert(!(regs[C7_RECV_RC_SYNC_H/4]&0x80000000u));
    uint32_t diagnostic[NT_SNAPSHOT_WORDS];
    assert(hw.snapshot(hw.ctx,diagnostic,NT_SNAPSHOT_WORDS-1)==0);
    assert(hw.snapshot(hw.ctx,diagnostic,NT_SNAPSHOT_WORDS)==NT_SNAPSHOT_WORDS);
    assert(diagnostic[0]==1 && diagnostic[1] && diagnostic[3]==1 && diagnostic[8]==2 && diagnostic[11]==0x41);
    for(unsigned j=0;j<36;j++) buf[C7_RX_RAM+0x30+j]=(uint8_t)j;
    regs[C7_SYS_INTERP_LIST/4]=(1u<<11)|(1u<<20); regs[C7_RX_TYPE_INFO/4]=0x12345672;
    hn_sys(&chip,1000); assert(regs[C7_SYS_INTERP_CLEAR/4]==1u<<11 && t.stats.rx_saved==1);
    assert(hw.snapshot(hw.ctx,diagnostic,NT_SNAPSHOT_WORDS)==NT_SNAPSHOT_WORDS);
    assert(diagnostic[3]==2 && diagnostic[4]==1000 && diagnostic[5]==((1u<<11)|(1u<<20)) && diagnostic[6]==0x12345672);
    nt_record r; assert(nt_pop(&t,t.session,&r) && r.rx_info==0x12345672);
    assert(r.sys_flags==((1u<<11)|(1u<<20))); /* preserve unrelated diagnostic flags without acknowledging them */
    for(unsigned j=0;j<36;j++) assert(r.raw[j]==j);
    nt_stop(&t,t.session,NT_USER_STOP); assert(nt_cleanup(&t));
    assert(hw.snapshot(hw.ctx,diagnostic,NT_SNAPSHOT_WORDS)==NT_SNAPSHOT_WORDS);
    assert(!diagnostic[1] && diagnostic[3]==2 && diagnostic[32]==2 && diagnostic[35]==0x41);
    unsigned restored[]={0x100,0x400,0x404,0x408,0x39c,0x13c,0x140,0x144,0x148,0x14c,0x150,0x154,0x158,0x15c,0x160};
    for(unsigned i=0;i<15;i++) assert(regs[restored[i]/4]==before[restored[i]/4]);
    unsigned untouched[]={C7_RF_MODE,C7_RF_MOD_BIAS,C7_DEV_LIMITER,C7_SIG_CENTER,0x16c,0x1ac};
    for(unsigned i=0;i<6;i++) assert(regs[untouched[i]/4]==before[untouched[i]/4]);
    assert(prep_count==1 && fast_count==1 && irq_count==1 && restore_count==1);
    assert(nt_arm(&t,&(nt_config){145500000,2,2000,250,NT_TRANSMIT,0,0},1)); load(0);load(1);
    assert(nt_begin(&t,t.session,0)); hn_slot(&chip,NT_IRQ_TX_SLOT,1000);
    for(unsigned j=0;j<36;j++) assert(buf[j]==j && buf[36+j]==j);
    assert(buf[72]==0xcc && regs[C7_LAYER2_TXRX_CTRL/4]==0x80);
    hn_slot(&chip,NT_IRQ_TX_SLOT,31000);
    for(unsigned j=0;j<36;j++) assert(buf[j]==(uint8_t)(37+j) && buf[36+j]==(uint8_t)(37+j));
    regs[C7_SYS_INTERP_LIST/4]=1u<<16; hn_sys(&chip,32000);
    assert(t.reason==NT_ABNORMAL && regs[C7_LAYER2_TXRX_CTRL/4]==0 && regs[C7_LAYER2_CONTROL/4]==0);
    assert(nt_cleanup(&t));
}
static void fuzz(void)
{
    uint32_t rand=0x700025u,now=0; uint8_t data[36]={0};
    for(unsigned run=0;run<500;run++) {
        begin(run&1?NT_RECEIVE:NT_TRANSMIT,8,now);
        for(unsigned i=0;i<200;i++) {
            rand^=rand<<13;rand^=rand>>17;rand^=rand<<5;now+=rand%8000;
            switch(rand%8) {
            case 0: nt_tick(&t,now);break;
            case 1: nt_boundary(&t,NT_IRQ_PHY,now);break;
            case 2: nt_rx(&t,now,0x800,rand,data);break;
            case 3: nt_ping(&t,t.session+(rand&1),now);break;
            case 4: nt_rf_tx(&t,now);break;
            case 5: nt_pa_ready(&t,now);break;
            case 6: nt_stop(&t,t.session,NT_USER_STOP);break;
            case 7: nt_abnormal(&t);break;
            }
            assert(t.rx_count<=16 && t.tx_count<=8 && t.stats.tx_sent<=8);
            assert(m.keys<=1 && m.offs<=1);
            if (t.state==NT_FAULT || t.state==NT_STOPPED) assert(!t.keyed && !t.key_requested && !t.tx_count);
        }
        nt_stop(&t,t.session,NT_USER_STOP); nt_cleanup(&t);
    }
}
int main(void)
{
    valid_tx(); independent_boundary_phases(); failures(); receive(); chip_tests(); fuzz();
    reset();assert(!nt_arm_voice(&t,145500000,0));assert(!nt_arm_voice(&t,145500000,300001));
    assert(nt_arm_voice(&t,145500000,300000));assert(t.config.mode==NT_RECEIVE && t.config.audio && t.config.duration_ms==300000);
    assert(nt_begin(&t,t.session,0));assert(!nt_load(&t,t.session,0,(uint8_t[36]){0}));
    for(uint32_t now=400000;now<300000000;now+=400000)assert(nt_ping(&t,t.session,now));
    nt_tick(&t,300000000);assert(t.reason==NT_RX_DURATION && !t.stats.pa_keys && nt_cleanup(&t));
    nt_config ordinary=cfg(NT_RECEIVE,2);ordinary.audio=1;assert(!nt_arm(&t,&ordinary,0));
    printf("PASS native controller/chip: lifecycle, wraparound, leases, deadlines, queues, IRQ dedup, PA cancellation, restore, 100000 adversarial events; context=%zu bytes\n",sizeof t);
    return 0;
}
