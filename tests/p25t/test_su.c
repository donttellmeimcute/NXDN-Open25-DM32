/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The trunked subscriber logic (src/p25t/su.c) against a simulated site
 * (site.c), over the air as symbols: hunting the control channel,
 * registration, affiliation, following group calls to voice channels and
 * back, group calls on PTT (grant, queued, acknowledged, denied, no answer),
 * several units contending for the inbound channel, a lost control channel,
 * de-registration, and the transmit safeguards.
 *
 * Each radio here is half duplex: deaf while it transmits and while its
 * synthesizer settles after a retune or a transmission, with a key-up delay
 * before its first symbol. Its receivers are the ones the radio runs:
 * p25t_rx on the control channel, the P25 framer and frame decoder on a
 * voice channel. On a Phase 2 channel (a site with TDMA voice channels) it
 * runs src/p25p2's receiver on its slot and sends its inbound bursts in its
 * slot's positions of the superframe it receives, deaf during each; the
 * radio itself has no Phase 2 physical layer (docs/p25-phase2.md), so this
 * covers everything above it. */
#include "p2_call.h"
#include "site.h"
#include "su.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum {
    KEYUP_SYMBOLS = 24,             /* 5 ms from key-up to the first symbol */
    SETTLE_SYMBOLS = 24,            /* 5 ms deaf after a retune or a transmission */
    WACN = 0xBEE00, SYSID = 0x3A5, NAC = 0x293, GROUP = 100,
};
#define WRONG_CC_HZ 853012500u

typedef struct {
    p25t_su su;
    p25t_rx cc;
    p25_rx vrx;
    uint32_t rx_hz;
    uint32_t ms;
    uint32_t rnd;
    /* transmitter */
    uint8_t tx[P25_LDU_BYTES * 4];
    unsigned tx_len, tx_pos, keyup;
    uint32_t tx_hz;
    bool voice_tx, voice_stop;
    unsigned voice_seq;
    p25_call call;
    unsigned deaf;
    /* Phase 2 */
    bool tdma;
    uint8_t slot;
    p2_rx p2;
    p2_tx p2tx;
    bool p2_talking, p2_sending;
    uint8_t p2_burst[P2_BURST_DIBITS];
    unsigned p2_pos, p2_bursts_sent;
    uint32_t next_vcw_ms;
    /* heard */
    uint16_t heard_group;
    uint32_t heard_source;
    unsigned voice_frames, voice_ends, isp_frames, tunes;
} radio;

static site S;
static radio R[3];
static unsigned n_radios;

static uint32_t rrnd(radio *r)
{
    r->rnd ^= r->rnd << 13;
    r->rnd ^= r->rnd >> 17;
    r->rnd ^= r->rnd << 5;
    return r->rnd;
}

static void unpack(const uint8_t *bytes, unsigned dibits, uint8_t *out)
{
    for (unsigned i = 0; i < dibits; i++) out[i] = (uint8_t)((bytes[i >> 2] >> (6 - 2 * (i & 3))) & 3u);
}

static void voice_frame(radio *r)
{
    static const uint16_t silence[P25_IMBE_FRAMES][8];
    static const uint8_t lsd[2];
    uint8_t buf[P25_LDU_BYTES];
    unsigned bytes;
    if (r->voice_seq == 0) {
        p25_encode_hdu(&r->call, buf);
        bytes = P25_HDU_BYTES;
    } else if (r->voice_stop) {
        p25_encode_tdu(&r->call, buf);
        bytes = P25_TDU_BYTES;
        r->voice_tx = false;
    } else {
        p25_encode_ldu(&r->call, (r->voice_seq & 1) ? P25_DUID_LDU1 : P25_DUID_LDU2, silence, lsd, buf);
        bytes = P25_LDU_BYTES;
    }
    r->voice_seq++;
    unpack(buf, bytes * 4, r->tx);
    r->tx_len = bytes * 4;
    r->tx_pos = 0;
}

static void on_p2_mac(void *ctx, uint8_t slot, const p2_mac *m);
static void on_p2_voice(void *ctx, uint8_t slot, const uint8_t vcw[P2_VCW_BYTES], bool encrypted);

