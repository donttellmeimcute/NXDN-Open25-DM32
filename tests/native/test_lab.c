/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "native_lab.h"
#include "board.h"
#include "radio/beep.h"
#include "debuglink/debuglink_proto.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint32_t clock_us;
static unsigned depth,starts,offs,restores,retunes;
static int busy,on=1,beeping,stop_during_start;
static uint16_t volts=8000;
static nt_test *live;
static uint32_t replace_sid,replacement_sid;
static int replace_arm;
static uint32_t arm(unsigned mode);
static int op(unsigned cmd,uint32_t sid);
void mock_enter(void) { depth++; }
void mock_exit(void) { assert(depth);depth--; }
uint32_t hal_micros(void) { return clock_us; }
uint32_t hal_millis(void) { return clock_us/1000; }
int p25_lab_busy(void) { return busy; }
#ifndef NATIVE_CHANNEL_TEST
/* The worker's side of the radio core's channel, idle in these tests. */
int p25_lab_live_begin(uint32_t sid) { (void)sid;return 0; }
int p25_lab_channel_rx(uint32_t hz) { (void)hz;return 0; }
void p25_lab_live_up(void) { }
#endif
bool beep_busy(void) {
    if(replace_arm) { replace_arm=0;assert(!op(5,replace_sid));replacement_sid=arm(NT_RECEIVE);return true; }
    return beeping;
}
static bool switched(void) { return on; }
static uint16_t battery(void) { return volts; }
static int hw_start(void *p,nt_test *t) { (void)p;live=t;starts++;if(stop_during_start)nt_stop(t,t->session,NT_USER_STOP);return 1; }
static int hw_write(void *p,const uint8_t b[36]) { (void)p;(void)b;return 1; }
static void noop(void *p) { (void)p; }
static void quiet(void *p) { (void)p;offs++; }
static int hw_restore(void *p) { (void)p;restores++;return 1; }
static uint32_t lock(void *p) { (void)p;mock_enter();return 0; }
static void unlock(void *p,uint32_t irq) { (void)p;(void)irq;mock_exit(); }
static nt_ops ops={NULL,hw_start,hw_write,noop,noop,noop,quiet,hw_restore,lock,unlock,NULL};
static unsigned snapshots;
static size_t diagnostic(void *ctx,uint32_t *out,size_t cap)
{
    (void)ctx;assert(cap==NT_SNAPSHOT_WORDS);snapshots++;
    for(unsigned i=0;i<NT_SNAPSHOT_WORDS;i++)out[i]=i+1;
    return NT_SNAPSHOT_WORDS;
}
static const hal_power_t power={.battery_mv=battery,.switch_on=switched};
const board_t board={.native_modem=&ops,.power=&power,.battery_critical_mv=6500};
void radio_sm_request(radio_sm_t *sm,const radio_req_t *r,uint32_t now)
{ (void)sm;(void)now;assert(r->type==RREQ_RETUNE);retunes++; }
static uint8_t reply[512];
static uint32_t get(const uint8_t *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void put(uint8_t *p,uint32_t x) { for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(x>>(i*8)); }
static size_t call(const uint8_t *p,size_t n,size_t cap) { memset(reply,0xee,sizeof reply);size_t r=native_lab_command(p,n,reply,cap);assert(!depth);return r; }
static uint32_t arm(unsigned mode)
{
    uint8_t b[17]={1,(uint8_t)mode,1,3,0,0xd0,7,250,0};put(b+9,145500000);put(b+13,0x3532504e);
    assert(call(b,17,512)==5 && reply[0]==0);return get(reply+1);
}
static int op(unsigned cmd,uint32_t sid) { uint8_t b[5]={(uint8_t)cmd};put(b+1,sid);call(b,5,512);return reply[0]; }
static void load(uint32_t sid,unsigned ix)
{ uint8_t p[43]={2};put(p+1,sid);p[5]=(uint8_t)ix;assert(call(p,43,512)==1 && reply[0]==0); }
static uint32_t status(unsigned ix)
{ uint8_t p=0;assert(call(&p,1,512)==1+4*NT_STATUS_WORDS && reply[0]==0);return get(reply+1+ix*4); }
static radio_sm_t sm;
static void fresh(void)
{
    native_lab_init();starts=offs=restores=retunes=0;live=NULL;clock_us=0;busy=beeping=stop_during_start=replace_arm=0;on=1;volts=8000;
    memset(&sm,0,sizeof sm);sm.view.state=RS_IDLE;sm.view.ch.mode=RADIO_MODE_FM;
    sm.view.ch.rx_hz=sm.view.ch.tx_hz=145500000;
}
int main(void)
{
    fresh();assert(!native_lab_wait_ms());assert(!call(NULL,0,0));assert(call(NULL,0,512)==1 && reply[0]==2);
    uint8_t diag=8;assert(call(&diag,1,512)==1 && reply[0]==4);
    ops.snapshot=diagnostic;
    assert(call(&diag,1,224)==1 && reply[0]==2 && !snapshots);
    assert(call(&diag,1,225)==225 && !reply[0] && get(reply+1)==1 && get(reply+221)==56 && snapshots==1);
    ops.snapshot=NULL;
    uint8_t s=0;assert(call(&s,1,2)==1 && reply[0]==2);
    uint32_t sid=arm(NT_TRANSMIT);assert(native_lab_wait_ms()==2);assert(native_lab_reserved() && !native_lab_owns() && !starts);
    assert(op(3,sid)==7);load(sid,0);load(sid,1);assert(op(3,sid)==0 && !starts);
    native_lab_service(&sm,clock_us);assert(starts==1 && native_lab_owns() && live->state==NT_TX);
    assert(op(5,sid+1)==7 && !offs);assert(op(5,sid)==0 && offs==1 && native_lab_owns());
    assert(op(7,sid)==7);native_lab_service(&sm,clock_us);assert(!native_lab_owns() && restores==1 && retunes==1);
    assert(status(1)==NT_STOPPED && status(26)==1 && status(17)==0);
    assert(op(5,sid)==0 && offs==1);
    sid=arm(NT_RECEIVE);assert(op(3,sid)==0);native_lab_service(&sm,0);
    uint8_t raw[36]={0};clock_us=1000;nt_rx(live,clock_us,0x800,3,raw);
    uint8_t read[5]={6};put(read+1,sid);assert(call(read,5,20)==1 && reply[0]==2 && live->rx_count==1);
    assert(call(read,5,512)==53 && reply[0]==0 && get(reply+13)==3 && live->rx_count==0);
    radio_req_t r={.type=RREQ_PTT,.arg=1};assert(native_lab_request(&sm,&r));
    assert(!native_lab_reserved() && live->reason==NT_INTERRUPTED && restores==2);
    assert(!native_lab_request(&sm,&r)); /* normal PTT routing after ownership released */
    for(unsigned deny=0;deny<8;deny++) {
        fresh();sid=arm(NT_TRANSMIT);load(sid,0);load(sid,1);
        switch(deny) {
        case 0:sm.ptt=1;break;case 1:sm.view.ch.mode=RADIO_MODE_DMR;break;
        case 2:sm.view.ch.rx_only=1;break;case 3:sm.set.tx_inhibit=1;break;
        case 4:volts=6000;break;case 5:sm.view.ch.tx_hz++;break;
        case 6:sm.scan.kind=SCAN_ZONE;break;case 7:beeping=1;break;
        }
        assert(op(3,sid)==0);native_lab_service(&sm,0);assert(!starts && status(2)==NT_DENIED && status(1)==NT_FAULT);
    }
    fresh();busy=1;uint8_t p[17]={1,1,0,3,0,0xd0,7,250,0};put(p+9,145500000);
    assert(call(p,17,512)==1 && reply[0]==7 && !starts);
    fresh();sid=arm(NT_RECEIVE);assert(op(3,sid)==0);native_lab_service(&sm,0);
    clock_us=26000;on=0;native_lab_service(&sm,0);assert(offs==1 && restores==1 && status(1)==NT_FAULT);
    /* Newer IRQ timestamp than service's argument must not underflow into stall. */
    fresh();sid=arm(NT_TRANSMIT);load(sid,0);load(sid,1);assert(op(3,sid)==0);native_lab_service(&sm,0);
    clock_us=3000;nt_boundary(live,NT_IRQ_PHY,clock_us);native_lab_service(&sm,1000);
    assert(live->state==NT_TX && !offs);
    clock_us=250000;nt_pa_ready(live,clock_us);assert(live->reason==NT_LEASE && offs==1);
    native_lab_service(&sm,clock_us);assert(restores==1 && !native_lab_owns());
    fresh();sid=arm(NT_RECEIVE);assert(op(3,sid)==0);stop_during_start=1;native_lab_service(&sm,0);
    assert(live->state==NT_STOPPED && live->reason==NT_USER_STOP && offs==2 && restores==1 && !native_lab_owns());
    /* A stale start's admission rejection must not fault a newly armed session. */
    fresh();sid=arm(NT_RECEIVE);assert(op(3,sid)==0);replace_sid=sid;replace_arm=1;
    native_lab_service(&sm,0);assert(status(1)==NT_ARMED && status(3)==replacement_sid && replacement_sid!=sid && !starts);
    assert(op(3,replacement_sid)==0);native_lab_service(&sm,0);assert(starts==1 && live->state==NT_RX);
    assert(op(5,replacement_sid)==0);native_lab_service(&sm,0);assert(restores==1);
    /* Local audio owns its session; cable mutators cannot steal/drain it.
     * Unlike the raw bench, RX may take over an already open FM squelch. */
    fresh();busy=1;sm.view.state=RS_RX;
    sid=native_lab_voice_start(145500000,60000);assert(sid && native_lab_reserved() && native_lab_wait_ms()==5);
    assert(op(3,sid)==DL_ST_BUSY);native_lab_service(&sm,0);
    assert(live->state==NT_RX && live->config.audio && live->config.duration_ms==60000);
    uint8_t vst,why;
    clock_us=400000;assert(native_lab_voice_poll(sid,&vst,&why) && vst==NT_RX);
    nt_rx(live,410000,0x800,0,raw);nt_record vr;
    assert(op(6,sid)==DL_ST_BUSY && native_lab_voice_read(sid,&vr) && vr.sequence==0);
    /* Shared-jack PTT down/up no longer cancels RX audio or changes sm.ptt. */
    assert(native_lab_request(&sm,&r) && live->state==NT_RX && !sm.ptt);
    r.arg=0;assert(native_lab_request(&sm,&r) && live->state==NT_RX && !sm.ptt);
    uint8_t events=9;assert(call(&events,1,32)==1 && reply[0]==DL_ST_BAD_ARG);
    assert(call(&events,1,33)==33 && !reply[0] && get(reply+17)==2 && get(reply+21)==0);
    radio_req_t change={.type=RREQ_RETUNE};
    assert(!native_lab_request(&sm,&change) && live->reason==NT_INTERRUPTED);
    assert(call(&events,1,33)==33 && get(reply+21)==1 && get(reply+25)==RREQ_RETUNE);
    assert(native_lab_reserved());assert(native_lab_voice_release(sid));assert(!native_lab_reserved() && !native_lab_wait_ms());
    /* A worker that stops renewing its lease shuts down independently. */
    fresh();sid=native_lab_voice_start(145500000,60000);native_lab_service(&sm,0);
    clock_us=501000;nt_tick(live,clock_us);native_lab_service(&sm,clock_us);
    assert(live->reason==NT_LEASE && !live->owned && !live->keyed);
    assert(native_lab_voice_release(sid) && !native_lab_reserved());
    puts("PASS native cable protocol/radio ownership: parsing, capacities, sessions, prefill, stop, capture, PTT, eight admission gates, battery loss, timestamp/start-stop races");
    return 0;
}
