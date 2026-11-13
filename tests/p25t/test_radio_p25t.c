/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The radio's trunking channel layer (integration/radiofoundry/p25t/radio_p25t.c) unchanged, on host stand-ins for
 * the native modem and the P25 worker (tests/p25t/stubs), against the simulated site (site.c): the control channel
 * reaches the logic through the receive tap, inbound packets go out as the native layer would send them (three lead
 * blocks, the TSDU, a tail block; deaf meanwhile, a gap in the receive stream after), calls followed play, PTT
 * requests a channel and transmits on the granted one, a refusal reaches the radio core as such. */
#include "config.h"
#include "native_modem/native_lab.h"
#include "p25_lab.h"
#include "radio_p25t.h"
#include "site.h"
#include "su.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { LEAD_DIBITS = 3 * 144, TAIL_DIBITS = 144, NT_TX_COMPLETE = 3 };

static site S;

/* ---- the native layer and the P25 worker, as radio_p25t.c sees them ---- */
static native_channel chan;
static bool chan_on, keyed, ptt_down;
static uint32_t native_calls, retunes;
static p25_phy_tap tap;
static void *tap_ctx;
static p25_lab_channel lab;
static uint8_t inbound[300];
static uint16_t inbound_n;
/* what is on air from us: an inbound packet or a voice call (one dibit a byte) */
static uint8_t tx[P25_LDU_BYTES * 4 + LEAD_DIBITS + TAIL_DIBITS];
static unsigned tx_len, tx_pos, inbound_sent, voice_seq;
static uint32_t tx_hz;
static bool voice_on, voice_stop;
static p25_call call;
/* the voice receiver of the P25 worker */
static p25_rx vrx;
static p25_lab_rx_status rxs;
static uint32_t rnd = 77;

uint32_t hal_millis(void) { return site_ms(&S); }
uint32_t hal_micros(void) { return S.tick * 1000u / 24u; }

int native_lab_channel(const native_channel *c)
{
    chan_on = c != NULL;
    if (c && (c->rx_hz != chan.rx_hz)) {
        retunes++;
        p25_rx_init(&vrx, P25_NAC_ANY, 2);
        if (tap) tap(tap_ctx, P25_PHY_TAP_GAP);
    }
    if (c) chan = *c;
    return 1;
}

static void unpack(const uint8_t *b, unsigned dibits, uint8_t *out)
{
    for (unsigned i = 0; i < dibits; i++) out[i] = (uint8_t)((b[i >> 2] >> (6 - 2 * (i & 3))) & 3u);
}

static void voice_frame(void)
{
    static const uint16_t silence[P25_IMBE_FRAMES][8];
    static const uint8_t lsd[2];
    uint8_t buf[P25_LDU_BYTES];
    unsigned bytes;
    if (voice_seq == 0) {
        p25_encode_hdu(&call, buf);
        bytes = P25_HDU_BYTES;
    } else if (voice_stop) {
        p25_encode_tdu(&call, buf);
        bytes = P25_TDU_BYTES;
        voice_on = false;
    } else {
        p25_encode_ldu(&call, (voice_seq & 1) ? P25_DUID_LDU1 : P25_DUID_LDU2, silence, lsd, buf);
        bytes = P25_LDU_BYTES;
    }
    voice_seq++;
    unpack(buf, bytes * 4, tx);
    tx_len = bytes * 4;
    tx_pos = 0;
}

int native_lab_channel_ptt(bool down)
{
    ptt_down = down;
    if (down) {
        /* the P25 worker's call: the channel's configuration (radio_p25t set it on the grant) */
        call = (p25_call){ .nac = lab.nac_tx, .talkgroup = lab.talkgroup, .source_id = lab.unit_id, .status_symbol = 2 };
        tx_hz = chan.tx_hz;
        voice_on = keyed = true;
        voice_stop = false;
        voice_seq = 0;
        voice_frame();
    } else {
        voice_stop = true;
    }
    return 1;
}