static void apply(radio *r, const p25t_su_out *o)
{
    if (o->tune) {
        r->rx_hz = o->rx_hz;
        p25t_rx_reset(&r->cc);
        p25_rx_init(&r->vrx, r->su.cfg.nac, 2);
        r->deaf = SETTLE_SYMBOLS;
        r->tunes++;
        r->tdma = o->tdma;
        r->slot = o->slot;
        r->p2_talking = r->p2_sending = false;
        if (o->tdma)
            p2_rx_init(&r->p2, o->slot, r->su.cfg.wacn, r->su.cfg.sysid, r->su.cc_nac, on_p2_mac, on_p2_voice, r);
    }
    if (o->send_isp) {
        /* the unit asks for every transmission: the harness checks it has a reason to */
        assert(r->su.cfg.tx_enabled && o->tx_hz);
        assert(r->tx_pos >= r->tx_len && !r->voice_tx);
        uint8_t f[P25T_TSDU_MAX_BYTES];
        unsigned bits = p25t_tsdu_encode(o->nac, (const uint8_t(*)[P25T_TSBK_BYTES])o->isp, 1, P25T_STATUS_UNKNOWN, f);
        unpack(f, bits / 2, r->tx);
        r->tx_len = bits / 2;
        r->tx_pos = 0;
        r->tx_hz = o->tx_hz;
        r->keyup = KEYUP_SYMBOLS;
        r->isp_frames++;
    }
    if (o->voice_tx_start && o->tdma) {
        assert(r->su.cfg.tx_enabled && r->su.cfg.tdma_tx && r->su.ptt && o->voice_source == r->su.cfg.unit_id);
        p2_tx_init(&r->p2tx, r->su.cc_nac);
        p2_tx_begin(&r->p2tx, o->voice_group, o->voice_source);
        r->p2_talking = true;
        r->tx_hz = o->tx_hz;
        r->next_vcw_ms = r->ms;
    } else if (o->voice_tx_start) {
        assert(r->su.cfg.tx_enabled && r->su.ptt && o->voice_source == r->su.cfg.unit_id);
        r->call = (p25_call){ .nac = r->su.cc_nac, .talkgroup = o->voice_group, .source_id = o->voice_source,
                              .status_symbol = 2 };
        r->voice_tx = true;
        r->voice_stop = false;
        r->voice_seq = 0;
        r->tx_hz = o->tx_hz;
        r->keyup = KEYUP_SYMBOLS;
        voice_frame(r);
    }
    if (o->voice_tx_stop) {
        r->voice_stop = true;
        if (r->p2_talking) p2_tx_end(&r->p2tx);
    }
}

static void on_p2_mac(void *ctx, uint8_t slot, const p2_mac *m)
{
    radio *r = ctx;
    (void)slot;
    p25t_su_out o;
    if (m->opcode == P2_MAC_PTT) {
        r->heard_source = m->source;
        r->heard_group = m->group;
        p25t_su_voice(&r->su, m->group, m->source, m->algid != P2_ALGID_CLEAR, false, &o);
    } else if (m->user) {
        r->heard_source = m->user_source;
        r->heard_group = m->user_group;
        p25t_su_voice(&r->su, m->user_group, m->user_source, (m->svc & P2_SVC_ENCRYPTED) != 0, false, &o);
    } else if (m->opcode == P2_MAC_END_PTT) {
        r->voice_ends++;
        p25t_su_voice(&r->su, m->group, 0, false, true, &o);
    } else {
        return;
    }
    apply(r, &o);
}

static void on_p2_voice(void *ctx, uint8_t slot, const uint8_t vcw[P2_VCW_BYTES], bool encrypted)
{
    radio *r = ctx;
    (void)slot;
    (void)vcw;
    r->voice_frames++;
    p25t_su_out o;
    p25t_su_voice(&r->su, 0, 0, encrypted, false, &o);
    apply(r, &o);
}

static void on_tsbk(void *ctx, uint16_t nac, const uint8_t t[P25T_TSBK_BYTES])
{
    radio *r = ctx;
    p25t_su_out o;
    p25t_su_tsbk(&r->su, nac, t, &o);
    apply(r, &o);
}

static void on_status(void *ctx, uint8_t status, uint32_t at)
{
    radio *r = ctx;
    (void)at;
    p25t_su_out o;
    p25t_su_status(&r->su, status, &o);
    apply(r, &o);
}

