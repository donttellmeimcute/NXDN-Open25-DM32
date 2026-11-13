/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "nxdn_call.h"
#include <string.h>

/* DMR's AMBE+2 silence frame; NXDN carries the same 72-bit air frame. */
const uint8_t nxdn_silence_slot[9] = { 0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B };

/* --- TX --- */
static void start_frame(uint8_t f[NXDN_FRAME_BYTES], const nxdn_tx *tx, uint8_t fct, uint8_t option)
{
    memset(f, 0, NXDN_FRAME_BYTES);
    nxdn_put_fsw(f);
    nxdn_lich l = { NXDN_RFCT_RDCH, fct, option, tx->cfg.direction };
    nxdn_lich_put(f, nxdn_lich_pack(&l));
}

static void control_frame(nxdn_tx *tx, uint8_t f[NXDN_FRAME_BYTES], uint8_t type)
{
    start_frame(f, tx, NXDN_USC_SACCH_NS, NXDN_STEAL_FACCH);
    nxdn_sacch s = { tx->cfg.ran, NXDN_SF_SINGLE, (uint32_t)NXDN_MSG_IDLE << 10 };
    nxdn_sacch_encode(&s, f);
    uint8_t l3[NXDN_LAYER3_BYTES];
    memcpy(l3, tx->layer3, sizeof l3);
    l3[0] = (uint8_t)((l3[0] & 0xC0u) | type);
    nxdn_facch1_encode(l3, f, 0);
    nxdn_facch1_encode(l3, f, 1);
    nxdn_scramble(f);
}

void nxdn_tx_begin(nxdn_tx *tx, const nxdn_tx_cfg *cfg, unsigned header_frames)
{
    memset(tx, 0, sizeof *tx);
    tx->cfg = *cfg;
    nxdn_call_msg m = { NXDN_MSG_VCALL, (uint8_t)(cfg->emergency ? 0x80 : 0), cfg->individual ? NXDN_CALL_INDIVIDUAL : NXDN_CALL_GROUP,
                        0, cfg->source, cfg->destination, 0, 0 };
    nxdn_layer3_encode(&m, tx->layer3);
    tx->state = NXDN_TX_HEADER;
    tx->headers_left = (uint8_t)(header_frames ? header_frames : 1);
}

int nxdn_tx_voice(nxdn_tx *tx, const uint8_t slot[9])
{
    if ((tx->state != NXDN_TX_HEADER && tx->state != NXDN_TX_VOICE) || tx->nslots >= 4) return 0;
    memcpy(tx->slots[tx->nslots++], slot, 9);
    return 1;
}

void nxdn_tx_end(nxdn_tx *tx)
{
    if (tx->state == NXDN_TX_HEADER || tx->state == NXDN_TX_VOICE) tx->state = NXDN_TX_ENDING;
}

static void voice_frame(nxdn_tx *tx, uint8_t f[NXDN_FRAME_BYTES])
{
    start_frame(f, tx, NXDN_USC_SACCH_SS, NXDN_STEAL_NONE);
    /* The VCALL's first 72 bits in four parts: part k has structure 3 - k */
    uint32_t chunk = 0;
    for (unsigned i = 0; i < 18; i++) chunk = chunk << 1 | nxdn_bit(tx->layer3, tx->part * 18u + i);
    nxdn_sacch s = { tx->cfg.ran, (uint8_t)(3u - tx->part), chunk };
    nxdn_sacch_encode(&s, f);
    tx->part = (uint8_t)((tx->part + 1u) & 3u);
    for (unsigned k = 0; k < 4; k++) {
        bool have = k < tx->nslots;
        nxdn_voice_put(f, k, have ? tx->slots[k] : nxdn_silence_slot);
        tx->padded_slots += !have;
    }
    tx->nslots = 0;
    tx->voice_frames++;
    nxdn_scramble(f);
}

