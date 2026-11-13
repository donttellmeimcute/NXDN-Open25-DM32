/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Phase 2 channel coding: RS(63,35) errors and erasures, the shortened and punctured SACCH / FACCH codes with
 * their CRC-12, ESS, ISCH and DUID codes, voice bursts and scrambling round trips. */
#include "p2.h"
#include "p2_call.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t rnd = 0xC0FFEEu;
static uint32_t next(void) { rnd ^= rnd << 13; rnd ^= rnd >> 17; rnd ^= rnd << 5; return rnd; }

static void rs_checks(void)
{
    unsigned tried = 0;
    for (int k = 0; k < 3000; k++) {
        uint8_t cw[63], sent[63], eras[28];
        for (int i = 0; i < 35; i++) cw[i] = (uint8_t)(next() & 63);
        p2_rs_encode(cw);
        memcpy(sent, cw, 63);
        assert(p2_rs_decode(cw, NULL, 0) == 0);
        /* e errors and f erasures with 2e + f <= 28 */
        unsigned f = next() % 29, e = (28 - f) / 2;
        e = e ? next() % (e + 1) : 0;
        bool used[63] = { 0 };
        for (unsigned i = 0; i < f; i++) {
            unsigned p;
            do p = next() % 63; while (used[p]);
            used[p] = true;
            eras[i] = (uint8_t)p;
            cw[p] = (uint8_t)(next() & 63);
        }
        for (unsigned i = 0; i < e; i++) {
            unsigned p;
            do p = next() % 63; while (used[p]);
            used[p] = true;
            cw[p] ^= (uint8_t)(1 + next() % 63);
        }
        int r = p2_rs_decode(cw, eras, f);
        assert(r >= 0 && !memcmp(cw, sent, 63));
        tried++;
    }
    fprintf(stderr, "RS(63,35): %u code words, up to 2 errors + erasures = 28, all corrected\n", tried);
}

static void xcch_checks(void)
{
    for (int facch = 0; facch < 2; facch++) {
        unsigned bytes = facch ? P2_FACCH_BYTES : P2_SACCH_BYTES, cap = facch ? 9 : 11;
        unsigned ok = 0, fixed = 0;
        for (int k = 0; k < 500; k++) {
            uint8_t pdu[21], back[21], payload[160] = { 0 };
            for (unsigned i = 0; i < bytes; i++) pdu[i] = (uint8_t)next();
            p2_xcch_encode(facch, pdu, payload);
            assert(p2_xcch_decode(facch, payload, back) == 0 && !memcmp(pdu, back, bytes));
            /* symbol errors up to what the punctured code corrects: (sent parity - erased) / 2 */
            unsigned errs = k % (cap / 2 + 1);
            uint8_t hurt[160];
            memcpy(hurt, payload, 160);
            unsigned dib_n = facch ? 135 : 156, hit[16] = { 0 }, nh = 0;
            for (unsigned i = 0; i < errs; i++) {
                unsigned h = next() % (dib_n / 3);
                bool dup = false;
                for (unsigned j = 0; j < nh; j++) dup |= hit[j] == h;
                if (dup) continue;
                hit[nh++] = h;
            }
            /* a hex bit is 3 dibits: change one of them in each chosen hex bit */
            static const uint8_t runs_s[3][2] = { { 1, 36 }, { 38, 84 }, { 123, 36 } };
            static const uint8_t runs_f[4][2] = { { 1, 36 }, { 38, 31 }, { 90, 32 }, { 123, 36 } };
            for (unsigned j = 0; j < nh; j++) {
                unsigned d = 3 * hit[j] + next() % 3, n = facch ? 4 : 3;
                for (unsigned r = 0; r < n; r++) {
                    unsigned at = facch ? runs_f[r][0] : runs_s[r][0], len = facch ? runs_f[r][1] : runs_s[r][1];
                    if (d < len) { hurt[at + d] ^= (uint8_t)(1 + next() % 3); break; }
                    d -= len;
                }
            }
            int r = p2_xcch_decode(facch, hurt, back);
            if (r >= 0 && !memcmp(pdu, back, bytes)) ok++, fixed += (unsigned)r;
        }
        fprintf(stderr, "%s: 500 PDUs with 0-%u hex bit errors: %u decoded\n", facch ? "FACCH" : "SACCH", cap / 2, ok);
        assert(ok == 500);
        (void)fixed;
    }
}

