/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "native_lab.h"
#include "board.h"
#include "hal_time.h"
#include "p25/p25_lab.h"
#if CONFIG_FIRSTBOOT
#include "firstboot/firstboot.h"
#endif
#include "radio/beep.h"
#include "debuglink/debuglink_proto.h"
#include "FreeRTOS.h"
#include "task.h"
static nt_test test;
static uint32_t pending_start, battery_at;
static uint32_t voice_owner;
/* The digital voice channel the radio core selected (native_lab_channel):
 * receive between calls, transmit while its PTT is down. */
static native_channel chan;
static bool live_enabled, live_rx_pending, live_tx_pending;
static uint32_t live_summary[5]; /* count, stop reason, blocks sent, PA keys, faults */
#if CONFIG_DEV_TX
/* Laboratory gate: TX also needs the cable's enable this boot, 145.500 MHz
 * and the lowest power (firstboot_native_tx_config). */
static bool lab_armed;
#endif
static int live_owner(void) { return voice_owner && test.config.mode==NT_TRANSMIT && test.config.audio; }
static uint32_t last_req_type,last_req_arg,last_req_ms,ptt_consumed,cancellations,cancel_type,cancel_arg;
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0]|p[1]<<8); }
static void put(uint8_t *p, uint32_t x) { for (unsigned i=0;i<4;i++) p[i]=(uint8_t)(x>>(i*8)); }
static void deny(uint32_t sid)
{
    taskENTER_CRITICAL();
    if (test.session==sid) nt_deny(&test);
    taskEXIT_CRITICAL();
}
void native_lab_init(void) {
    pending_start=battery_at=voice_owner=0;
    live_enabled=live_rx_pending=live_tx_pending=false;
    for (unsigned i=0;i<5;i++) live_summary[i]=0;
    chan=(native_channel){0};
#if CONFIG_DEV_TX
    lab_armed=false;
#endif
    last_req_type=last_req_arg=last_req_ms=ptt_consumed=cancellations=cancel_type=cancel_arg=0;
    nt_init(&test,board.native_modem);
}
static int updater(void) {
#if CONFIG_FIRSTBOOT
    return firstboot_update_busy();
#else
    return 0;
#endif
}
int native_lab_reserved(void) {
    taskENTER_CRITICAL();
    int r=updater() || voice_owner || test.owned || test.state==NT_ARMED || pending_start || live_enabled;
    taskEXIT_CRITICAL();return r;
}
uint32_t native_lab_wait_ms(void)
{
    taskENTER_CRITICAL();
    uint32_t ms=voice_owner?5u:(test.owned || test.state==NT_ARMED || pending_start)?2u:0u;
    if (live_enabled && !ms) ms=5;
    taskEXIT_CRITICAL();return ms;
}
/* The radio core keeps running while its own channel owns the modem: it
 * holds the policy (PTT, time-out, busy lockout, scan). Bench sessions and
 * the updater still stop it. */