static void radio_init(radio *r, uint32_t unit, bool tx, uint32_t seed)
{
    memset(r, 0, sizeof *r);
    r->rnd = seed;
    p25t_su_cfg c = { .wacn = WACN, .sysid = SYSID, .unit_id = unit, .nac = NAC, .cc_hz = { WRONG_CC_HZ, S.cc_down_hz },
                      .cc_count = 2, .talkgroup = GROUP, .tx_enabled = tx, .tdma_rx = true, .tdma_tx = true };
    p25t_rx_init(&r->cc, P25_NAC_ANY, on_tsbk, NULL, on_status, r);
    p25_rx_init(&r->vrx, NAC, 2);
    p25t_su_out o;
    p25t_su_init(&r->su, &c, site_ms(&S), seed, &o);
    apply(r, &o);
}

static bool transmitting(const radio *r)
{
    return r->keyup || r->tx_pos < r->tx_len || r->voice_tx || r->p2_talking || r->p2_sending;
}

/* One Phase 2 symbol time: send our inbound burst's symbol (deaf meanwhile) or receive; at each burst boundary of
 * the superframe received, start our next inbound burst when the next burst is our slot's */
static void radio_tdma_symbol(radio *r)
{
    uint8_t heard = (uint8_t)(rrnd(r) & 3u);
    if (r->p2_sending) {
        site_tx(&S, r->tx_hz, r->p2_burst[r->p2_pos++]);
        if (r->p2_pos == P2_BURST_DIBITS) r->p2_sending = false;
    } else {
        bool fresh;
        int d = site_rx(&S, r->rx_hz, &fresh);
        if (d >= 0 && !r->deaf) heard = (uint8_t)d;
        if (r->deaf) r->deaf--;
    }
    p2_rx_dibit(&r->p2, heard);
    if (!r->p2_talking || !r->p2.locked || !r->p2.known || r->p2.pos != 0) return;
    unsigned next = (r->p2.burst + 1u) % P2_SUPERFRAME_BURSTS;
    if (p2_burst_channel(next) != r->slot) return;
    if (r->p2tx.state == P2_TX_HANG || r->p2tx.state == P2_TX_IDLE) {
        r->p2_talking = false;   /* MAC_END_PTT went out: the unit is done; the site keeps the hang time */
        return;
    }
    uint8_t payload[P2_PAYLOAD_DIBITS];
    uint8_t duid = p2_tx_burst(&r->p2tx, next, payload);
    /* no ISCH inbound: those 20 symbols stand for the inbound burst's own framing (guard, ramp, sync) */
    p2_burst_build(0, duid, payload, r->p2.mask, next, r->p2_burst);
    r->p2_sending = true;
    r->p2_pos = 0;
    r->p2_bursts_sent++;
}

static void radio_symbol(radio *r)
{
    if (transmitting(r)) {
        if (r->keyup) {
            r->keyup--;
            return;
        }
        site_tx(&S, r->tx_hz, r->tx[r->tx_pos++]);
        if (r->tx_pos >= r->tx_len) {
            if (r->voice_tx) voice_frame(r);
            if (!transmitting(r)) r->deaf = SETTLE_SYMBOLS;
        }
        return;
    }
    bool fresh;
    int d = site_rx(&S, r->rx_hz, &fresh);
    if (d < 0) d = (int)(rrnd(r) & 3u);      /* no carrier: noise */
    if (r->deaf) {
        r->deaf--;
        return;
    }
    if (r->su.state == P25T_SU_VOICE_RX) {
        if (p25_rx_push_dibit(&r->vrx, (uint8_t)d) == 1) {
            p25_decoded f;
            if (p25_decode_frame(r->vrx.frame, p25_frame_bytes(r->vrx.nid.duid), &f) != 0) return;
            uint8_t duid = f.nid.duid;
            bool ended = duid == P25_DUID_TDU || duid == P25_DUID_TDULC;
            bool lc = f.metadata_valid && (duid == P25_DUID_LDU1 || duid == P25_DUID_HDU);
            uint16_t g = lc ? f.talkgroup : 0;
            uint32_t src = lc && duid == P25_DUID_LDU1 ? f.source_id : 0;
            r->voice_frames++;
            if (src) r->heard_source = src;
            if (g) r->heard_group = g;
            r->voice_ends += ended;
            p25t_su_out o;
            p25t_su_voice(&r->su, g, src, f.metadata_valid && !f.clear_voice, ended, &o);
            apply(r, &o);
        }
    } else {
        p25t_rx_dibit(&r->cc, (uint8_t)d);
    }
}

