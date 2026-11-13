/* SPDX-License-Identifier: GPL-3.0-or-later */
/* NXDN48 frame layer. The convolutional code, Viterbi decoder, puncturing,
 * interleaving and CRC generators follow MMDVMHost (Copyright (C) 2018,2023,
 * 2025 by Jonathan Naylor G4KLX, GPL-2.0-or-later; NXDNConvolution.cpp,
 * NXDNCRC.cpp, NXDNSACCH.cpp, NXDNFACCH1.cpp, NXDNLICH.cpp at the commit in
 * docs/protocol-sources.json), rewritten in C for this library. */
#include "nxdn.h"
#include <string.h>

void nxdn_put_fsw(uint8_t f[NXDN_FRAME_BYTES])
{
    for (unsigned i = 0; i < NXDN_FSW_BITS; i++) nxdn_set_bit(f, i, (NXDN_FSW >> (19 - i)) & 1u);
}

uint8_t nxdn_preamble_dibit(unsigned i, unsigned n)
{
    static const uint8_t pre[3] = { 0x57u, 0x75u, 0xFDu };
    if (i + 12u >= n) {
        unsigned k = i + 12u - n;                       /* 0..11 within 0x5775FD */
        return (uint8_t)((pre[k / 4u] >> (6u - 2u * (k % 4u))) & 3u);
    }
    /* dotting, ending on -3 where the preamble's +3 starts: count back from it */
    return (n - 12u - 1u - i) % 2u ? 1u : 3u;
}

/* --- LICH --- */
uint8_t nxdn_lich_pack(const nxdn_lich *l)
{
    uint8_t v = (uint8_t)((l->rfct & 3u) << 6 | (l->fct & 3u) << 4 | (l->option & 3u) << 2 | (l->direction & 1u) << 1);
    uint8_t p = (uint8_t)(((v >> 7) ^ (v >> 6) ^ (v >> 5) ^ (v >> 4)) & 1u);
    return (uint8_t)(v | p);
}

bool nxdn_lich_unpack(uint8_t raw, nxdn_lich *l)
{
    l->rfct = (raw >> 6) & 3u;
    l->fct = (raw >> 4) & 3u;
    l->option = (raw >> 2) & 3u;
    l->direction = (raw >> 1) & 1u;
    return nxdn_lich_pack(l) == raw;
}

void nxdn_lich_put(uint8_t f[NXDN_FRAME_BYTES], uint8_t raw)
{
    for (unsigned i = 0; i < 8; i++) {
        nxdn_set_bit(f, NXDN_FSW_BITS + 2 * i, (raw >> (7 - i)) & 1u);
        nxdn_set_bit(f, NXDN_FSW_BITS + 2 * i + 1, 1);
    }
}

uint8_t nxdn_lich_get(const uint8_t f[NXDN_FRAME_BYTES], unsigned *off_bits)
{
    uint8_t raw = 0;
    unsigned off = 0;
    for (unsigned i = 0; i < 8; i++) {
        raw = (uint8_t)(raw << 1 | nxdn_bit(f, NXDN_FSW_BITS + 2 * i));
        off += !nxdn_bit(f, NXDN_FSW_BITS + 2 * i + 1);
    }
    if (off_bits) *off_bits = off;
    return raw;
}

/* --- Scrambler: PN9 x^9 + x^4 + 1, seed 0xE4, one bit per symbol on the
 * symbol's sign (the dibit's first bit), from the symbol after the FSW --- */
static uint8_t mask[NXDN_FRAME_BYTES];
static bool mask_ready;

const uint8_t *nxdn_scrambler_mask(void)
{
    if (!mask_ready) {
        unsigned s = 0xE4;
        memset(mask, 0, sizeof mask);
        for (unsigned sym = NXDN_FSW_BITS / 2; sym < NXDN_FRAME_SYMBOLS; sym++) {
            nxdn_set_bit(mask, 2 * sym, s & 1u);
            unsigned fb = (s ^ (s >> 4)) & 1u;
            s = (s >> 1) | (fb << 8);
        }
        mask_ready = true;
    }
    return mask;
}