int native_lab_owns(void) {
    taskENTER_CRITICAL();
    int r=updater() || (test.owned && !live_enabled);
    taskEXIT_CRITICAL();
    return r;
}
static int battery_ok(void)
{
    if (!board.power || !board.power->switch_on || !board.power->switch_on()) return 0;
    if (test.config.mode!=NT_TRANSMIT) return 1;
    if (!board.power->battery_mv) return 0;
    uint16_t mv=board.power->battery_mv();
    return mv && (!board.battery_critical_mv || mv>board.battery_critical_mv);
}
static int eligible(const radio_sm_t *sm)
{
    const radio_view_t *v=&sm->view;
    if (!board.native_modem || sm->hotspot || v->power_off || !battery_ok()) return 0;
    if (live_enabled && voice_owner) {
        /* The radio core's channel: it already applied its TX rules. */
        if (!codeplug_mode_digital_voice(v->ch.mode)) return 0;
        if (test.config.mode==NT_TRANSMIT)
            return chan.tx_hz && test.config.frequency_hz==chan.tx_hz && !v->ch.rx_only && !sm->set.tx_inhibit;
        return test.config.frequency_hz==chan.rx_hz;
    }
    if ((p25_lab_busy() && !voice_owner) || sm->ptt ||
        (v->state!=RS_IDLE && !(voice_owner && v->state==RS_RX)) ||
        v->ch.mode!=RADIO_MODE_FM || v->am || v->bcast || beep_busy()) return 0;
#if CONFIG_SCAN
    if (scan_on(&sm->scan)) return 0;
#endif
    if (board.audio && board.audio->spk_pending && board.audio->spk_pending()) return 0;
    if (test.config.mode==NT_TRANSMIT && (v->ch.rx_only || sm->set.tx_inhibit ||
        v->ch.tx_hz!=test.config.frequency_hz)) return 0;
    return test.config.mode==NT_TRANSMIT || v->ch.rx_hz==test.config.frequency_hz;
}
static int lab_gate_tx(void)
{
#if CONFIG_DEV_TX
    return lab_armed && chan.tx_hz==145500000u && chan.power==0;
#else
    return 1;
#endif
}
/* Start the next session of the channel: TX while PTT is down, else RX. */
static void live_next(radio_sm_t *sm)
{
    taskENTER_CRITICAL();
    if (!live_enabled || voice_owner || pending_start || test.owned || p25_lab_busy()) {
        taskEXIT_CRITICAL();return;
    }
    bool tx=live_tx_pending, rx=live_rx_pending;
    if (tx && board.audio && board.audio->spk_pending && board.audio->spk_pending()) {
        taskEXIT_CRITICAL();return;
    }
    live_tx_pending=false;
    if (!tx) live_rx_pending=false;
    if ((tx || rx) && test.state==NT_FAULT) nt_reset(&test);
    uint32_t sid=0;
    if (tx && lab_gate_tx() && nt_arm_live(&test,chan.tx_hz,chan.power)) voice_owner=sid=test.session;
    else if (tx) { live_summary[0]++;live_summary[1]=NT_DENIED;live_summary[2]=0;live_summary[3]=0;live_summary[4]++; }
    taskEXIT_CRITICAL();
    if (sid && (!eligible(sm) || !p25_lab_live_begin(sid))) {
        deny(sid);
        native_lab_voice_release(sid);
    }
    if (!tx && rx) p25_lab_channel_rx(chan.rx_hz);
}
static void restore_channel(radio_sm_t *sm)
{
    sm->audio_out=sm->green_out=sm->red_out=-1;
    radio_req_t r={.type=RREQ_RETUNE};
    radio_sm_request(sm,&r,hal_millis());
}
void native_lab_service(radio_sm_t *sm, uint32_t now)
{
    if(updater())return;
    now=hal_micros();
    taskENTER_CRITICAL();
    uint32_t sid=pending_start; pending_start=0;
    taskEXIT_CRITICAL();
    if (sid && test.state==NT_ARMED && sid==test.session) {
        if (eligible(sm)) {
            if (!nt_begin(&test,sid,now)) deny(sid);
            battery_at=now;
        }
        else deny(sid);
    }
    if (test.owned) {
        now=hal_micros(); /* setup may have taken time */
        if ((uint32_t)(now-battery_at)>=25000u) {
            battery_at=now;
            if (!battery_ok()) { taskENTER_CRITICAL(); nt_fault(&test,NT_DENIED); taskEXIT_CRITICAL(); }
        }
        /* Sample the clock with interrupts excluded: a newer IRQ timestamp
         * must not be compared against a stale task timestamp across setup. */
        taskENTER_CRITICAL(); nt_tick(&test,hal_micros()); taskEXIT_CRITICAL();
        if (nt_cleanup(&test)) {
            /* The channel's next session follows; the radio core keeps the
             * speaker and LEDs. A bench session hands the board back. */
            if (live_enabled) return;
            /* Board is back in FM; force a full retune and fresh speaker/LED decisions. */
            restore_channel(sm);
        }
    }
    live_next(sm);
}

