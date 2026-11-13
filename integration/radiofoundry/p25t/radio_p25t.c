/* SPDX-License-Identifier: GPL-3.0-or-later */
/* radio_p25t.h: a P25 trunking channel on the radio. */
#include "radio_p25t.h"
#include "config.h"
#if CONFIG_P25_TRUNK
#include "hal_time.h"
#include "native_modem/native_lab.h"
#include "p25_lab.h"
#include "su.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

/* What the control channel decoder (in the P25 worker task) hands the radio task */
enum { EV_TSBK, EV_STATUS, EV_RING = 32 };
typedef struct {
    uint8_t type, status;
    uint16_t nac;
    uint8_t tsbk[P25T_TSBK_BYTES];
} event;

static event ring[EV_RING];
static volatile uint8_t ring_head, ring_count;
static uint32_t ring_drops;

static p25t_rx cc;                  /* worker task */
static p25t_su su;                  /* radio task */
static radio_dv_cfg_t chan_cfg;
static native_channel nch;
static bool selected, requesting;
static uint32_t tx_calls;           /* PTTs that ended without a call (refused, no answer, released while waiting) */
static uint32_t native_calls_seen;  /* our voice calls the native layer ended */
static uint32_t frames_seen;
static uint8_t tx_reason;           /* how the last of either ended */

static void push(const event *e)
{
    taskENTER_CRITICAL();
    if (ring_count < EV_RING) {
        ring[(ring_head + ring_count) % EV_RING] = *e;
        ring_count++;
    } else {
        ring_drops++;
    }
    taskEXIT_CRITICAL();
}

static void on_tsbk(void *ctx, uint16_t nac, const uint8_t t[P25T_TSBK_BYTES])
{
    (void)ctx;
    event e = { .type = EV_TSBK, .nac = nac };
    memcpy(e.tsbk, t, sizeof e.tsbk);
    push(&e);
}

static void on_status(void *ctx, uint8_t status, uint32_t at)
{
    (void)ctx;
    (void)at;
    event e = { .type = EV_STATUS, .status = status };
    push(&e);
}

/* Every dibit the native receive session hears (P25 worker task) */
static void tap(void *ctx, uint8_t dibit)
{
    (void)ctx;
    if (dibit == P25_PHY_TAP_GAP) p25t_rx_reset(&cc);
    else p25t_rx_dibit(&cc, dibit);
}

static void end_request(uint8_t reason)
{
    if (!requesting) return;
    requesting = false;
    tx_reason = reason;
    tx_calls++;
}

static uint8_t deny_reason(uint8_t deny)
{
    switch (deny) {
    case P25T_SU_DENY_NO_TX: return RADIO_DV_TRUNK_NO_TX;
    case P25T_SU_DENY_NO_RESPONSE:
    case P25T_SU_DENY_QUEUE_TIMEOUT: return RADIO_DV_TRUNK_NO_ANSWER;
    default: return RADIO_DV_TRUNK_REFUSED;   /* the system refused, or we are not on it yet */
    }
}

static void tune(uint32_t rx_hz, uint32_t tx_hz, uint16_t group, uint32_t source)
{
    p25_lab_channel c = { P25_NAC_ANY, su.cc_nac, group, source, true };
    p25_lab_channel_config(&c);
    nch.rx_hz = rx_hz;
    nch.tx_hz = tx_hz;
    native_lab_channel(&nch);
}

static void apply(const p25t_su_out *o)
{
    if (o->voice_tx_start) {
        /* granted: our group call on the voice channel's inbound frequency (FDMA) */
        tune(o->rx_hz, o->tx_hz, o->voice_group, o->voice_source);
        requesting = false;
        native_lab_channel_ptt(true);
        return;
    }
    if (o->tune) {
        /* the control channel, or a call followed (every group the logic follows plays) */
        bool cc_tune = su.state == P25T_SU_HUNT || su.state == P25T_SU_CC;
        tune(o->rx_hz, cc_tune ? su.cc_tx_hz : 0, chan_cfg.talkgroup, chan_cfg.unit_id);
        p25t_rx_reset(&cc);
    }
    if (o->voice_tx_stop) native_lab_channel_ptt(false);
#if CONFIG_P25_TRUNK_TX
    if (o->send_isp) {
        static uint8_t frame[P25T_TSDU_MAX_BYTES], dibits[P25T_TSDU_MAX_BYTES * 4];
        unsigned bits = p25t_tsdu_encode(o->nac, (const uint8_t(*)[P25T_TSBK_BYTES])o->isp, 1, P25T_STATUS_UNKNOWN, frame);
        for (unsigned i = 0; i < bits / 2; i++) dibits[i] = (uint8_t)((frame[i / 4] >> (6 - 2 * (i % 4))) & 3u);
        if (nch.tx_hz != o->tx_hz) {
            nch.tx_hz = o->tx_hz;
            native_lab_channel(&nch);
        }
        /* refused (a session still running): the logic retries once its response time passes */
        if (p25_lab_inbound_load(dibits, (uint16_t)(bits / 2))) native_lab_channel_inbound();
    }
#endif
}

