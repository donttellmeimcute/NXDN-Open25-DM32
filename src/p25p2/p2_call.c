/* SPDX-License-Identifier: GPL-3.0-or-later */
/* p2_call.h: Phase 2 group voice on one logical channel. */
#include "p2_call.h"
#include <string.h>

const uint8_t p2_silence_vcw[P2_VCW_BYTES] = { 0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B };

/* ---- sender ---- */

void p2_tx_init(p2_tx *tx, uint16_t nac)
{
    memset(tx, 0, sizeof *tx);
    tx->nac = nac;
}

void p2_tx_begin(p2_tx *tx, uint16_t group, uint32_t source)
{
    static const uint8_t no_mi[9];
    tx->state = P2_TX_PTT;
    tx->left = P2_TX_PTT_BURSTS;
    tx->group = group;
    tx->source = source;
    tx->ending = false;
    tx->cycle = 0;
    tx->head = tx->count = 0;
    p2_ess_encode(P2_ALGID_CLEAR, 0, no_mi, tx->ess);
}

bool p2_tx_voice(p2_tx *tx, const uint8_t vcw[P2_VCW_BYTES])
{
    if (tx->state == P2_TX_IDLE || tx->ending || tx->count >= P2_TX_QUEUE) return false;
    memcpy(tx->q[(tx->head + tx->count) % P2_TX_QUEUE], vcw, P2_VCW_BYTES);
    tx->count++;
    return true;
}

void p2_tx_end(p2_tx *tx)
{
    if (tx->state == P2_TX_PTT) {
        /* nothing said yet: straight to the end */
        tx->state = P2_TX_END;
        tx->left = P2_TX_END_BURSTS;
    }
    tx->ending = true;
}

static void pop(p2_tx *tx, uint8_t vcw[P2_VCW_BYTES])
{
    if (tx->count) {
        memcpy(vcw, tx->q[tx->head], P2_VCW_BYTES);
        tx->head = (uint8_t)((tx->head + 1) % P2_TX_QUEUE);
        tx->count--;
        tx->voice_frames++;
    } else {
        memcpy(vcw, p2_silence_vcw, P2_VCW_BYTES);
        tx->padded++;
    }
}

static uint8_t mac_burst(p2_tx *tx, bool sacch, uint8_t offset, uint8_t payload[P2_PAYLOAD_DIBITS])
{
    p2_mac m = { 0 };
    m.offset = offset;
    switch (tx->state) {
    case P2_TX_PTT:
        m.opcode = P2_MAC_PTT;
        m.algid = P2_ALGID_CLEAR;
        m.source = tx->source;
        m.group = tx->group;
        break;
    case P2_TX_VOICE:
    case P2_TX_HANG:
        m.opcode = tx->state == P2_TX_VOICE ? P2_MAC_ACTIVE : P2_MAC_HANGTIME;
        m.user = true;
        m.svc = 0x04;   /* priority 4, clear, group */
        m.user_group = tx->group;
        m.user_source = tx->source;
        break;
    case P2_TX_END:
        m.opcode = P2_MAC_END_PTT;
        m.nac = tx->nac;
        m.source = tx->source;
        m.group = tx->group;
        break;
    default:
        m.opcode = P2_MAC_IDLE;
        break;
    }
    uint8_t pdu[P2_SACCH_BYTES];
    p2_mac_build(&m, pdu, sacch ? P2_SACCH_BYTES : P2_FACCH_BYTES);
    p2_xcch_encode(!sacch, pdu, payload);
    return sacch ? P2_DUID_SACCH_S : P2_DUID_FACCH_S;
}