int nxdn_tx_frame(nxdn_tx *tx, uint8_t f[NXDN_FRAME_BYTES])
{
    switch (tx->state) {
    case NXDN_TX_HEADER:
        control_frame(tx, f, NXDN_MSG_VCALL);
        if (!--tx->headers_left) tx->state = NXDN_TX_VOICE;
        break;
    case NXDN_TX_VOICE:
        if (tx->nslots < 4) return 0;
        voice_frame(tx, f);
        break;
    case NXDN_TX_ENDING:
        if (tx->headers_left) {          /* released during the header: still announce the call */
            control_frame(tx, f, NXDN_MSG_VCALL);
            tx->headers_left--;
            break;
        }
        if (tx->nslots) {                /* the partial frame, padded with silence */
            voice_frame(tx, f);
            break;
        }
        tx->state = NXDN_TX_RELEASE;
        /* fall through */
    case NXDN_TX_RELEASE:
        control_frame(tx, f, NXDN_MSG_TX_REL);
        tx->state = NXDN_TX_DONE;
        break;
    default:
        return -1;
    }
    tx->frames++;
    return 1;
}

/* --- RX --- */
enum { FSW_MAX_ERRORS = 2, FLYWHEEL_FRAMES = 3 };

void nxdn_rx_init(nxdn_rx *rx, uint8_t ran, uint16_t group, uint16_t unit, nxdn_slot_sink sink, void *ctx)
{
    memset(rx, 0, sizeof *rx);
    rx->ran = ran;
    rx->group = group;
    rx->unit = unit;
    rx->sink = sink;
    rx->ctx = ctx;
}

static void end_call(nxdn_rx *rx)
{
    rx->in_call = rx->have_msg = rx->admitted = rx->playing = false;
    rx->part_mask = 0;
}

void nxdn_rx_reset(nxdn_rx *rx)
{
    end_call(rx);
    rx->state = NXDN_RX_HUNT;
    rx->shift = 0;
    rx->fsw_hits = 0;
    rx->tentative = false;
    rx->pos = 0;
    rx->misses = 0;
}

static unsigned popcount(uint32_t v)
{
    unsigned n = 0;
    for (; v; v &= v - 1) n++;
    return n;
}

static bool admit(const nxdn_rx *rx, const nxdn_call_msg *m)
{
    if (m->call_type == NXDN_CALL_INDIVIDUAL) return rx->unit && m->destination == rx->unit;
    return !rx->group || m->destination == rx->group || m->destination == NXDN_GROUP_ALL;
}

static void take_msg(nxdn_rx *rx, const nxdn_call_msg *m)
{
    if (m->type == NXDN_MSG_TX_REL) {
        rx->releases++;
        end_call(rx);
        return;
    }
    if (rx->in_call && rx->have_msg && (m->source != rx->msg.source || m->destination != rx->msg.destination)) end_call(rx);
    if (!rx->in_call) rx->calls++;
    rx->in_call = rx->have_msg = true;
    rx->msg = *m;
    rx->admitted = admit(rx, m) && m->cipher_type == 0;
}

static void voice(nxdn_rx *rx, const uint8_t f[NXDN_FRAME_BYTES], unsigned first, unsigned count)
{
    rx->playing = rx->admitted;
    for (unsigned k = first; k < first + count; k++) {
        uint8_t slot[9];
        nxdn_voice_get(f, k, slot);
        if (rx->admitted && rx->sink) {
            rx->sink(rx->ctx, slot);
            rx->slots_played++;
        } else rx->slots_muted++;
    }
}