int native_lab_channel(const native_channel *c)
{
    taskENTER_CRITICAL();
    if (c && (updater() || (!live_enabled && (test.owned || test.state==NT_ARMED || pending_start || voice_owner)))) {
        taskEXIT_CRITICAL();return 0; /* a bench session or the updater has the modem */
    }
    if (c && live_enabled && c->rx_hz==chan.rx_hz && c->tx_hz==chan.tx_hz && c->power==chan.power) {
        taskEXIT_CRITICAL();return 1; /* same channel: the running session stays */
    }
    bool was=live_enabled;
    live_enabled=c!=NULL;
    live_tx_pending=false;
    live_rx_pending=live_enabled;
    if (c) chan=*c;
    if (was && (voice_owner || test.owned)) { pending_start=0;nt_stop(&test,test.session,NT_USER_STOP); }
    taskEXIT_CRITICAL();
    /* Leaving: hand the board back now so the radio core can tune it. The
     * worker releases its session when it sees the stop. */
    if (!c && test.owned) nt_cleanup(&test);
    return 1;
}
int native_lab_channel_ptt(bool down)
{
    taskENTER_CRITICAL();
    int ok=live_enabled;
    if (ok && down) {
        if (!live_owner()) {
            live_tx_pending=live_rx_pending=true;
            if (voice_owner) { pending_start=0;nt_stop(&test,test.session,NT_USER_STOP); }
        }
    } else if (ok) {
        live_tx_pending=false;
        live_rx_pending=true;
        if (live_owner()) p25_lab_live_up();
    }
    taskEXIT_CRITICAL();
    return ok;
}
void native_lab_channel_state(native_channel_state *s)
{
    taskENTER_CRITICAL();
    s->enabled=live_enabled;
    s->tx_pending=live_tx_pending;
    s->transmitting=live_owner() && (test.state==NT_ARMED || test.state==NT_TX || test.state==NT_DRAIN || pending_start);
    s->keyed=live_owner() && test.keyed;
    s->receiving=voice_owner && !live_owner() && test.state==NT_RX;
    s->calls=live_summary[0];
    s->reason=(uint8_t)live_summary[1];
    s->blocks=live_summary[2];
    s->faults=live_summary[4];
    taskEXIT_CRITICAL();
}
int native_lab_request(radio_sm_t *sm, const radio_req_t *r)
{
    if(updater()) {
#if CONFIG_FIRSTBOOT
        if(r->type==RREQ_POWER_OFF) { firstboot_update_cancel();return 0; }
#endif
        return 1;
    }
#if CONFIG_DEV_TX
    /* The input task's duplicate physical PTT edges belong to the old bench path. */
    if (r->type==RREQ_PTT && (r->arg==NATIVE_PHYSICAL_PTT_DOWN || r->arg==NATIVE_PHYSICAL_PTT_UP)) return 1;
#endif
    /* On the radio core's own channel every request is the radio core's. */
    if (live_enabled) return 0;
    if (!native_lab_reserved()) return 0;
    taskENTER_CRITICAL();
    last_req_type=r->type;last_req_arg=(uint32_t)r->arg;last_req_ms=hal_millis();
    /* The programming plug shares PTT. RX audio never admits TX, so PTT
     * press/release are consumed rather than returning the receiver to FM. */
    if (voice_owner && r->type==RREQ_PTT) {
        if(ptt_consumed!=UINT32_MAX)ptt_consumed++;
        taskEXIT_CRITICAL();return 1;
    }
    if(cancellations!=UINT32_MAX)cancellations++;
    cancel_type=r->type;cancel_arg=(uint32_t)r->arg;
    taskEXIT_CRITICAL();
    taskENTER_CRITICAL(); pending_start=0; nt_stop(&test,test.session,NT_INTERRUPTED); taskEXIT_CRITICAL();
    native_lab_service(sm,hal_micros());
    return r->type==RREQ_PTT && r->arg!=0;
}
uint32_t native_lab_voice_start(uint32_t hz,uint32_t ms)
{
    taskENTER_CRITICAL();
    uint32_t sid=0;
    if (!updater() && board.native_modem && !voice_owner && !pending_start) {
        int armed=ms && nt_arm_voice(&test,hz,ms);
        if (!ms && live_enabled && !live_tx_pending) armed=nt_arm_live_rx(&test,hz);
        if (armed) pending_start=voice_owner=sid=test.session;
    }
    taskEXIT_CRITICAL();
    return sid;
}
int native_lab_voice_poll(uint32_t sid,uint8_t *state,uint8_t *reason)
{
    taskENTER_CRITICAL();
    int ok=sid && sid==voice_owner && sid==test.session;
    if (ok) {
        if (test.state==NT_RX) nt_ping(&test,sid,hal_micros());
        *state=test.state;*reason=test.reason;
    }
    taskEXIT_CRITICAL();return ok;
}
int native_lab_voice_read(uint32_t sid,nt_record *r)
{
    taskENTER_CRITICAL();int ok=sid && sid==voice_owner && nt_pop(&test,sid,r);taskEXIT_CRITICAL();return ok;
}
void native_lab_voice_stop(uint32_t sid)
{
    taskENTER_CRITICAL();
    if (sid && sid==voice_owner) { pending_start=0;nt_stop(&test,sid,NT_USER_STOP); }
    taskEXIT_CRITICAL();
}
int native_lab_voice_release(uint32_t sid)
{
    int released=0;
    taskENTER_CRITICAL();
    if (sid && sid==voice_owner && !test.owned && test.state!=NT_ARMED) {
        if (live_owner()) {
            live_summary[0]++;
            live_summary[1]=test.reason;
            live_summary[2]=test.stats.tx_sent;
            live_summary[3]=test.stats.pa_keys;
            live_summary[4]=test.stats.faults;
        }
        nt_record discard;
        while (nt_pop(&test,sid,&discard)) {}
        voice_owner=0;released=1;
    }
    taskEXIT_CRITICAL();return released;
}
void native_lab_live_summary(uint32_t out[5])
{
    taskENTER_CRITICAL();
    for (unsigned i=0;i<5;i++) out[i]=live_summary[i];
    taskEXIT_CRITICAL();
}
int native_lab_live_poll(uint32_t sid,uint8_t *state,uint8_t *reason,uint8_t *queued)
{
    taskENTER_CRITICAL();
    int ok=live_owner() && sid==voice_owner && sid==test.session;
    if (ok) {
        if (test.state==NT_TX || test.state==NT_DRAIN) nt_ping(&test,sid,hal_micros());
        *state=test.state;
        *reason=test.reason;
        *queued=test.tx_count;
    }
    taskEXIT_CRITICAL();
    return ok;
}
int native_lab_live_block(uint32_t sid,const uint8_t raw[36],int last)
{
    taskENTER_CRITICAL();
    int ok=live_owner() && sid==voice_owner && nt_load(&test,sid,test.stats.tx_loaded,raw);
    if (ok && last) ok=nt_live_seal(&test,sid);
    taskEXIT_CRITICAL();
    return ok;
}
int native_lab_live_go(uint32_t sid)
{
    taskENTER_CRITICAL();
    int ok=live_owner() && sid==voice_owner && sid==test.session &&
        test.state==NT_ARMED && test.tx_count>=nt_tx_required(&test.config) && !pending_start;
    if (ok) pending_start=sid;
    taskEXIT_CRITICAL();
    return ok;
}
/* Private 0x21 protocol v1. Fixed sizes; no raw register write command.
 * ARM is pure software, START executes only on the radio task. TX requires
 * the explicit 'NP25' guard; it is an opt-in marker, not authentication. */