uint8_t p2_tx_burst(p2_tx *tx, unsigned b, uint8_t payload[P2_PAYLOAD_DIBITS])
{
    memset(payload, 0, P2_PAYLOAD_DIBITS);
    tx->bursts++;
    if (b >= 10) return mac_burst(tx, true, tx->state == P2_TX_VOICE ? (uint8_t)((5 - tx->cycle) % 5) : 0, payload);
    switch (tx->state) {
    case P2_TX_PTT: {
        /* offset: voice bursts of this channel between this one and the first 4V */
        uint8_t duid = mac_burst(tx, false, (uint8_t)(tx->left - 1), payload);
        if (--tx->left == 0) {
            tx->state = P2_TX_VOICE;
            tx->cycle = 0;
        }
        return duid;
    }
    case P2_TX_VOICE: {
        if (tx->ending && !tx->count) {
            tx->state = P2_TX_END;
            tx->left = P2_TX_END_BURSTS;
            return p2_tx_burst(tx, b, payload);
        }
        uint8_t duid;
        if (tx->cycle < 4) {
            uint8_t v[4][P2_VCW_BYTES];
            for (unsigned k = 0; k < 4; k++) pop(tx, v[k]);
            p2_4v_encode((const uint8_t(*)[P2_VCW_BYTES])v, tx->ess + 4 * tx->cycle, payload);
            duid = P2_DUID_4V;
        } else {
            uint8_t v[2][P2_VCW_BYTES];
            for (unsigned k = 0; k < 2; k++) pop(tx, v[k]);
            p2_2v_encode((const uint8_t(*)[P2_VCW_BYTES])v, tx->ess + 16, payload);
            duid = P2_DUID_2V;
        }
        tx->cycle = (uint8_t)((tx->cycle + 1) % 5);
        return duid;
    }
    case P2_TX_END: {
        uint8_t duid = mac_burst(tx, false, 0, payload);
        if (--tx->left == 0) {
            tx->state = P2_TX_HANG;
            tx->left = P2_TX_HANG_BURSTS;
        }
        return duid;
    }
    case P2_TX_HANG: {
        uint8_t duid = mac_burst(tx, false, 0, payload);
        if (--tx->left == 0) tx->state = P2_TX_IDLE;
        return duid;
    }
    default:
        return mac_burst(tx, false, 0, payload);
    }
}

void p2_outbound_burst(p2_tx *ch[2], unsigned b, unsigned n, const uint8_t mask[P2_MASK_BYTES],
                       uint8_t out[P2_BURST_DIBITS])
{
    uint8_t payload[P2_PAYLOAD_DIBITS];
    uint8_t duid = p2_tx_burst(ch[p2_burst_channel(b)], b, payload);
    uint64_t isch = p2_burst_has_sync(b) ? P2_S_ISCH
                                         : p2_isch_encode(p2_isch_value((uint8_t)(b & 3u), (uint8_t)(b / 4), false,
                                                                        (uint8_t)(n & 3u)));
    p2_burst_build(isch, duid, payload, mask, b, out);
}

/* ---- receiver ---- */

static void channel_reset(p2_rx *rx)
{
    for (unsigned c = 0; c < 2; c++) {
        memset(rx->ch[c].ess, 0, sizeof rx->ch[c].ess);
        rx->ch[c].v4 = 0;
        rx->ch[c].algid = P2_ALGID_CLEAR;
        rx->ch[c].encrypted = false;
    }
}

void p2_rx_init(p2_rx *rx, uint8_t follow, uint32_t wacn, uint16_t sysid, uint16_t nac, p2_mac_cb on_mac,
                p2_voice_cb on_voice, void *ctx)
{
    memset(rx, 0, sizeof *rx);
    rx->follow = follow;
    rx->scrambled = true;
    p2_mask(wacn, sysid, nac, rx->mask);
    rx->on_mac = on_mac;
    rx->on_voice = on_voice;
    rx->ctx = ctx;
    channel_reset(rx);
}

void p2_rx_reset(p2_rx *rx)
{
    rx->shift = 0;
    rx->locked = rx->known = false;
    rx->pos = 0;
    rx->misses = 0;
    channel_reset(rx);
}

static unsigned popcount64(uint64_t v)
{
    unsigned n = 0;
    for (; v; v &= v - 1) n++;
    return n;
}