static void step(void)
{
    site_begin(&S);
    uint32_t ms = site_ms(&S);
    for (unsigned i = 0; i < n_radios; i++) {
        radio *r = &R[i];
        if (ms != r->ms) {
            r->ms = ms;
            p25t_su_out o;
            p25t_su_tick(&r->su, ms, &o);
            apply(r, &o);
            /* the microphone: a 20 ms frame at a time while the Phase 2 call is up */
            while (r->p2_talking && !r->p2tx.ending && (int32_t)(ms - r->next_vcw_ms) >= 0) {
                p2_tx_voice(&r->p2tx, p2_silence_vcw);
                r->next_vcw_ms += 20;
            }
        }
        if (r->tdma) {
            if (S.tick % SITE_TDMA_TICKS == 0) radio_tdma_symbol(r);
        } else if (S.tick % SITE_FDMA_TICKS == 0) {
            radio_symbol(r);
        }
    }
    site_end(&S);
}

static void run_ms(uint32_t ms)
{
    for (uint32_t t = 0; t < ms * (SITE_TICK_HZ / 1000); t++) step();
}

/* Run until cond holds, at most ms; returns the time it took */
#define RUN_UNTIL(cond, ms)                                                                                          \
    ({                                                                                                               \
        uint32_t t0_ = site_ms(&S), lim_ = (ms);                                                                     \
        while (!(cond) && site_ms(&S) - t0_ < lim_) step();                                                          \
        assert(cond);                                                                                                \
        site_ms(&S) - t0_;                                                                                           \
    })

static site_cfg base_cfg(void)
{
    return (site_cfg){ .nac = NAC, .wacn = WACN, .sysid = SYSID, .rfss = 2, .site = 3,
                       .allowed = { 1001, 1002, 1003 }, .deny_group = 666, .queue_group = 0, .seed = 77 };
}

static void world(const site_cfg *c, unsigned radios, bool tx)
{
    site_init(&S, c);
    n_radios = radios;
    for (unsigned i = 0; i < radios; i++) radio_init(&R[i], 1001 + i, tx, 0x1234u + 77u * i);
}

static void ptt(radio *r, bool down)
{
    p25t_su_out o;
    p25t_su_ptt(&r->su, down, &o);
    apply(r, &o);
}

/* Every inbound packet the site heard carried a configured unit ID and system */
static void check_isps_are_ours(void)
{
    for (unsigned i = 0; i < S.n_isps; i++) {
        const site_isp_log *l = &S.isps[i];
        bool ours = false;
        for (unsigned k = 0; k < n_radios; k++) ours |= l->source == R[k].su.cfg.unit_id;
        assert(ours);
        if (l->opcode == P25T_ISP_U_REG_REQ || l->opcode == P25T_ISP_U_DE_REG_REQ) assert(l->wacn == WACN);
        if (l->opcode != P25T_ISP_GRP_V_REQ) assert(l->sysid == SYSID);
    }
    for (unsigned i = 0; i < S.n_calls; i++)
        if (S.calls[i].unit && S.calls[i].ldu1s) assert(S.calls[i].lc_source == S.calls[i].source);
}

static void registered_and_affiliated(void)
{
    site_cfg c = base_cfg();
    world(&c, 1, true);
    radio *r = &R[0];
    /* the first frequency carries nothing: the unit moves on after P25T_SU_HUNT_MS */
    uint32_t found = RUN_UNTIL(r->su.state == P25T_SU_CC, 8000);
    assert(found >= P25T_SU_HUNT_MS && r->su.cc_index == 1);
    uint32_t reg = RUN_UNTIL(r->su.registered, 3000);
    assert(r->su.cc_tx_hz == S.cc_up_hz && r->su.rfss == 2 && r->su.site == 3);
    uint32_t aff = RUN_UNTIL(r->su.affiliated, 3000);
    assert(site_is_registered(&S, 1001) && site_affiliation(&S, 1001) == GROUP);
    run_ms(5000);
    assert(S.n_isps == 2 && S.isps[0].opcode == P25T_ISP_U_REG_REQ && S.isps[1].opcode == P25T_ISP_GRP_AFF_REQ);
    assert(r->su.isps_sent == 2 && S.collisions == 0);
    check_isps_are_ours();
    fprintf(stderr, "control channel found in %u ms, registered %u ms later, affiliated %u ms after that\n", found, reg,
            aff);
}