static void superframe(nxdn_rx *rx, const nxdn_sacch *s)
{
    unsigned part = 3u - (s->structure & 3u);
    /* The VCALL repeats unchanged in every superframe of a call, so its four parts may come
     * from two superframes: a receiver that joins in the middle of one needs four frames, not
     * up to seven. Each part has its own CRC (nxdn_sacch_decode). */
    rx->parts[part][0] = (uint8_t)(s->data >> 10);
    rx->parts[part][1] = (uint8_t)(s->data >> 2);
    rx->parts[part][2] = (uint8_t)(s->data << 6);
    rx->part_mask |= (uint8_t)(1u << part);
    rx->last_part = (uint8_t)part;
    if (rx->part_mask != 0xF) return;
    uint8_t l3[NXDN_LAYER3_BYTES] = { 0 };
    for (unsigned p = 0; p < 4; p++)
        for (unsigned i = 0; i < 18; i++) nxdn_set_bit(l3, p * 18 + i, nxdn_bit(rx->parts[p], i));
    nxdn_call_msg m;
    rx->part_mask = 0;
    if (!nxdn_layer3_decode(l3, &m) || m.type != NXDN_MSG_VCALL) return;
    if (!rx->have_msg) rx->late_entries++;
    take_msg(rx, &m);
}

static void process(nxdn_rx *rx)
{
    uint8_t *f = rx->frame;
    nxdn_scramble(f);
    rx->frames++;
    nxdn_lich l;
    uint8_t raw = nxdn_lich_get(f, NULL);
    if (!nxdn_lich_unpack(raw, &l) || (l.rfct != NXDN_RFCT_RDCH && l.rfct != NXDN_RFCT_RTCH)) {
        rx->bad_lich++;
        /* The LICH has only a parity bit. In a call, a damaged one between voice
         * frames is taken to be the voice frame's LICH it replaces. */
        if (!rx->in_call || !nxdn_lich_unpack(rx->last_lich, &l) || l.fct != NXDN_USC_SACCH_SS) return;
        rx->lich_assumed++;
    } else rx->last_lich = raw;
    nxdn_sacch s;
    bool sacch_ok = nxdn_sacch_decode(f, &s, NULL);
    if (sacch_ok) {
        rx->last_ran = s.ran;
        if (rx->ran && s.ran && s.ran != rx->ran) {
            rx->foreign_ran++;
            end_call(rx);
            return;
        }
    } else rx->bad_sacch++;
    if (l.fct == NXDN_USC_SACCH_NS) {
        /* header or release: VCALL / TX_REL in FACCH1 (either copy) */
        uint8_t l3[NXDN_LAYER3_BYTES];
        nxdn_call_msg m;
        if ((nxdn_facch1_decode(f, 0, l3, NULL) || nxdn_facch1_decode(f, 1, l3, NULL)) && nxdn_layer3_decode(l3, &m))
            take_msg(rx, &m);
        return;
    }
    if (l.fct != NXDN_USC_SACCH_SS) return;
    if (sacch_ok) superframe(rx, &s);
    uint8_t l3[NXDN_LAYER3_BYTES];
    nxdn_call_msg m;
    switch (l.option) {
    case NXDN_STEAL_NONE:
        voice(rx, f, 0, 4);
        break;
    case NXDN_STEAL_FACCH1_1:
        if (nxdn_facch1_decode(f, 0, l3, NULL) && nxdn_layer3_decode(l3, &m)) take_msg(rx, &m);
        voice(rx, f, 2, 2);
        break;
    case NXDN_STEAL_FACCH1_2:
        voice(rx, f, 0, 2);
        if (nxdn_facch1_decode(f, 1, l3, NULL) && nxdn_layer3_decode(l3, &m)) take_msg(rx, &m);
        break;
    default:
        for (unsigned h = 0; h < 2; h++)
            if (nxdn_facch1_decode(f, h, l3, NULL) && nxdn_layer3_decode(l3, &m)) take_msg(rx, &m);
        break;
    }
}

/* Sync on the FSW that ended `ago` dibits back: the frame restarts with the FSW
 * and the dibits since, which are still in the shift register (ago < 10) */