bool radio_p25t_select(const radio_dv_cfg_t *cfg)
{
    if (!cfg) {
        /* No de-registration on the way out: the board goes back to FM before this returns, so nothing can be sent;
         * the system ages the registration out */
        p25_lab_set_tap(NULL, NULL);
        native_lab_channel(NULL);
        selected = false;
        return true;
    }
    chan_cfg = *cfg;
    p25t_su_cfg c = { 0 };
    c.wacn = cfg->wacn;
    c.sysid = cfg->sysid;
    c.unit_id = cfg->unit_id;
    c.nac = cfg->nac_rx == RADIO_DV_NAC_ANY ? P25_NAC_ANY : cfg->nac_rx;
    c.cc_hz[0] = cfg->rx_hz;
    c.cc_count = 1;
    c.talkgroup = cfg->talkgroup;
    c.hear_all = cfg->hear_all;
    c.tx_enabled = CONFIG_P25_TRUNK_TX != 0;   /* p25t_su_init also wants the whole identity */
    c.tdma_rx = c.tdma_tx = false;             /* no Phase 2 physical layer on this radio */
    taskENTER_CRITICAL();
    ring_head = ring_count = 0;
    taskEXIT_CRITICAL();
    p25t_rx_init(&cc, P25_NAC_ANY, on_tsbk, NULL, on_status, NULL);
    nch = (native_channel){ cfg->rx_hz, cfg->tx_hz, cfg->power };
    requesting = false;
    native_channel_state ch;
    native_lab_channel_state(&ch);
    native_calls_seen = ch.calls;
    p25t_su_out o;
    p25t_su_init(&su, &c, hal_millis(), hal_micros() | 1u, &o);
    p25_lab_set_tap(tap, NULL);
    selected = true;
    apply(&o);
    return true;
}

bool radio_p25t_ptt(bool down)
{
    if (!selected) return false;
    p25t_su_out o;
    p25t_su_ptt(&su, down, &o);
    if (down) {
        requesting = true;
        if (su.deny != P25T_SU_DENY_NONE) end_request(deny_reason(su.deny));   /* refused at once */
    } else if (requesting) {
        end_request(0);
    }
    apply(&o);
    return true;
}

void radio_p25t_service(uint32_t now_ms)
{
    if (!selected) return;
    p25t_su_out o;
    for (;;) {
        event e;
        taskENTER_CRITICAL();
        bool have = ring_count != 0;
        if (have) {
            e = ring[ring_head];
            ring_head = (uint8_t)((ring_head + 1) % EV_RING);
            ring_count--;
        }
        taskEXIT_CRITICAL();
        if (!have) break;
        if (e.type == EV_TSBK) p25t_su_tsbk(&su, e.nac, e.tsbk, &o);
        else p25t_su_status(&su, e.status, &o);
        apply(&o);
    }
    /* frames on the voice channel followed: the call's group, source, encryption, its end */
    p25_lab_rx_status rx;
    p25_lab_rx_status_get(&rx);
    if (rx.frames != frames_seen) {
        frames_seen = rx.frames;
        if (su.state == P25T_SU_VOICE_RX) {
            p25t_su_voice(&su, rx.talkgroup, rx.source, rx.encrypted, rx.ended, &o);
            apply(&o);
        }
    }
    native_channel_state ch;
    native_lab_channel_state(&ch);
    if (ch.calls != native_calls_seen) {
        native_calls_seen = ch.calls;
        tx_reason = ch.reason;
    }
    uint8_t before = su.state;
    p25t_su_tick(&su, now_ms, &o);
    apply(&o);
    if (requesting && su.deny != P25T_SU_DENY_NONE) end_request(deny_reason(su.deny));
    if (before == P25T_SU_VOICE_TX && su.state != P25T_SU_VOICE_TX) requesting = false;
}

void radio_p25t_status(radio_dv_status_t *s, uint32_t now_ms)
{
    p25_lab_rx_status rx;
    native_channel_state ch;
    p25_lab_rx_status_get(&rx);
    native_lab_channel_state(&ch);
    bool on_cc = su.state == P25T_SU_CC || su.state == P25T_SU_REQUEST || su.state == P25T_SU_QUEUED;
    bool voice = su.state == P25T_SU_VOICE_RX;
    bool recent = rx.last_frame_ms && now_ms - rx.last_frame_ms < RADIO_DV_BUSY_MS;
    s->active = selected;
    s->sync = (on_cc && now_ms - su.last_tsbk_ms < RADIO_DV_BUSY_MS) || (voice && recent);
    s->sync_ours = s->sync;
    s->playing = voice && recent && rx.playing && !rx.ended && !su.call_encrypted;
    s->encrypted = voice && su.call_encrypted;
    s->emergency = false;
    s->nac = su.cc_nac;
    s->talkgroup = voice || su.state == P25T_SU_VOICE_TX ? su.call_group : 0;
    s->source = voice || su.state == P25T_SU_VOICE_TX ? su.call_source : 0;
    /* PTT's request (on the control channel), then our call on the voice channel */
    s->transmitting = requesting || su.state == P25T_SU_REQUEST || su.state == P25T_SU_QUEUED ||
                      (su.state == P25T_SU_VOICE_TX && (ch.transmitting || ch.tx_pending));
    s->keyed = su.state == P25T_SU_VOICE_TX && ch.keyed;
    s->tx_calls = tx_calls + ch.calls;
    s->tx_reason = tx_reason;
    s->tx_blocks = ch.blocks;
    s->trunk_state = su.state;
    s->trunk_registered = su.registered;
    s->trunk_affiliated = su.affiliated;
}
#else
typedef int radio_p25t_unused;
#endif
