/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tsbk.h"
#include <string.h>

#define SYNC UINT64_C(0x5575f5ff77ff)
#define SYNC_INV (SYNC ^ UINT64_C(0xaaaaaaaaaaaa))
enum { SYNC_ERRORS = 4 };

static unsigned bit_get(const uint8_t *p, unsigned i) { return (p[i >> 3] >> (7 - (i & 7))) & 1u; }
static void bit_set(uint8_t *p, unsigned i, unsigned v)
{
    uint8_t m = (uint8_t)(0x80u >> (i & 7));
    p[i >> 3] = (uint8_t)(v ? p[i >> 3] | m : p[i >> 3] & ~m);
}
static unsigned popcount64(uint64_t v)
{
    unsigned n = 0;
    for (; v; v &= v - 1) n++;
    return n;
}

/* --- CRC-CCITT (x^16 + x^12 + x^5 + 1), inverted --- */
uint16_t p25t_crc16(const uint8_t *data, unsigned bits)
{
    uint32_t crc = 0;
    for (unsigned i = 0; i < bits + 16; i++) {
        unsigned b = i < bits ? bit_get(data, i) : 0;
        crc = ((crc << 1) | b) & 0x1ffffu;
        if (crc & 0x10000u) crc = (crc & 0xffffu) ^ 0x1021u;
    }
    return (uint16_t)(crc ^ 0xffffu);
}

void p25t_tsbk_seal(uint8_t t[P25T_TSBK_BYTES])
{
    uint16_t c = p25t_crc16(t, 80);
    t[10] = (uint8_t)(c >> 8);
    t[11] = (uint8_t)c;
}

bool p25t_tsbk_crc_ok(const uint8_t t[P25T_TSBK_BYTES])
{
    return p25t_crc16(t, 80) == (uint16_t)(t[10] << 8 | t[11]);
}

/* --- Rate 1/2 trellis: 48 data dibits and a zero flush dibit, each sent as a 4-bit
 * constellation point that depends on the previous dibit (the state) --- */
static const uint8_t next_word[4][4] = {
    { 0x2, 0xC, 0x1, 0xF },
    { 0xE, 0x0, 0xD, 0x3 },
    { 0x9, 0x7, 0xA, 0x4 },
    { 0x5, 0xB, 0x6, 0x8 },
};

/* Where bit k of trellis step g goes in the 196-bit block: four rows of 13, 12, 12 and 12 steps */
static unsigned interleave(unsigned g, unsigned k)
{
    static const uint8_t base[4] = { 0, 52, 100, 148 };
    return base[g % 4] + 4 * (g / 4) + k;
}

void p25t_trellis_encode(const uint8_t t[P25T_TSBK_BYTES], uint8_t block[25])
{
    memset(block, 0, 25);
    unsigned state = 0;
    for (unsigned g = 0; g < 49; g++) {
        unsigned in = g < 48 ? (t[g / 4] >> (6 - 2 * (g % 4))) & 3u : 0;
        unsigned w = next_word[state][in];
        for (unsigned k = 0; k < 4; k++) bit_set(block, interleave(g, k), (w >> (3 - k)) & 1u);
        state = in;
    }
}

int p25t_trellis_decode(const uint8_t block[25], uint8_t t[P25T_TSBK_BYTES])
{
    if (!block || !t) return -1;
    enum { BIG = 1 << 14 };
    uint16_t metric[4] = { 0, BIG, BIG, BIG };
    uint8_t from[49][4];
    for (unsigned g = 0; g < 49; g++) {
        unsigned w = 0;
        for (unsigned k = 0; k < 4; k++) w = w << 1 | bit_get(block, interleave(g, k));
        uint16_t next[4] = { BIG, BIG, BIG, BIG };
        for (unsigned s = 0; s < 4; s++) {
            if (metric[s] >= BIG) continue;
            for (unsigned in = 0; in < 4; in++) {
                if (g == 48 && in) continue;     /* the flush dibit is 0 */
                unsigned d = (unsigned)__builtin_popcount(w ^ next_word[s][in]);
                uint16_t m = (uint16_t)(metric[s] + d);
                if (m < next[in]) {
                    next[in] = m;
                    from[g][in] = (uint8_t)s;
                }
            }
        }
        memcpy(metric, next, sizeof metric);
    }
    /* trace back from state 0 (the flush) */
    uint8_t in[49];
    unsigned s = 0;
    for (unsigned g = 49; g-- > 0;) {
        in[g] = (uint8_t)s;
        s = from[g][s];
    }
    memset(t, 0, P25T_TSBK_BYTES);
    for (unsigned g = 0; g < 48; g++) t[g / 4] |= (uint8_t)(in[g] << (6 - 2 * (g % 4)));
    return metric[0];
}

/* --- TSDU frame --- */
/* Data bit i of a frame sits after the status symbols before it: one every 70 data bits */
static unsigned physical(unsigned i) { return i + 2 * (i / 70); }