void nxdn_scramble(uint8_t f[NXDN_FRAME_BYTES])
{
    const uint8_t *m = nxdn_scrambler_mask();
    for (unsigned i = 0; i < NXDN_FRAME_BYTES; i++) f[i] ^= m[i];
}

/* --- CRCs (initial value all ones, MSB first) --- */
static unsigned crc(const uint8_t *b, unsigned bits, unsigned width, unsigned poly)
{
    unsigned top = 1u << (width - 1), all = (1u << width) - 1u, c = all;
    for (unsigned i = 0; i < bits; i++) {
        unsigned in = nxdn_bit(b, i), hi = (c & top) != 0;
        c = (c << 1) & all;
        if (in ^ hi) c ^= poly;
    }
    return c & all;
}
uint8_t nxdn_crc6(const uint8_t *b, unsigned bits) { return (uint8_t)crc(b, bits, 6, 0x27); }
uint16_t nxdn_crc12(const uint8_t *b, unsigned bits) { return (uint16_t)crc(b, bits, 12, 0x80F); }
uint16_t nxdn_crc15(const uint8_t *b, unsigned bits) { return (uint16_t)crc(b, bits, 15, 0x4CC5); }

/* --- Convolutional code --- */
void nxdn_conv_encode(const uint8_t *in, unsigned n, uint8_t *out)
{
    unsigned d1 = 0, d2 = 0, d3 = 0, d4 = 0, k = 0;
    for (unsigned i = 0; i < n; i++) {
        unsigned d = nxdn_bit(in, i);
        nxdn_set_bit(out, k++, (d + d3 + d4) & 1u);
        nxdn_set_bit(out, k++, (d + d1 + d2 + d4) & 1u);
        d4 = d3; d3 = d2; d2 = d1; d1 = d;
    }
}

/* 16-state Viterbi. A state is the last four input bits, newest in bit 3.
 * Branch metric: Hamming distance on the two coded bits, erasures free. */
enum { VSTATES = 16, VMAX = 200 };
unsigned nxdn_viterbi(const uint8_t *sym, unsigned n, uint8_t *out)
{
    static uint16_t dec[VMAX];       /* survivor bits: bit s set = the predecessor's oldest bit was 1 */
    unsigned metric[VSTATES], next[VSTATES];
    if (n > VMAX) return ~0u;
    for (unsigned s = 0; s < VSTATES; s++) metric[s] = s ? 1000u : 0u;
    for (unsigned t = 0; t < n; t++) {
        uint8_t c0 = sym[2 * t], c1 = sym[2 * t + 1];
        uint16_t bits = 0;
        for (unsigned s = 0; s < VSTATES; s++) {
            /* state s = (d, d1, d2, d3) with d the newest input; predecessors p = (d1, d2, d3, x) */
            unsigned d = (s >> 3) & 1u, d1 = (s >> 2) & 1u, d2 = (s >> 1) & 1u, d3 = s & 1u;
            unsigned best = ~0u, choice = 0;
            for (unsigned x = 0; x < 2; x++) {
                unsigned p = (s << 1 & 0xEu) | x;    /* p's bits: d1 d2 d3 x */
                unsigned g1 = (d + d3 + x) & 1u, g2 = (d + d1 + d2 + x) & 1u;
                unsigned m = metric[p] + (c0 < 2 && c0 != g1) + (c1 < 2 && c1 != g2);
                if (m < best) { best = m; choice = x; }
            }
            next[s] = best;
            if (choice) bits |= (uint16_t)(1u << s);
        }
        dec[t] = bits;
        memcpy(metric, next, sizeof metric);
    }
    /* The encoder flushed with four zeros: end in state 0 */
    unsigned s = 0, cost = metric[0];
    for (unsigned t = n; t-- > 0;) {
        nxdn_set_bit(out, t, (s >> 3) & 1u);
        unsigned x = (dec[t] >> s) & 1u;
        s = (s << 1 & 0xEu) | x;
    }
    return cost;
}

