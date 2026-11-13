/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Actual controller/lab/grant with the existing board/scheduler fixtures. */
#define main native_fixture_main
#include "test_lab.c"
#undef main
#include "firstboot/firstboot.h"
static bool update_active;
static unsigned live_press,live_release;
static uint32_t local_sid, rx_sid;
int p25_lab_live_begin(uint32_t sid) { local_sid=sid;live_press++;return 1; }
int p25_lab_channel_rx(uint32_t hz) { rx_sid=native_lab_voice_start(hz,0);return rx_sid!=0; }
void p25_lab_live_up(void) { live_release++; }
bool firstboot_update_busy(void) { return update_active; }
void firstboot_update_cancel(void) { update_active=false; }
uint32_t firstboot_irq_save(void) { mock_enter();return 0; }
void firstboot_irq_restore(uint32_t irq) { (void)irq;mock_exit(); }
static int granted_start(void *ctx,nt_test *t)
{ if (t->config.mode==NT_TRANSMIT) assert(firstboot_native_tx_open(t));return hw_start(ctx,t); }
static void revoked_quiet(void *ctx)
{ firstboot_native_tx_close();quiet(ctx); }
static uint32_t queue_tx(void)
{ uint32_t sid=arm(NT_TRANSMIT);load(sid,0);load(sid,1);assert(op(3,sid)==0);return sid; }
int main(void)
{
    ops.start=granted_start;ops.quiet=revoked_quiet;
    fresh();queue_tx();native_lab_service(&sm,0);
    assert(firstboot_tx_allowed(FB_BOARD_TX) && native_lab_owns());
    radio_req_t ptt={.type=RREQ_PTT,.arg=1};assert(native_lab_request(&sm,&ptt));
    assert(live->reason==NT_INTERRUPTED && !native_lab_owns() && !firstboot_tx_allowed(FB_BOARD_TX));
    fresh();queue_tx();volts=6000;native_lab_service(&sm,0);
    assert(!starts && !firstboot_tx_allowed(FB_BOARD_TX));
    fresh();queue_tx();native_lab_service(&sm,0);
    volts=6000;clock_us=25001;native_lab_service(&sm,clock_us);
    assert(live->reason==NT_DENIED && !live->owned && !firstboot_tx_allowed(FB_PA_KEY));
    fresh();queue_tx();stop_during_start=1;native_lab_service(&sm,0);
    assert(live->state==NT_STOPPED && offs==2 && !firstboot_tx_allowed(FB_BOARD_TX));
    fresh();queue_tx();sm.set.tx_inhibit=true;native_lab_service(&sm,0);
    assert(!starts && !firstboot_tx_allowed(FB_BOARD_TX));
    fresh();queue_tx();sm.view.ch.tx_hz++;native_lab_service(&sm,0);
    assert(!starts && !firstboot_tx_allowed(FB_BOARD_TX));
    fresh();queue_tx();native_lab_service(&sm,0);clock_us=250000;
    nt_pa_ready(live,clock_us);assert(live->state==NT_FAULT && live->reason==NT_LEASE);
    assert(!firstboot_tx_allowed(FB_FD_TX));native_lab_service(&sm,clock_us);assert(!live->owned);
    fresh();update_active=true;uint8_t arm_req[17]={1,2,1,3,0,0xd0,7,250,0};
    put(arm_req+9,145500000);put(arm_req+13,0x3532504e);
    assert(call(arm_req,17,512)==1 && reply[0]==DL_ST_BUSY && !starts);
    update_active=false;
    fresh();
    ptt.arg=NATIVE_PHYSICAL_PTT_DOWN;
    assert(native_lab_request(&sm,&ptt) && !live_press && !starts);
    uint8_t enable[]={10,1},raw[36]={0};
    assert(call(enable,2,512)==1 && !reply[0]);
    native_lab_service(&sm,clock_us);native_lab_service(&sm,clock_us);
    assert(rx_sid && starts==1 && live->state==NT_RX && !live->config.duration_ms);
    ptt.arg=1;
    assert(native_lab_request(&sm,&ptt) && !live_press && live->state==NT_RX); /* host down */
    for (unsigned call_no=1;call_no<=2;call_no++) {
        ptt.arg=NATIVE_PHYSICAL_PTT_DOWN;
        assert(native_lab_request(&sm,&ptt) && live->state==NT_STOPPED);
        native_lab_service(&sm,clock_us);assert(native_lab_voice_release(rx_sid));
        native_lab_service(&sm,clock_us);assert(live_press==call_no && native_lab_owns());
        assert(native_lab_live_block(local_sid,raw,0) && native_lab_live_block(local_sid,raw,0));
        assert(native_lab_live_go(local_sid));native_lab_service(&sm,clock_us);
        assert(live->state==NT_TX && !live->config.duration_ms && !live->config.blocks);
        uint8_t trace_op=11, page[7]={11};
        put(page+1,local_sid);
        assert(call(page,7,512)==1 && reply[0]==DL_ST_REFUSED);
        if (call_no==1) nt_rf_tx(live,clock_us); /* one RF edge arrived too early */
        uint8_t state,reason,queued;
        for (unsigned block=0;block<201;block++) {
            clock_us+=30000;
            assert(native_lab_live_poll(local_sid,&state,&reason,&queued) && state==NT_TX);
            nt_boundary(live,NT_IRQ_PHY,clock_us);
            if (!block && call_no==2) {
                nt_rf_tx(live,clock_us+1000);
                nt_pa_ready(live,clock_us+29000);
            }
            assert(native_lab_live_block(local_sid,raw,0));
        }
        ptt.arg=NATIVE_PHYSICAL_PTT_UP;
        assert(native_lab_request(&sm,&ptt) && live_release==call_no);
        assert(native_lab_live_block(local_sid,raw,1) && !native_lab_live_block(local_sid,raw,0));
        for (unsigned block=0;block<3;block++) { clock_us+=30000;nt_boundary(live,NT_IRQ_PHY,clock_us); }
        assert(live->state==NT_DRAIN);
        clock_us+=50000;native_lab_service(&sm,clock_us);
        assert(live->reason==NT_TX_COMPLETE && native_lab_voice_release(local_sid));
        native_lab_service(&sm,clock_us);native_lab_service(&sm,clock_us);
        assert(live->state==NT_RX && !retunes);
        assert(call(&trace_op,1,512)==1+4*NT_TRACE_WORDS && !reply[0]);
        assert(get(reply+5)==local_sid && get(reply+9)==1);
        assert(get(reply+17)==NT_TRACE_BLOCKS && get(reply+25)==204);
        assert(get(reply+29)==(call_no==2) && get(reply+33)==1);
        assert(get(reply+45)==(call_no==2) && get(reply+57)==(call_no==2));
        assert(call(page,7,1+NT_TRACE_BLOCK_BYTES)==1+NT_TRACE_BLOCK_BYTES &&
               !reply[0] && get(reply+5)==2 && !memcmp(reply+9,raw,sizeof raw));
        page[5]=NT_TRACE_BLOCKS;
        assert(call(page,7,512)==1 && reply[0]==DL_ST_REFUSED);
    }
    enable[1]=0;assert(call(enable,2,512)==1 && !reply[0]);
    native_lab_service(&sm,clock_us);assert(native_lab_voice_release(rx_sid));
    assert(!native_lab_owns() && retunes==1);
    assert(!depth);
    puts("Native TX ownership: PTT cancels, battery/admission failures, setup interruption, lease and updater exclusion PASS");
}