static void follows_calls(void)
{
    site_cfg c = base_cfg();
    world(&c, 1, true);
    radio *r = &R[0];
    RUN_UNTIL(r->su.affiliated, 10000);
    /* our talkgroup */
    assert(site_talk(&S, GROUP, 5555, 2000));
    uint32_t t = RUN_UNTIL(r->su.state == P25T_SU_VOICE_RX, 500);
    assert(r->su.call_group == GROUP && !r->su.call_tdma);
    RUN_UNTIL(r->heard_source == 5555, 1000);
    assert(r->su.call_source == 5555 && r->heard_group == GROUP);
    uint32_t back = RUN_UNTIL(r->su.state == P25T_SU_CC, 3000);
    assert(r->voice_ends >= 1);
    fprintf(stderr, "followed a grant in %u ms; %u voice frames; back on the control channel %u ms later\n", t,
            r->voice_frames, back);
    run_ms(500);
    /* another group: not followed */
    unsigned followed = r->su.grants_followed;
    assert(site_talk(&S, 300, 6666, 1000));
    run_ms(2000);
    assert(r->su.grants_followed == followed && r->su.state == P25T_SU_CC);
    run_ms(1000);
    /* ... unless in the priority list */
    r->su.cfg.priority[0] = 300;
    r->su.cfg.priority_count = 1;
    assert(site_talk(&S, 300, 6666, 1000));
    RUN_UNTIL(r->su.state == P25T_SU_VOICE_RX && r->su.call_group == 300, 500);
    RUN_UNTIL(r->heard_source == 6666, 1000);
    RUN_UNTIL(r->su.state == P25T_SU_CC, 3000);
    run_ms(1000);
    /* ... or the unit hears every group */
    r->su.cfg.hear_all = true;
    assert(site_talk(&S, 400, 7777, 1000));
    RUN_UNTIL(r->su.state == P25T_SU_VOICE_RX && r->su.call_group == 400, 500);
    RUN_UNTIL(r->su.state == P25T_SU_CC, 3000);
    /* a call already in progress is found from the grant updates */
    r->su.cfg.hear_all = false;
    run_ms(1000);
    assert(site_talk(&S, GROUP, 8888, 3000));
    S.nq = 0;                                       /* the grants are lost: only the updates remain */
    RUN_UNTIL(r->su.state == P25T_SU_VOICE_RX && r->su.call_group == GROUP, 1000);
    RUN_UNTIL(r->heard_source == 8888, 1000);
    check_isps_are_ours();
}

static void group_call(void)
{
    site_cfg c = base_cfg();
    world(&c, 2, true);
    radio *a = &R[0], *b = &R[1];
    RUN_UNTIL(a->su.affiliated && b->su.affiliated, 15000);
    run_ms(500);
    unsigned isps = S.n_isps;
    ptt(a, true);
    assert(a->su.state == P25T_SU_REQUEST && a->su.deny == P25T_SU_DENY_NONE);
    uint32_t granted = RUN_UNTIL(a->su.state == P25T_SU_VOICE_TX, 2000);
    assert(S.n_isps == isps + 1 && S.isps[isps].opcode == P25T_ISP_GRP_V_REQ && S.isps[isps].source == 1001 &&
           S.isps[isps].group == GROUP);
    /* the other unit follows the grant and hears the first unit through the repeater */
    RUN_UNTIL(b->su.state == P25T_SU_VOICE_RX, 500);
    RUN_UNTIL(b->heard_source == 1001, 1500);
    run_ms(1000);
    ptt(a, false);
    assert(a->su.state == P25T_SU_VOICE_RX);
    RUN_UNTIL(!transmitting(a), 500);
    RUN_UNTIL(b->voice_ends >= 1, 1000);
    RUN_UNTIL(a->su.state == P25T_SU_CC && b->su.state == P25T_SU_CC, 3000);
    const site_call_log *call = &S.calls[S.n_calls - 1];
    assert(call->unit && call->source == 1001 && call->group == GROUP);
    assert(call->hdus == 1 && call->ldu1s >= 3 && call->ldu2s >= 3 && call->tdus == 1);
    assert(call->lc_source == 1001 && call->lc_group == GROUP);
    fprintf(stderr, "group call: granted %u ms after PTT; site heard HDU, %u LDU1, %u LDU2, TDU from unit 1001; "
                    "unit 1002 heard it\n", granted, call->ldu1s, call->ldu2s);
    check_isps_are_ours();
}