static void sync_at(nxdn_rx *rx, unsigned ago, unsigned errors)
{
    rx->state = NXDN_RX_SYNCED;
    rx->tentative = true;
    rx->sync_errors = (uint8_t)errors;
    memset(rx->frame, 0, sizeof rx->frame);
    nxdn_put_fsw(rx->frame);
    rx->pos = NXDN_FSW_BITS;
    for (unsigned k = ago; k-- > 0;) {
        uint8_t d = (uint8_t)((rx->shift >> (2 * k)) & 3u);
        nxdn_set_bit(rx->frame, rx->pos, d >> 1);
        nxdn_set_bit(rx->frame, rx->pos + 1, d & 1u);
        rx->pos = (uint16_t)(rx->pos + 2);
    }
    rx->misses = 0;
}

/* The LICH of the frame being collected (its 16 bits are in) is one a frame can carry */
static bool lich_plausible(const nxdn_rx *rx)
{
    uint8_t f[NXDN_FRAME_BYTES];
    memcpy(f, rx->frame, sizeof f);
    nxdn_scramble(f);
    nxdn_lich l;
    return nxdn_lich_unpack(nxdn_lich_get(f, NULL), &l) && (l.rfct == NXDN_RFCT_RDCH || l.rfct == NXDN_RFCT_RTCH);
}

int nxdn_rx_push_dibit(nxdn_rx *rx, uint8_t dibit)
{
    rx->shift = ((rx->shift << 2) | (dibit & 3u)) & 0xFFFFFu;
    unsigned errors = popcount(rx->shift ^ NXDN_FSW);
    rx->fsw_hits = (uint16_t)(rx->fsw_hits << 1 | (errors <= FSW_MAX_ERRORS));
    if (rx->state == NXDN_RX_HUNT) {
        if (rx->fsw_hits & 1u) sync_at(rx, 0, errors);
        return 0;
    }
    /* Until the next frame's FSW confirms a sync the hunt found, a cleaner FSW elsewhere
     * replaces it: the first may have been noise, its LICH by chance one a frame can carry */
    if (rx->tentative && errors < rx->sync_errors && rx->pos > NXDN_FSW_BITS) {
        rx->false_syncs++;
        sync_at(rx, 0, errors);
        return 0;
    }
    nxdn_set_bit(rx->frame, rx->pos, dibit >> 1);
    nxdn_set_bit(rx->frame, rx->pos + 1, dibit & 1u);
    rx->pos = (uint16_t)(rx->pos + 2);
    if (rx->tentative && rx->pos == NXDN_FSW_BITS + NXDN_LICH_BITS && !lich_plausible(rx)) {
        /* A sync the hunt found in noise (a 20-bit word within two errors turns up in
         * about 5 % of 100 ms of noise) must not hold the receiver while the real one
         * passes: without a LICH a frame can carry, hunt again from the FSWs seen since */
        rx->false_syncs++;
        rx->state = NXDN_RX_HUNT;
        rx->tentative = false;
        uint16_t since = rx->fsw_hits & ((1u << (NXDN_LICH_BITS / 2)) - 1u);
        for (unsigned k = NXDN_LICH_BITS / 2; k-- > 0;)
            if (since >> k & 1u) {
                sync_at(rx, k, FSW_MAX_ERRORS);
                break;
            }
        return 0;
    }
    if (rx->pos == NXDN_FSW_BITS) {
        /* The next frame's FSW follows at once. The flywheel rides over a few damaged
         * ones; after that sync is lost, the call ends and the hunt starts again. */
        if (errors > 2 * FSW_MAX_ERRORS) {
            if (++rx->misses > FLYWHEEL_FRAMES) {
                rx->sync_losses++;
                nxdn_rx_reset(rx);
                return 0;
            }
        } else {
            rx->misses = 0;
            rx->tentative = false;   /* a frame later, the FSW is there: confirmed */
        }
    }
    if (rx->pos < NXDN_FRAME_BITS) return 0;
    process(rx);
    rx->pos = 0;
    memset(rx->frame, 0, sizeof rx->frame);
    return 1;
}