void p2_rx_burst(p2_rx *rx, unsigned b, const uint8_t burst[P2_BURST_DIBITS])
{
    uint8_t c = p2_burst_channel(b);
    if (rx->follow != P2_RX_FOLLOW_BOTH && c != rx->follow) return;
    uint8_t payload[P2_PAYLOAD_DIBITS];
    int duid = p2_burst_parse(burst, rx->scrambled ? rx->mask : NULL, b, payload);
    rx->bursts++;
    switch (duid) {
    case P2_DUID_4V: {
        uint8_t v[4][P2_VCW_BYTES], essb[4];
        p2_4v_decode(payload, v, essb);
        if (rx->ch[c].v4 < 4) memcpy(rx->ch[c].ess + 4 * rx->ch[c].v4, essb, 4);
        rx->ch[c].v4++;
        for (unsigned k = 0; k < 4; k++) {
            rx->vcws++;
            if (rx->on_voice) rx->on_voice(rx->ctx, c, v[k], rx->ch[c].encrypted);
        }
        break;
    }
    case P2_DUID_2V: {
        uint8_t v[2][P2_VCW_BYTES];
        p2_2v_decode(payload, v, rx->ch[c].ess + 16);
        for (unsigned k = 0; k < 2; k++) {
            rx->vcws++;
            if (rx->on_voice) rx->on_voice(rx->ctx, c, v[k], rx->ch[c].encrypted);
        }
        /* the encryption sync takes effect after the 2V that completes it (as OP25) */
        if (rx->ch[c].v4 == 4) {
            uint8_t alg, mi[9];
            uint16_t key;
            if (p2_ess_decode(rx->ch[c].ess, &alg, &key, mi) >= 0) {
                rx->ch[c].algid = alg;
                rx->ch[c].encrypted = alg != P2_ALGID_CLEAR;
            }
        }
        rx->ch[c].v4 = 0;
        break;
    }
    case P2_DUID_SACCH_S:
    case P2_DUID_SACCH:
    case P2_DUID_FACCH_S:
    case P2_DUID_FACCH: {
        bool facch = duid == P2_DUID_FACCH_S || duid == P2_DUID_FACCH;
        uint8_t pdu[P2_SACCH_BYTES];
        p2_mac m;
        if (p2_xcch_decode(facch, payload, pdu) < 0 || !p2_mac_parse(pdu, facch ? P2_FACCH_BYTES : P2_SACCH_BYTES, &m)) {
            rx->mac_bad++;
            break;
        }
        rx->mac_ok++;
        if (m.opcode == P2_MAC_PTT) {
            rx->ch[c].algid = m.algid;
            rx->ch[c].encrypted = m.algid != P2_ALGID_CLEAR;
            rx->ch[c].v4 = 0;
        } else if (m.user && (m.svc & P2_SVC_ENCRYPTED)) {
            rx->ch[c].encrypted = true;
        } else if (m.opcode == P2_MAC_END_PTT || m.opcode == P2_MAC_IDLE) {
            rx->ch[c].algid = P2_ALGID_CLEAR;
            rx->ch[c].encrypted = false;
            rx->ch[c].v4 = 0;
        }
        if (rx->on_mac) rx->on_mac(rx->ctx, c, &m);
        break;
    }
    default:
        break;
    }
}

void p2_rx_dibit(p2_rx *rx, uint8_t dibit)
{
    rx->shift = ((rx->shift << 2) | (dibit & 3u)) & UINT64_C(0xFFFFFFFFFF);
    if (!rx->locked) {
        if (popcount64(rx->shift ^ P2_S_ISCH) <= 4) {
            rx->locked = true;
            rx->known = false;
            rx->misses = 0;
            for (unsigned i = 0; i < P2_ISCH_DIBITS; i++) rx->buf[i] = (uint8_t)((rx->shift >> (38 - 2 * i)) & 3u);
            rx->pos = P2_ISCH_DIBITS;
        }
        return;
    }
    rx->buf[rx->pos++] = dibit & 3u;
    if (rx->pos < P2_BURST_DIBITS) return;
    rx->pos = 0;
    uint64_t isch = 0;
    for (unsigned i = 0; i < P2_ISCH_DIBITS; i++) isch = isch << 2 | rx->buf[i];
    uint8_t errs = 0;
    int v = p2_isch_decode(isch, &errs);
    int at = -1;
    if (v >= 0 && (v >> 5) < 2 && ((v >> 3) & 3) < 3) at = ((v >> 3) & 3) * 4 + (v >> 5);
    if (!rx->known) {
        if (at >= 0) {
            rx->known = true;
            rx->burst = (uint8_t)at;
            rx->misses = 0;
        } else if (v != -2 && ++rx->misses > 4) {
            rx->locked = false;
            rx->sync_losses++;
            return;
        }
        if (!rx->known) return;
    } else {
        unsigned expect = (rx->burst + 1u) % P2_SUPERFRAME_BURSTS;
        bool ok = (v == -2 && p2_burst_has_sync(expect)) || at == (int)expect;
        if (at >= 0 && at != (int)expect) expect = (unsigned)at;   /* the I-ISCH is a strong code: follow it */
        rx->burst = (uint8_t)expect;
        if (ok) {
            rx->misses = 0;
        } else {
            rx->isch_errors++;
            if (++rx->misses > 8) {
                rx->locked = rx->known = false;
                rx->sync_losses++;
                channel_reset(rx);
                return;
            }
        }
    }
    p2_rx_burst(rx, rx->burst, rx->buf);
}