static void queued_acknowledged_denied(void)
{
    site_cfg c = base_cfg();
    c.queue_group = GROUP;
    c.queue_ms = 1500;
    world(&c, 1, true);
    radio *r = &R[0];
    RUN_UNTIL(r->su.affiliated, 10000);
    ptt(r, true);
    RUN_UNTIL(r->su.state == P25T_SU_QUEUED, 1000);
    uint32_t q = RUN_UNTIL(r->su.state == P25T_SU_VOICE_TX, 3000);
    assert(q >= 1000);
    run_ms(500);
    ptt(r, false);
    RUN_UNTIL(r->su.state == P25T_SU_CC, 4000);
    assert(S.calls[S.n_calls - 1].tdus == 1);
    /* released while queued: the grant that comes later is not used to transmit */
    ptt(r, true);
    RUN_UNTIL(r->su.state == P25T_SU_QUEUED, 1000);
    ptt(r, false);
    assert(r->su.state == P25T_SU_CC);
    run_ms(3000);
    assert(!transmitting(r) && r->su.state != P25T_SU_VOICE_TX);
    RUN_UNTIL(r->su.state == P25T_SU_CC, 3000);

    /* acknowledged, then granted */
    S.cfg.queue_group = 0;
    S.cfg.ack_ms = 600;
    run_ms(500);
    ptt(r, true);
    uint32_t a = RUN_UNTIL(r->su.state == P25T_SU_VOICE_TX, 2000);
    assert(a >= 600);
    ptt(r, false);
    RUN_UNTIL(r->su.state == P25T_SU_CC, 4000);

    /* denied: back on the control channel, the reason kept for the display */
    S.cfg.ack_ms = 0;
    r->su.cfg.talkgroup = 666;
    run_ms(500);
    unsigned isps = r->su.isps_sent;
    ptt(r, true);
    RUN_UNTIL(r->su.deny != P25T_SU_DENY_NONE, 1500);
    assert(r->su.deny == P25T_SU_DENY_SYSTEM && r->su.deny_reason == 0x31 && r->su.state == P25T_SU_CC);
    assert(r->su.isps_sent == isps + 1);
    ptt(r, false);
    run_ms(2000);
    assert(r->su.isps_sent == isps + 1);
    check_isps_are_ours();
}

static void no_answer(void)
{
    site_cfg c = base_cfg();
    c.silent = true;
    world(&c, 1, true);
    radio *r = &R[0];
    RUN_UNTIL(r->su.state == P25T_SU_CC, 8000);
    /* registration: P25T_SU_TRIES attempts, then quiet until the next automatic attempt */
    run_ms(P25T_SU_RESPONSE_MS * (P25T_SU_TRIES + 2) + 2000);
    assert(r->su.isps_sent == P25T_SU_TRIES && S.n_isps == P25T_SU_TRIES && !r->su.registered);
    for (unsigned i = 1; i < S.n_isps; i++) assert(S.isps[i].tick - S.isps[i - 1].tick >= P25T_SU_RESPONSE_MS * 24u);
    /* PTT without registration is refused before anything is sent */
    ptt(r, true);
    assert(r->su.deny == P25T_SU_DENY_NOT_REGISTERED && r->su.state == P25T_SU_CC);
    ptt(r, false);
    run_ms(P25T_SU_AUTO_GAP_MS);
    assert(r->su.isps_sent > P25T_SU_TRIES && r->su.isps_sent <= 2 * P25T_SU_TRIES);

    /* registered, then the site stops answering: a call request gives up after its tries */
    site_cfg c2 = base_cfg();
    world(&c2, 1, true);
    r = &R[0];
    RUN_UNTIL(r->su.affiliated, 10000);
    S.cfg.silent = true;
    unsigned before = r->su.isps_sent;
    ptt(r, true);
    RUN_UNTIL(r->su.deny == P25T_SU_DENY_NO_RESPONSE, P25T_SU_RESPONSE_MS * (P25T_SU_TRIES + 2) + 2000);
    assert(r->su.isps_sent == before + P25T_SU_TRIES && r->su.state == P25T_SU_CC);
    ptt(r, false);
    check_isps_are_ours();
}