/* --- SACCH: 26 + CRC6 + 4 tail = 36 -> 72 coded, 12 punctured -> 60,
 * interleaved 12 x 5 --- */
static const uint8_t sacch_interleave[60] = {
    0, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55,
    1, 6, 11, 16, 21, 26, 31, 36, 41, 46, 51, 56,
    2, 7, 12, 17, 22, 27, 32, 37, 42, 47, 52, 57,
    3, 8, 13, 18, 23, 28, 33, 38, 43, 48, 53, 58,
    4, 9, 14, 19, 24, 29, 34, 39, 44, 49, 54, 59 };
static bool sacch_punctured(unsigned i) { return i % 6 == 5; }   /* 5, 11, ..., 71 */

void nxdn_sacch_encode(const nxdn_sacch *s, uint8_t f[NXDN_FRAME_BYTES])
{
    uint8_t in[5] = { 0 }, coded[9], punct[8] = { 0 };
    in[0] = (uint8_t)((s->structure & 3u) << 6 | (s->ran & 0x3Fu));
    for (unsigned i = 0; i < 18; i++) nxdn_set_bit(in, 8 + i, (s->data >> (17 - i)) & 1u);
    uint8_t c = nxdn_crc6(in, 26);
    for (unsigned i = 0; i < 6; i++) nxdn_set_bit(in, 26 + i, (c >> (5 - i)) & 1u);
    nxdn_conv_encode(in, 36, coded);
    unsigned n = 0;
    for (unsigned i = 0; i < 72; i++)
        if (!sacch_punctured(i)) nxdn_set_bit(punct, n++, nxdn_bit(coded, i));
    for (unsigned i = 0; i < 60; i++)
        nxdn_set_bit(f, NXDN_SACCH_OFFSET + sacch_interleave[i], nxdn_bit(punct, i));
}

bool nxdn_sacch_decode(const uint8_t f[NXDN_FRAME_BYTES], nxdn_sacch *s, unsigned *corrected)
{
    uint8_t sym[80], out[5] = { 0 };
    unsigned n = 0;
    for (unsigned i = 0; i < 72; i++)
        sym[i] = sacch_punctured(i) ? 2 : (uint8_t)nxdn_bit(f, NXDN_SACCH_OFFSET + sacch_interleave[n++]);
    for (unsigned i = 72; i < 80; i++) sym[i] = 0;   /* not sent; the tail is already in the 36 bits */
    unsigned cost = nxdn_viterbi(sym, 36, out);
    if (corrected) *corrected = cost;
    unsigned got = 0;
    for (unsigned i = 0; i < 6; i++) got = got << 1 | nxdn_bit(out, 26 + i);
    s->ran = out[0] & 0x3Fu;
    s->structure = out[0] >> 6;
    s->data = 0;
    for (unsigned i = 0; i < 18; i++) s->data = s->data << 1 | nxdn_bit(out, 8 + i);
    return got == nxdn_crc6(out, 26);
}

/* --- FACCH1: 80 + CRC12 + 4 tail = 96 -> 192, 48 punctured -> 144,
 * interleaved 16 x 9 --- */
static unsigned facch_interleave(unsigned i) { return (i % 16) * 9 + i / 16; }
static bool facch_punctured(unsigned i) { return i % 4 == 1; }   /* 1, 5, ..., 189 */