size_t native_lab_command(const uint8_t *p, uint16_t n, uint8_t *out, size_t cap)
{
    if (!cap) return 0;
    out[0]=DL_ST_BAD_ARG;
    if (!n || !p) return 1;
    int ok=0;
    if (p[0]==0 && n==1) {
        if (cap<1+4*NT_STATUS_WORDS) return 1;
        uint32_t words[NT_STATUS_WORDS];
        taskENTER_CRITICAL(); nt_status(&test,words); taskEXIT_CRITICAL();
        for (unsigned i=0;i<NT_STATUS_WORDS;i++) put(out+1+4*i,words[i]);
        out[0]=DL_ST_OK; return 1+4*NT_STATUS_WORDS;
    }
    if(p[0]==9 && n==1) {
        if(cap<33)return 1;
        taskENTER_CRITICAL();
        uint32_t words[]={1,last_req_type,last_req_arg,last_req_ms,ptt_consumed,cancellations,cancel_type,cancel_arg};
        for(unsigned i=0;i<8;i++)put(out+1+4*i,words[i]);
        taskEXIT_CRITICAL();out[0]=DL_ST_OK;return 33;
    }
    if (!board.native_modem) { out[0]=DL_ST_UNAVAILABLE; return 1; }
    if (p[0]==8 && n==1) {
        if (!board.native_modem->snapshot) { out[0]=DL_ST_UNAVAILABLE; return 1; }
        if (cap<1+4*NT_SNAPSHOT_WORDS)return 1;
        uint32_t words[NT_SNAPSHOT_WORDS];
        taskENTER_CRITICAL();
        size_t count=board.native_modem->snapshot(board.native_modem->ctx,words,NT_SNAPSHOT_WORDS);
        taskEXIT_CRITICAL();
        if (count!=NT_SNAPSHOT_WORDS) { out[0]=DL_ST_UNAVAILABLE; return 1; }
        for (unsigned i=0;i<count;i++)put(out+1+4*i,words[i]);
        out[0]=DL_ST_OK;return 1+4*count;
    }
#if CONFIG_DEV_TX
    if (p[0]==11 && n==1 && cap>=1+4*NT_TRACE_WORDS) {
        uint32_t words[NT_TRACE_WORDS];
        taskENTER_CRITICAL();nt_trace_summary(words);taskEXIT_CRITICAL();
        for (unsigned i=0;i<NT_TRACE_WORDS;i++) put(out+1+4*i,words[i]);
        out[0]=DL_ST_OK;return 1+4*NT_TRACE_WORDS;
    }
    if (p[0]==11 && n==7) {
        taskENTER_CRITICAL();
        size_t count=nt_trace_read(u32(p+1),u16(p+5),out+1,cap-1);
        taskEXIT_CRITICAL();
        out[0]=count?DL_ST_OK:DL_ST_REFUSED;
        return 1+count;
    }
    if (p[0]==10 && n==2 && p[1]<=1) {
        /* Laboratory gate: arm or disarm P25 TX for this boot. RAM only. */
        taskENTER_CRITICAL();
        lab_armed=p[1]!=0;
        if (!lab_armed && live_owner()) { pending_start=0;nt_stop(&test,test.session,NT_USER_STOP); }
        taskEXIT_CRITICAL();
        out[0]=DL_ST_OK;
        return 1;
    }
    if (live_enabled) { out[0]=DL_ST_BUSY;return 1; }
#endif
    if (voice_owner || updater()) { out[0]=DL_ST_BUSY;return 1; }
    if (p[0]==1 && n==17 && cap>=5) {
        nt_config c={.mode=p[1],.p25_sync=p[2],.blocks=u16(p+3),.duration_ms=u16(p+5),
                     .lease_ms=u16(p+7),.frequency_hz=u32(p+9)};
        taskENTER_CRITICAL();
        ok=!p25_lab_busy() && !pending_start && nt_arm(&test,&c,u32(p+13)==0x3532504eu);
        if (ok) put(out+1,test.session);
        taskEXIT_CRITICAL();
        out[0]=ok?DL_ST_OK:DL_ST_REFUSED; return ok?5:1;
    }
    if (n<5) return 1;
    uint32_t sid=u32(p+1);
    taskENTER_CRITICAL();
    if (p[0]==2 && n==43) ok=nt_load(&test,sid,u16(p+5),p+7);
    else if (p[0]==3 && n==5) {
        ok=sid==test.session && test.state==NT_ARMED && !pending_start &&
            (test.config.mode==NT_RECEIVE || test.tx_count>=nt_tx_required(&test.config));
        if (ok) pending_start=sid;
    } else if (p[0]==4 && n==5) ok=nt_ping(&test,sid,hal_micros());
    else if (p[0]==5 && n==5) { ok=nt_stop(&test,sid,NT_USER_STOP); if (ok) pending_start=0; }
    else if (p[0]==6 && n==5 && cap>=53) {
        nt_record r;
        ok=nt_pop(&test,sid,&r);
        if (ok) {
            put(out+1,r.sequence); put(out+5,r.time_us); put(out+9,r.sys_flags); put(out+13,r.rx_info);
            for (unsigned i=0;i<36;i++) out[17+i]=r.raw[i];
            taskEXIT_CRITICAL(); out[0]=DL_ST_OK; return 53;
        }
    } else if (p[0]==7 && n==5 && sid==test.session) ok=nt_reset(&test);
    else { taskEXIT_CRITICAL(); return 1; }
    taskEXIT_CRITICAL();
    out[0]=ok?DL_ST_OK:DL_ST_REFUSED; return 1;
}