static void refused_and_other_systems(void)
{
    /* a unit ID the administrator did not assign: refused, not retried for P25T_SU_REG_RETRY_MS */
    site_cfg c = base_cfg();
    c.allowed[0] = 1002;
    c.allowed[1] = 0;
    world(&c, 1, true);
    radio *r = &R[0];
    RUN_UNTIL(r->su.reg_refused, 10000);
    run_ms(P25T_SU_REG_RETRY_MS - 2000);
    assert(r->su.isps_sent == 1 && !r->su.registered);
    ptt(r, true);
    assert(r->su.deny == P25T_SU_DENY_NOT_REGISTERED);
    ptt(r, false);

    /* another system on the configured frequency (another WACN, another System ID): never a transmission */
    site_cfg o = base_cfg();
    o.wacn = 0xABCDE;
    world(&o, 1, true);
    r = &R[0];
    run_ms(20000);
    assert(r->su.state == P25T_SU_HUNT && r->su.isps_sent == 0 && r->isp_frames == 0 && S.n_isps == 0);
    ptt(r, true);
    assert(r->su.deny == P25T_SU_DENY_SYSTEM && !transmitting(r));
    ptt(r, false);
    o = base_cfg();
    o.sysid = 0x3A6;
    world(&o, 1, true);
    run_ms(20000);
    assert(R[0].su.isps_sent == 0 && S.n_isps == 0);

    /* transmit off (CONFIG_P25_TRUNK_TX off, or no identity entered): follows calls, never transmits */
    site_cfg c3 = base_cfg();
    world(&c3, 1, false);
    r = &R[0];
    RUN_UNTIL(r->su.state == P25T_SU_CC, 8000);
    run_ms(5000);
    assert(!r->su.registered && r->su.isps_sent == 0 && S.n_isps == 0);
    site_talk(&S, GROUP, 4242, 1000);
    RUN_UNTIL(r->heard_source == 4242, 1500);
    RUN_UNTIL(r->su.state == P25T_SU_CC, 3000);
    ptt(r, true);
    assert(r->su.deny == P25T_SU_DENY_NO_TX && r->su.state == P25T_SU_CC);
    run_ms(2000);
    assert(r->isp_frames == 0 && !transmitting(r));
    ptt(r, false);

    /* an identity not whole: transmit stays off */
    p25t_su su;
    p25t_su_out out;
    p25t_su_cfg bad[] = {
        { .wacn = 0, .sysid = SYSID, .unit_id = 1001, .talkgroup = GROUP, .tx_enabled = true },
        { .wacn = WACN, .sysid = 0, .unit_id = 1001, .talkgroup = GROUP, .tx_enabled = true },
        { .wacn = WACN, .sysid = SYSID, .unit_id = 0, .talkgroup = GROUP, .tx_enabled = true },
        { .wacn = WACN, .sysid = SYSID, .unit_id = 0x1000000, .talkgroup = GROUP, .tx_enabled = true },
        { .wacn = 0x100000, .sysid = SYSID, .unit_id = 1001, .talkgroup = GROUP, .tx_enabled = true },
        { .wacn = WACN, .sysid = SYSID, .unit_id = 1001, .talkgroup = 0, .tx_enabled = true },
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        p25t_su_init(&su, &bad[i], 0, 1, &out);
        assert(!su.cfg.tx_enabled);
    }
}

static void contention(void)
{
    /* three units switched on together: they register and affiliate through the random access */
    site_cfg c = base_cfg();
    world(&c, 3, true);
    uint32_t t = RUN_UNTIL(R[0].su.affiliated && R[1].su.affiliated && R[2].su.affiliated, 30000);
    for (unsigned i = 0; i < 3; i++) assert(site_affiliation(&S, 1001 + i) == GROUP);
    unsigned sent = R[0].su.isps_sent + R[1].su.isps_sent + R[2].su.isps_sent;
    fprintf(stderr, "3 units registered and affiliated in %u ms: %u inbound packets, %u symbols collided, status "
                    "busy %u / idle %u\n", t, sent, S.collisions, S.busy_status, S.idle_status);
    assert(S.busy_status > 0);
    check_isps_are_ours();
}

static void lost_and_leave(void)
{
    site_cfg c = base_cfg();
    world(&c, 1, true);
    radio *r = &R[0];
    RUN_UNTIL(r->su.affiliated, 10000);
    S.cfg.no_cc = true;
    uint32_t lost = RUN_UNTIL(r->su.state == P25T_SU_HUNT, 5000);
    assert(lost >= P25T_SU_CC_LOST_MS);
    run_ms(4000);
    assert(r->su.state == P25T_SU_HUNT);
    S.cfg.no_cc = false;
    RUN_UNTIL(r->su.state == P25T_SU_CC, 10000);
    /* still registered: no new registration on finding the control channel again */
    unsigned isps = r->su.isps_sent;
    run_ms(3000);
    assert(r->su.isps_sent == isps);

    /* leaving the channel: de-registration on the configured system */
    p25t_su_out o;
    p25t_su_leave(&r->su, &o);
    assert(o.send_isp && o.tx_hz == S.cc_up_hz);
    apply(r, &o);
    run_ms(500);
    assert(!site_is_registered(&S, 1001) && S.isps[S.n_isps - 1].opcode == P25T_ISP_U_DE_REG_REQ);
    check_isps_are_ours();
}