unsigned p25t_tsdu_encode(uint16_t nac, const uint8_t tsbk[][P25T_TSBK_BYTES], unsigned n, uint8_t status,
                          uint8_t out[P25T_TSDU_MAX_BYTES])
{
    if (n < 1) n = 1;
    if (n > P25T_TSDU_MAX_BLOCKS) n = P25T_TSDU_MAX_BLOCKS;
    memset(out, 0, P25T_TSDU_MAX_BYTES);
    unsigned data = 0;
    for (unsigned i = 0; i < 48; i++, data++) bit_set(out, physical(data), (unsigned)(SYNC >> (47 - i)) & 1u);
    uint64_t nid = p25_nid_encode(nac, P25T_DUID_TSDU);
    for (unsigned i = 0; i < 64; i++, data++) bit_set(out, physical(data), (unsigned)(nid >> (63 - i)) & 1u);
    for (unsigned b = 0; b < n; b++) {
        uint8_t t[P25T_TSBK_BYTES], block[25];
        memcpy(t, tsbk[b], sizeof t);
        t[0] = (uint8_t)((t[0] & 0x7fu) | (b + 1 == n ? 0x80u : 0));
        p25t_tsbk_seal(t);
        p25t_trellis_encode(t, block);
        for (unsigned i = 0; i < P25T_BLOCK_BITS; i++, data++) bit_set(out, physical(data), bit_get(block, i));
    }
    unsigned bits = physical(data);
    /* the status symbols: dibit 35 of every 36 */
    for (unsigned d = 35; 2 * d < bits; d += 36) {
        bit_set(out, 2 * d, status >> 1);
        bit_set(out, 2 * d + 1, status & 1u);
    }
    return bits;
}

/* --- Receiver --- */
enum { HUNT, NID, BLOCKS };

void p25t_rx_init(p25t_rx *rx, uint16_t nac, p25t_tsbk_cb on_tsbk, p25t_frame_cb on_frame, p25t_status_cb on_status,
                  void *ctx)
{
    memset(rx, 0, sizeof *rx);
    rx->nac = nac;
    rx->on_tsbk = on_tsbk;
    rx->on_frame = on_frame;
    rx->on_status = on_status;
    rx->ctx = ctx;
}

void p25t_rx_reset(p25t_rx *rx)
{
    rx->state = HUNT;
    rx->shift = 0;
}

void p25t_rx_dibit(p25t_rx *rx, uint8_t dibit)
{
    dibit &= 3u;
    rx->dibits++;
    rx->shift = ((rx->shift << 2) | dibit) & UINT64_C(0xffffffffffff);
    if (rx->state != HUNT) {
        rx->pos++;
        if (rx->pos % 36 == 35) {
            /* a status symbol, not data */
            if (rx->on_status) rx->on_status(rx->ctx, rx->inverted ? dibit ^ 2u : dibit, rx->dibits - 1);
            return;
        }
    }
    if (popcount64(rx->shift ^ SYNC) <= SYNC_ERRORS || popcount64(rx->shift ^ SYNC_INV) <= SYNC_ERRORS) {
        /* a frame starts (a TSDU ends at its last block; voice frames run until the next sync) */
        rx->inverted = popcount64(rx->shift ^ SYNC) > SYNC_ERRORS;
        rx->state = NID;
        rx->pos = 23;           /* the sync's last dibit */
        rx->data_bits = 48;
        rx->nid_bits = 0;
        return;
    }
    if (rx->state == HUNT) return;
    if (rx->inverted) dibit ^= 2u;
    if (rx->state == NID) {
        rx->nid_bits = rx->nid_bits << 2 | dibit;
        rx->data_bits += 2;
        if (rx->data_bits < 112) return;
        p25_nid nid;
        if (p25_nid_decode_any(rx->nid_bits, &nid) || (rx->nac != P25_NAC_ANY && nid.nac != rx->nac)) {
            rx->bad_nid++;
            rx->state = HUNT;
            return;
        }
        rx->frames++;
        rx->frame_nac = nid.nac;
        if (nid.duid != P25T_DUID_TSDU) {
            rx->other_frames++;
            if (rx->on_frame) rx->on_frame(rx->ctx, nid.nac, nid.duid);
            rx->state = HUNT;
            return;
        }
        rx->tsdus++;
        rx->state = BLOCKS;
        rx->blocks = 0;
        memset(rx->block, 0, sizeof rx->block);
        return;
    }
    /* a block */
    unsigned i = rx->data_bits - 112 - (unsigned)rx->blocks * P25T_BLOCK_BITS;
    bit_set(rx->block, i, dibit >> 1);
    bit_set(rx->block, i + 1, dibit & 1u);
    rx->data_bits += 2;
    if (i + 2 < P25T_BLOCK_BITS) return;
    uint8_t t[P25T_TSBK_BYTES];
    rx->blocks++;
    if (p25t_trellis_decode(rx->block, t) < 0 || !p25t_tsbk_crc_ok(t)) {
        rx->bad_crc++;
        rx->state = HUNT;
        return;
    }
    rx->tsbks++;
    if (rx->on_tsbk) rx->on_tsbk(rx->ctx, rx->frame_nac, t);
    memset(rx->block, 0, sizeof rx->block);
    if ((t[0] & 0x80u) || rx->blocks >= P25T_TSDU_MAX_BLOCKS) rx->state = HUNT;
}
