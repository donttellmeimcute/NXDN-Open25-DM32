/* SPDX-License-Identifier: GPL-3.0-or-later */
/* NXDN48 channels on the radio (radio_nxdn.h). The radio core keeps the
 * policy (TX rules, time-out, busy lockout by RAN, scan, display); this file
 * keeps the board's audio paths, the link and the voice bridge.
 *
 * Receive: the board's flat FM receive audio (8 kHz) -> nxdn_link -> voice
 * slots, paced one per 20 ms into ambeVoice's sink (it primes three, then
 * plays at the audio clock; NXDN's slots come four at a time every 80 ms).
 * Transmit: board.radio->tx() in FM, the board's flat modulation path fed from
 * nxdn_link's ring by the board's interrupt, voice slots from ambeVoice's
 * source (the mic encoded every 20 ms). Encrypted calls are muted by the call
 * layer and never decrypted. */
#include "radio_nxdn.h"
#include "config.h"
#if CONFIG_NXDN
#include <string.h>
#include "board.h"
#include "hal_time.h"
#include "radio/radio.h"
#include "dmr/ambe/ambeVoice.h"
#include "native_test.h"
#include "nxdn_link.h"

/* The board's flat paths: the transmit scale comes from the tap's nominal level (60 % of the channel's peak
 * deviation, 1.5 kHz at 12.5 kHz), the audio path's measured shape from its flat_* fields */
#define NOMINAL_HZ 1500

static nxdn_link link;
static radio_dv_cfg_t cfg;
static bool selected, rx_on, tx_on, ptt_up, keyed;
static uint32_t tx_calls, tx_blocks, last_frames, last_frame_ms;
static uint8_t tx_reason;
/* receive voice, paced to the sink */
static uint8_t vq[16][9];
static uint8_t vq_head, vq_tail;
static bool v_open;
static uint32_t v_next_ms;

static const hal_audio_tap_t *tap(void)
{
    const hal_audio_tap_t *t = board.audio_tap;
    if (t == NULL || t->fm_rx_flat == NULL || t->fm_rx_read == NULL || t->fm_tx_inject == NULL ||
        (t->caps & (AUDIO_TAP_FM_TX_FLAT | AUDIO_TAP_FM_RX_FLAT)) != (AUDIO_TAP_FM_TX_FLAT | AUDIO_TAP_FM_RX_FLAT))
        return NULL;
    return t;
}

/* call layer -> pacing queue (the link calls this from radio_nxdn_service) */
static void slot_in(void *ctx, const uint8_t slot[9])
{
    (void)ctx;
    if ((uint8_t)(vq_head - vq_tail) >= sizeof vq / sizeof vq[0]) return;   /* never with the pacing below */
    memcpy(vq[vq_head % (sizeof vq / sizeof vq[0])], slot, 9);
    vq_head++;
}

static void voice_out(uint32_t now_ms)
{
    const dmr_voice_sink_t *sink = ambeVoiceSink();
    uint8_t queued = (uint8_t)(vq_head - vq_tail);
    if (!v_open) {
        /* start once a frame is in: the sink primes three slots on top */
        if (queued < 4) return;
        v_open = true;
        v_next_ms = now_ms;
        if (sink->begin) sink->begin(NULL, sink->ctx);
    }
    while ((int32_t)(now_ms - v_next_ms) >= 0 && vq_tail != vq_head) {
        sink->frame(vq[vq_tail % (sizeof vq / sizeof vq[0])], sink->ctx);
        vq_tail++;
        v_next_ms += 20;
    }
    if (vq_tail == vq_head && !link.rx.playing && (int32_t)(now_ms - v_next_ms) >= 0) {
        v_open = false;
        if (sink->end) sink->end(sink->ctx);
    }
}

static bool rx_start(void)
{
    const hal_audio_tap_t *t = tap();
    if (!board.radio->rx(cfg.rx_hz, RADIO_BW_12K5, NULL) || !t->fm_rx_flat(true)) return false;
    nxdn_link_rx_restart(&link);
    rx_on = true;
    return true;
}

static void rx_stop(void)
{
    if (rx_on) tap()->fm_rx_flat(false);
    rx_on = false;
}

bool radio_nxdn_select(const radio_dv_cfg_t *c)
{
    if (selected) {
        if (tx_on) {
            tap()->fm_tx_inject(NULL, AUDIO_TAP_FLAT);
            nxdn_link_tx_abort(&link);
            board.radio->tx_off();
            tx_on = keyed = false;
        }
        rx_stop();
        if (v_open && ambeVoiceSink()->end) ambeVoiceSink()->end(ambeVoiceSink()->ctx);
        v_open = false;
        ambeVoiceStop();
        selected = false;
    }
    if (c == NULL) return true;
    const hal_audio_tap_t *t = tap();
    if (c->mode != RADIO_MODE_NXDN || t == NULL || board.vocoder == NULL) return false;
    cfg = *c;
    nxdn_audio_path path = {
        t->flat_rx_hpf_hz, t->flat_rx_deemph_us, t->flat_tx_hpf_hz,
        t->fm_tx_nominal_peak > 0 ? (int32_t)t->fm_tx_nominal_peak * 256 / NOMINAL_HZ : 256,
    };
    /* RAN 0 on receive hears every RAN; group 0 (hear all) every group */
    nxdn_link_init(&link, &path, (uint8_t)(cfg.nac_rx & 63u), cfg.hear_all ? 0 : cfg.talkgroup, (uint16_t)cfg.unit_id,
                   slot_in, NULL);
    vq_head = vq_tail = 0;
    last_frames = 0;
    last_frame_ms = 0;
    if (!rx_start()) return false;
    selected = true;
    return true;
}