void native_lab_channel_state(native_channel_state *s)
{
    memset(s, 0, sizeof *s);
    s->enabled = chan_on;
    s->transmitting = voice_on || (keyed && tx_pos < tx_len);
    s->keyed = keyed;
    s->receiving = !keyed && tx_pos >= tx_len;
    s->calls = native_calls;
    s->reason = NT_TX_COMPLETE;
    s->inbound_sent = inbound_sent;
}

int p25_lab_inbound_load(const uint8_t *d, uint16_t n)
{
    assert(n <= sizeof inbound);
    memcpy(inbound, d, n);
    inbound_n = n;
    return 1;
}

int native_lab_channel_inbound(void)
{
    if (tx_pos < tx_len || voice_on) return 0;
    /* lead blocks of 0x77 (dibits 1 3 1 3 ...), the packet, a tail block */
    unsigned n = 0;
    for (unsigned i = 0; i < LEAD_DIBITS; i++) tx[n++] = (i & 1) ? 3 : 1;
    memcpy(tx + n, inbound, inbound_n);
    n += inbound_n;
    for (unsigned i = 0; i < TAIL_DIBITS; i++) tx[n++] = (i & 1) ? 3 : 1;
    tx_len = n;
    tx_pos = 0;
    tx_hz = chan.tx_hz;
    inbound_sent++;
    return 1;
}

void p25_lab_channel_config(const p25_lab_channel *c) { lab = *c; }
void p25_lab_rx_status_get(p25_lab_rx_status *s) { *s = rxs; }
void p25_lab_set_tap(p25_phy_tap fn, void *ctx)
{
    tap = fn;
    tap_ctx = ctx;
}

/* One symbol time of the radio: transmit what is queued (deaf, the receive session stopped), else receive */
static void radio_symbol(void)
{
    if (tx_pos < tx_len) {
        site_tx(&S, tx_hz, tx[tx_pos++]);
        if (tx_pos == tx_len) {
            if (voice_on) {
                voice_frame();
            } else {
                if (keyed) native_calls++;   /* the call ended */
                keyed = false;
                /* a new receive session: a gap in the stream */
                if (tap) tap(tap_ctx, P25_PHY_TAP_GAP);
            }
        }
        return;
    }
    if (!chan_on) return;
    bool fresh;
    int d = site_rx(&S, chan.rx_hz, &fresh);
    if (d < 0) {
        rnd ^= rnd << 13;
        rnd ^= rnd >> 17;
        rnd ^= rnd << 5;
        d = (int)(rnd & 3u);
    }
    if (tap) tap(tap_ctx, (uint8_t)d);
    if (p25_rx_push_dibit(&vrx, (uint8_t)d) == 1) {
        p25_decoded f;
        if (p25_decode_frame(vrx.frame, p25_frame_bytes(vrx.nid.duid), &f) == 0) {
            rxs.frames++;
            rxs.last_frame_ms = site_ms(&S) | 1u;
            rxs.nac = f.nid.nac;
            rxs.ended = f.nid.duid == P25_DUID_TDU || f.nid.duid == P25_DUID_TDULC;
            if (f.metadata_valid && (f.nid.duid == P25_DUID_LDU1 || f.nid.duid == P25_DUID_HDU)) {
                rxs.talkgroup = f.talkgroup;
                if (f.nid.duid == P25_DUID_LDU1) rxs.source = f.source_id;
            }
            rxs.encrypted = f.metadata_valid && !f.clear_voice;
            rxs.playing = !rxs.encrypted;
        }
    }
}

static radio_dv_status_t st;
static void step(void)
{
    site_begin(&S);
    if (S.tick % SITE_FDMA_TICKS == 0) radio_symbol();
    site_end(&S);
    if (S.tick % (SITE_TICK_HZ / 1000) == 0) {
        radio_p25t_service(site_ms(&S));
        radio_p25t_status(&st, site_ms(&S));
    }
}