void nxdn_facch1_encode(const uint8_t data[NXDN_LAYER3_BYTES], uint8_t f[NXDN_FRAME_BYTES], unsigned which)
{
    uint8_t in[12] = { 0 }, coded[24], punct[18] = { 0 };
    memcpy(in, data, NXDN_LAYER3_BYTES);
    uint16_t c = nxdn_crc12(in, 80);
    for (unsigned i = 0; i < 12; i++) nxdn_set_bit(in, 80 + i, (c >> (11 - i)) & 1u);
    nxdn_conv_encode(in, 96, coded);
    unsigned n = 0;
    for (unsigned i = 0; i < 192; i++)
        if (!facch_punctured(i)) nxdn_set_bit(punct, n++, nxdn_bit(coded, i));
    unsigned base = NXDN_PAYLOAD_OFFSET + which * NXDN_FACCH1_BITS;
    for (unsigned i = 0; i < 144; i++) nxdn_set_bit(f, base + facch_interleave(i), nxdn_bit(punct, i));
}

bool nxdn_facch1_decode(const uint8_t f[NXDN_FRAME_BYTES], unsigned which, uint8_t data[NXDN_LAYER3_BYTES], unsigned *corrected)
{
    uint8_t sym[192], out[12] = { 0 };
    unsigned base = NXDN_PAYLOAD_OFFSET + which * NXDN_FACCH1_BITS, n = 0;
    for (unsigned i = 0; i < 192; i++)
        sym[i] = facch_punctured(i) ? 2 : (uint8_t)nxdn_bit(f, base + facch_interleave(n++));
    unsigned cost = nxdn_viterbi(sym, 96, out);
    if (corrected) *corrected = cost;
    unsigned got = 0;
    for (unsigned i = 0; i < 12; i++) got = got << 1 | nxdn_bit(out, 80 + i);
    memcpy(data, out, NXDN_LAYER3_BYTES);
    return got == nxdn_crc12(out, 80);
}

/* --- Layer 3 (TS 1-C VCALL / TX_REL) --- */
void nxdn_layer3_encode(const nxdn_call_msg *m, uint8_t d[NXDN_LAYER3_BYTES])
{
    memset(d, 0, NXDN_LAYER3_BYTES);
    d[0] = m->type & 0x3Fu;
    d[1] = m->cc_option;
    d[2] = (uint8_t)((m->call_type & 7u) << 5 | (m->voice_option & 0x1Fu));
    d[3] = (uint8_t)(m->source >> 8);
    d[4] = (uint8_t)m->source;
    d[5] = (uint8_t)(m->destination >> 8);
    d[6] = (uint8_t)m->destination;
    d[7] = (uint8_t)((m->cipher_type & 3u) << 6 | (m->key_id & 0x3Fu));
}

bool nxdn_layer3_decode(const uint8_t d[NXDN_LAYER3_BYTES], nxdn_call_msg *m)
{
    m->type = d[0] & 0x3Fu;
    m->cc_option = d[1];
    m->call_type = d[2] >> 5;
    m->voice_option = d[2] & 0x1Fu;
    m->source = (uint16_t)(d[3] << 8 | d[4]);
    m->destination = (uint16_t)(d[5] << 8 | d[6]);
    m->cipher_type = d[7] >> 6;
    m->key_id = d[7] & 0x3Fu;
    return m->type == NXDN_MSG_VCALL || m->type == NXDN_MSG_TX_REL;
}

/* --- Voice slots: 72 bits each, in air order (the AMBE+2 3600x2450 frame) --- */
void nxdn_voice_put(uint8_t f[NXDN_FRAME_BYTES], unsigned k, const uint8_t slot[9])
{
    unsigned base = NXDN_PAYLOAD_OFFSET + k * NXDN_VOICE_BITS;
    for (unsigned i = 0; i < 72; i++) nxdn_set_bit(f, base + i, nxdn_bit(slot, i));
}

void nxdn_voice_get(const uint8_t f[NXDN_FRAME_BYTES], unsigned k, uint8_t slot[9])
{
    unsigned base = NXDN_PAYLOAD_OFFSET + k * NXDN_VOICE_BITS;
    memset(slot, 0, 9);
    for (unsigned i = 0; i < 72; i++) nxdn_set_bit(slot, i, nxdn_bit(f, base + i));
}