/* A call on one channel through the stream receiver: sync from anywhere, MAC_PTT, every frame in order, the end */
static uint8_t got_v[400][9];
static unsigned n_got_v, n_ptt, n_end, n_active, n_enc;
static p2_mac last_ptt, last_end, last_active;
static void on_mac(void *c, uint8_t ch, const p2_mac *m)
{
    (void)c;
    (void)ch;
    if (m->opcode == P2_MAC_PTT) n_ptt++, last_ptt = *m;
    if (m->opcode == P2_MAC_END_PTT) n_end++, last_end = *m;
    if (m->opcode == P2_MAC_ACTIVE) n_active++, last_active = *m;
}
static void on_voice(void *c, uint8_t ch, const uint8_t v[9], bool enc)
{
    (void)c;
    (void)ch;
    n_enc += enc;
    if (n_got_v < 400) memcpy(got_v[n_got_v++], v, 9);
}

static void call_stream(uint8_t chan)
{
    uint8_t mask[P2_MASK_BYTES];
    p2_mask(0xBEE00, 0x3A5, 0x293, mask);
    p2_tx t0, t1;
    p2_tx *ch[2] = { &t0, &t1 };
    p2_tx_init(&t0, 0x293);
    p2_tx_init(&t1, 0x293);
    p2_rx rx;
    p2_rx_init(&rx, chan, 0xBEE00, 0x3A5, 0x293, on_mac, on_voice, NULL);
    n_got_v = n_ptt = n_end = n_active = n_enc = 0;
    /* noise first, a start in mid burst */
    for (int i = 0; i < 1234; i++) p2_rx_dibit(&rx, (uint8_t)(next() & 3));
    uint8_t sent[300][9];
    unsigned n_sent = 0, frames = 100;
    for (unsigned n = 0; n < 42; n++)
        for (unsigned b = 0; b < 12; b++) {
            /* the call starts once the receiver has had a superframe (idle bursts) to find the sync */
            if (n == 2 && b == 0) p2_tx_begin(ch[chan], 1234, 0xABCDE);
            /* 20 ms frames arrive as the call runs: 1.5 per burst on average */
            while (n >= 2 && n_sent < frames && n_sent * 2 < ((n - 2) * 12 + b) * 3 + 6) {
                for (int j = 0; j < 9; j++) sent[n_sent][j] = (uint8_t)next();
                if (!p2_tx_voice(ch[chan], sent[n_sent])) break;
                n_sent++;
            }
            if (n_sent == frames && !ch[chan]->ending) p2_tx_end(ch[chan]);
            uint8_t burst[180];
            p2_outbound_burst(ch, b, n, mask, burst);
            for (int i = 0; i < 180; i++) p2_rx_dibit(&rx, burst[i]);
        }
    assert(rx.locked && rx.known && rx.sync_losses == 0 && rx.isch_errors == 0);
    assert(n_ptt >= 1 && last_ptt.group == 1234 && last_ptt.source == 0xABCDE && last_ptt.algid == 0x80);
    assert(n_end >= 1 && last_end.group == 1234 && last_end.source == 0xABCDE && last_end.nac == 0x293);
    assert(n_active >= 1 && last_active.user && last_active.user_group == 1234 && last_active.user_source == 0xABCDE);
    /* every frame sent arrives in order; only silence pads the last 4V / 2V */
    assert(n_got_v >= frames && n_enc == 0);
    for (unsigned i = 0; i < frames; i++) assert(!memcmp(got_v[i], sent[i], 9));
    for (unsigned i = frames; i < n_got_v; i++) assert(!memcmp(got_v[i], p2_silence_vcw, 9));
    assert(t0.padded + t1.padded == n_got_v - frames);
    fprintf(stderr, "channel %u: %u frames through %u bursts, MAC_PTT x%u, MAC_ACTIVE x%u, MAC_END_PTT x%u\n", chan,
            frames, rx.bursts, n_ptt, n_active, n_end);
}