static bool next_slot(void *ctx, uint8_t slot[9])
{
    (void)ctx;
    const dmr_voice_source_t *src = ambeVoiceSource();
    return !ptt_up && src->next(slot, src->ctx);
}

/* The board's interrupt, 8 kHz samples */
static void pull(int16_t *pcm, uint16_t n)
{
    nxdn_link_pull(&link, pcm, n);
}

static void tx_finish(uint8_t reason)
{
    const dmr_voice_source_t *src = ambeVoiceSource();
    tap()->fm_tx_inject(NULL, AUDIO_TAP_FLAT);
    nxdn_link_tx_abort(&link);
    board.radio->tx_off();
    if (src->end) src->end(src->ctx);
    tx_on = keyed = false;
    tx_reason = reason;
    tx_calls++;
    rx_start();   /* back to receive on the channel */
}

bool radio_nxdn_ptt(bool down)
{
    if (!selected) return false;
    if (!down) {
        if (tx_on) {
            ptt_up = true;
            nxdn_link_tx_end(&link);   /* the release frame follows; the service drops the carrier after it */
        }
        return true;
    }
    if (tx_on) return true;
    /* The radio core checked the TX rules; the board checks band and power once more and keys */
    rx_stop();
    if (!board.radio->tx(cfg.tx_hz, RADIO_BW_12K5, cfg.power, NULL)) {
        tx_reason = NT_DENIED;
        tx_calls++;
        rx_start();
        return false;
    }
    const dmr_voice_source_t *src = ambeVoiceSource();
    if (src->begin) src->begin(src->ctx);
    nxdn_tx_cfg t = { (uint8_t)(cfg.nac_tx & 63u), (uint16_t)cfg.unit_id, cfg.talkgroup, false, false, NXDN_DIR_INBOUND };
    nxdn_link_tx_begin(&link, &t);
    nxdn_link_tx_fill(&link, next_slot, NULL);
    ptt_up = false;
    tx_on = true;
    tx_blocks = 0;
    if (!tap()->fm_tx_inject(pull, AUDIO_TAP_FLAT)) {
        tx_finish(NT_DENIED);
        return false;
    }
    keyed = true;
    return true;
}

void radio_nxdn_service(uint32_t now_ms)
{
    if (!selected) return;
    if (tx_on) {
        nxdn_link_tx_fill(&link, next_slot, NULL);
        tx_blocks = link.tx_frames;
        if (ptt_up && nxdn_link_tx_done(&link)) tx_finish(NT_TX_COMPLETE);
        return;
    }
    if (!rx_on) return;
    int16_t buf[128];
    for (unsigned guard = 0; guard < 16; guard++) {
        uint16_t n = tap()->fm_rx_read(buf, sizeof buf / sizeof buf[0]);
        if (n == 0) break;
        nxdn_link_rx_audio(&link, buf, n);
    }
    if (link.rx.frames != last_frames) {
        /* a frame with a LICH this channel can carry: activity (busy lockout, scan) */
        last_frames = link.rx.frames;
        last_frame_ms = now_ms != 0 ? now_ms : 1;
    }
    voice_out(now_ms);
}

void radio_nxdn_status(radio_dv_status_t *o, uint32_t now_ms)
{
    memset(o, 0, sizeof *o);
    if (!selected) return;
    const nxdn_rx *rx = &link.rx;
    bool recent = last_frame_ms && now_ms - last_frame_ms < RADIO_DV_BUSY_MS;
    o->active = rx_on;
    o->sync = recent;
    o->sync_ours = recent && (cfg.nac_rx == 0 || rx->last_ran == (cfg.nac_rx & 63u));
    o->playing = recent && rx->playing && !tx_on;
    o->encrypted = rx->have_msg && rx->msg.cipher_type != 0;
    o->emergency = rx->have_msg && (rx->msg.cc_option & 0x80u) != 0;
    o->nac = rx->last_ran;
    o->talkgroup = rx->have_msg ? rx->msg.destination : 0;
    o->source = rx->have_msg ? rx->msg.source : 0;
    o->transmitting = tx_on;
    o->keyed = keyed;
    o->tx_calls = tx_calls;
    o->tx_reason = tx_reason;
    o->tx_blocks = tx_blocks;
}
#endif