#define RUN_UNTIL(cond, ms)                                                                                          \
    do {                                                                                                             \
        uint32_t t0_ = site_ms(&S);                                                                                  \
        while (!(cond) && site_ms(&S) - t0_ < (ms)) step();                                                          \
        assert(cond);                                                                                                \
    } while (0)

static void run_ms(uint32_t ms)
{
    uint32_t t0 = site_ms(&S);
    while (site_ms(&S) - t0 < ms) step();
}

int main(void)
{
    site_cfg c = { .nac = 0x293, .wacn = 0xBEE00, .sysid = 0x3A5, .rfss = 2, .site = 3, .allowed = { 1001 }, .seed = 9 };
    site_init(&S, &c);
    p25_rx_init(&vrx, P25_NAC_ANY, 2);
    radio_dv_cfg_t cfg = { .mode = 4, .rx_hz = S.cc_down_hz, .tx_hz = S.cc_up_hz, .power = 1, .nac_rx = 0x293,
                           .nac_tx = 0x293, .talkgroup = 100, .unit_id = 1001, .wacn = 0xBEE00, .sysid = 0x3A5 };
    assert(radio_p25t_select(&cfg));
    assert(tap && chan_on && chan.rx_hz == S.cc_down_hz);

    /* the control channel through the tap: registration and affiliation as inbound packets */
    RUN_UNTIL(st.trunk_registered && st.trunk_affiliated, 10000);
    assert(site_is_registered(&S, 1001) && site_affiliation(&S, 1001) == 100);
    assert(inbound_sent == 2 && S.n_isps == 2 && st.sync && !st.transmitting);
    fprintf(stderr, "radio layer: registered and affiliated by %u ms through the receive tap and 2 inbound packets\n",
            site_ms(&S));

    /* a call on our talkgroup: followed, played, back to the control channel */
    run_ms(500);
    assert(site_talk(&S, 100, 5555, 1500));
    RUN_UNTIL(st.playing && st.talkgroup == 100 && st.source == 5555, 1500);
    RUN_UNTIL(st.trunk_state == P25T_SU_CC && chan.rx_hz == S.cc_down_hz, 4000);

    /* PTT: a request, the grant, our call on the voice channel's inbound frequency, its end */
    run_ms(500);
    uint32_t calls = st.tx_calls;
    assert(radio_p25t_ptt(true));
    run_ms(1);
    assert(st.transmitting && !keyed);
    RUN_UNTIL(keyed && ptt_down, 2000);
    assert(chan.tx_hz != S.cc_up_hz && tx_hz == chan.tx_hz && call.source_id == 1001 && call.talkgroup == 100);
    run_ms(1000);
    assert(st.transmitting && st.keyed);
    assert(radio_p25t_ptt(false));
    RUN_UNTIL(st.tx_calls == calls + 1 && !st.transmitting, 1500);
    assert(st.tx_reason == NT_TX_COMPLETE);
    const site_call_log *log = &S.calls[S.n_calls - 1];
    assert(log->unit && log->lc_source == 1001 && log->lc_group == 100 && log->hdus == 1 && log->tdus == 1);
    RUN_UNTIL(st.trunk_state == P25T_SU_CC, 4000);
    fprintf(stderr, "radio layer: group call granted and sent (HDU, %u LDU1, %u LDU2, TDU heard by the site)\n",
            log->ldu1s, log->ldu2s);

    /* the system refuses the next call: the radio core gets a refusal, nothing is keyed */
    S.cfg.deny_group = 100;
    run_ms(500);
    calls = st.tx_calls;
    radio_p25t_ptt(true);
    RUN_UNTIL(st.tx_calls == calls + 1, 2000);
    assert(st.tx_reason == RADIO_DV_TRUNK_REFUSED && !keyed);
    radio_p25t_ptt(false);
    run_ms(200);
    assert(st.tx_calls == calls + 1);

    /* leaving: the tap goes, the native channel is released */
    radio_p25t_select(NULL);
    assert(!tap && !chan_on);
    fprintf(stderr, "test_radio_p25t: ok\n");
    return 0;
}