int main(void)
{
    rs_checks();
    xcch_checks();
    /* ISCH: every value back, with up to 7 bit errors; the S-ISCH */
    for (unsigned v = 0; v < 128; v++) {
        uint64_t cw = p2_isch_encode((uint8_t)v);
        uint8_t e;
        assert(p2_isch_decode(cw, &e) == (int)v && e == 0);
        for (int k = 0; k < 7; k++) cw ^= UINT64_C(1) << (next() % 40);
        assert(p2_isch_decode(cw, &e) == (int)v);
    }
    assert(p2_isch_decode(P2_S_ISCH, NULL) == -2);
    /* DUID: every value, one bit error */
    for (unsigned d = 0; d < 16; d++) {
        uint8_t cw = p2_duid_encode((uint8_t)d);
        assert(p2_duid_decode(cw) == (int)d);
        for (int b = 0; b < 8; b++) assert(p2_duid_decode((uint8_t)(cw ^ (1u << b))) == (int)d);
    }
    /* ESS */
    for (int k = 0; k < 200; k++) {
        uint8_t mi[9], mi2[9], hex[44], a2;
        uint16_t key2;
        for (int i = 0; i < 9; i++) mi[i] = (uint8_t)next();
        uint8_t alg = (uint8_t)next();
        uint16_t key = (uint16_t)next();
        p2_ess_encode(alg, key, mi, hex);
        for (int e = 0; e < 14; e++) hex[next() % 44] ^= (uint8_t)(1 + next() % 63);
        assert(p2_ess_decode(hex, &a2, &key2, mi2) >= 0 && a2 == alg && key2 == key && !memcmp(mi, mi2, 9));
    }
    /* bursts: voice and signalling, scrambled, back */
    uint8_t mask[P2_MASK_BYTES];
    p2_mask(0xBEE00, 0x3A5, 0x293, mask);
    for (unsigned b = 0; b < 12; b++) {
        uint8_t vcw[4][9], essb[4], payload[160] = { 0 }, burst[180], back[160], v2[4][9], e2[4];
        for (int i = 0; i < 4; i++) for (int j = 0; j < 9; j++) vcw[i][j] = (uint8_t)next();
        for (int i = 0; i < 4; i++) essb[i] = (uint8_t)(next() & 63);
        p2_4v_encode(vcw, essb, payload);
        p2_burst_build(p2_isch_encode(p2_isch_value(b & 3, b / 4, false, 0)), P2_DUID_4V, payload, mask, b, burst);
        assert(p2_burst_parse(burst, mask, b, back) == P2_DUID_4V);
        p2_4v_decode(back, v2, e2);
        assert(!memcmp(vcw, v2, sizeof vcw) && !memcmp(essb, e2, 4));
        /* the scrambled burst differs from the clear one */
        uint8_t clear[180];
        p2_burst_build(0, P2_DUID_4V, payload, NULL, b, clear);
        assert(memcmp(clear + 20, burst + 20, 160));
        uint8_t essa[28], ea2[28], w2[2][9];
        for (int i = 0; i < 28; i++) essa[i] = (uint8_t)(next() & 63);
        p2_2v_encode((const uint8_t(*)[9])vcw, essa, payload);
        p2_burst_build(P2_S_ISCH, P2_DUID_2V, payload, mask, b, burst);
        assert(p2_burst_parse(burst, mask, b, back) == P2_DUID_2V);
        p2_2v_decode(back, w2, ea2);
        assert(!memcmp(vcw, w2, sizeof w2) && !memcmp(essa, ea2, 28));
    }
    call_stream(0);
    call_stream(1);
    fprintf(stderr, "PASS P25 Phase 2 coding: RS(63,35), SACCH/FACCH + CRC-12, ESS, ISCH, DUID, 4V/2V bursts, scrambling\n");
    return 0;
}