static void tdma_calls(void)
{
    site_cfg c = base_cfg();
    c.tdma = true;
    world(&c, 3, true);
    radio *a = &R[0], *b = &R[1], *old = &R[2];
    old->su.cfg.tdma_rx = old->su.cfg.tdma_tx = false;   /* a radio like this one: no Phase 2 */
    RUN_UNTIL(a->su.affiliated && b->su.affiliated && old->su.affiliated, 20000);
    run_ms(500);
    /* a talker's call on a TDMA slot: followed by the units that can, not by the one that cannot */
    assert(site_talk(&S, GROUP, 5555, 2000));
    uint32_t t = RUN_UNTIL(a->su.state == P25T_SU_VOICE_RX && b->su.state == P25T_SU_VOICE_RX, 500);
    assert(a->su.call_tdma && a->su.call_slot == 0 && a->tdma);
    RUN_UNTIL(a->heard_source == 5555 && b->heard_source == 5555, 1500);
    RUN_UNTIL(a->voice_ends >= 1, 3000);
    RUN_UNTIL(a->su.state == P25T_SU_CC && b->su.state == P25T_SU_CC, 3000);
    assert(old->su.state == P25T_SU_CC && old->su.grants_followed == 0);
    fprintf(stderr, "TDMA: followed a grant to slot 0 in %u ms, %u voice frames heard, MAC_END_PTT heard\n", t,
            a->voice_frames);
    run_ms(1000);
    /* a group call on PTT, granted on a TDMA slot: inbound bursts in the slot, repeated, heard by the other unit */
    unsigned heard_before = b->voice_frames;
    ptt(a, true);
    uint32_t g = RUN_UNTIL(a->su.state == P25T_SU_VOICE_TX, 2000);
    assert(a->su.call_tdma);
    RUN_UNTIL(b->su.state == P25T_SU_VOICE_RX && b->heard_source == 1001, 1500);
    run_ms(1500);
    ptt(a, false);
    RUN_UNTIL(!a->p2_talking && !a->p2_sending, 1000);
    RUN_UNTIL(b->voice_ends >= 2, 1500);
    RUN_UNTIL(a->su.state == P25T_SU_CC && b->su.state == P25T_SU_CC, 4000);
    const site_call_log *call = &S.calls[S.n_calls - 1];
    assert(call->unit && call->source == 1001 && call->lc_source == 1001 && call->lc_group == GROUP);
    /* MAC_PTT and MAC_END_PTT twice on the FACCH, once more when a SACCH position falls in them */
    assert(call->mac_ptts >= 2 && call->mac_ends >= 2 && call->vcws >= 70 && S.t[0].in.mac_bad == 0);
    assert(b->voice_frames - heard_before >= 70);
    fprintf(stderr, "TDMA group call: granted %u ms after PTT; %u inbound bursts; site heard MAC_PTT x%u, %u voice "
                    "frames, MAC_END_PTT x%u from unit 1001; unit 1002 heard %u frames\n", g, a->p2_bursts_sent,
            call->mac_ptts, call->vcws, call->mac_ends, b->voice_frames - heard_before);
    /* the radio without a Phase 2 transmitter is granted a TDMA slot: it refuses, never transmits voice */
    unsigned calls = S.n_calls;
    ptt(old, true);
    RUN_UNTIL(old->su.deny != P25T_SU_DENY_NONE, 2000);
    assert(old->su.deny == P25T_SU_DENY_BAND && old->su.state == P25T_SU_CC && !old->voice_tx && !old->p2_talking);
    ptt(old, false);
    assert(S.n_calls == calls + 1 && S.calls[calls].mac_ptts == 0 && S.calls[calls].vcws == 0);
    check_isps_are_ours();
}

int main(void)
{
    registered_and_affiliated();
    follows_calls();
    group_call();
    queued_acknowledged_denied();
    no_answer();
    refused_and_other_systems();
    contention();
    lost_and_leave();
    tdma_calls();
    fprintf(stderr, "test_su: ok\n");
    return 0;
}
