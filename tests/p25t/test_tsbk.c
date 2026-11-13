/* SPDX-License-Identifier: GPL-3.0-or-later */
/* TSBK coding: CRC, trellis round trip and error correction, TSDU frames found
 * in a dibit stream with status symbols, polarity, NAC filter. */
#include "tsbk.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t rnd = 0x7ED5u;
static uint32_t next(void) { rnd ^= rnd << 13; rnd ^= rnd >> 17; rnd ^= rnd << 5; return rnd; }

static uint8_t got[16][12];
static unsigned n_got, n_status, status_idle, n_frames;
static uint8_t last_duid;
static void on_tsbk(void *c, uint16_t nac, const uint8_t t[12]) { (void)c; (void)nac; memcpy(got[n_got++ % 16], t, 12); }
static void on_frame(void *c, uint16_t nac, uint8_t duid) { (void)c; (void)nac; last_duid = duid; n_frames++; }
static void on_status(void *c, uint8_t s, uint32_t at) { (void)c; (void)at; n_status++; status_idle += s == P25T_STATUS_IDLE; }

static void feed(p25t_rx *rx, const uint8_t *frame, unsigned bits, bool invert)
{
    for (unsigned i = 0; i < bits; i += 2) {
        uint8_t d = (uint8_t)(((frame[i >> 3] >> (7 - (i & 7))) & 1u) << 1 | ((frame[(i + 1) >> 3] >> (7 - ((i + 1) & 7))) & 1u));
        p25t_rx_dibit(rx, invert ? d ^ 2u : d);
    }
}

int main(void)
{
    /* CRC: a sealed block checks, any one flipped bit fails; residual over all 96 bits is 0xffff (OP25's check) */
    uint8_t t[12];
    for (int k = 0; k < 1000; k++) {
        for (int i = 0; i < 10; i++) t[i] = (uint8_t)next();
        p25t_tsbk_seal(t);
        assert(p25t_tsbk_crc_ok(t));
        int b = (int)(next() % 96);
        t[b / 8] ^= (uint8_t)(0x80u >> (b % 8));
        assert(!p25t_tsbk_crc_ok(t));
    }

    /* Trellis: round trip; every block with one or two bit errors corrects, nearly every one with three (spread out) */
    unsigned fixed3 = 0, tried3 = 0;
    for (int k = 0; k < 2000; k++) {
        uint8_t a[12], blk[25], back[12];
        for (int i = 0; i < 10; i++) a[i] = (uint8_t)next();
        p25t_tsbk_seal(a);
        p25t_trellis_encode(a, blk);
        assert(p25t_trellis_decode(blk, back) == 0 && !memcmp(a, back, 12));
        unsigned errs = k % 4;
        for (unsigned e = 0; e < errs; e++) {
            unsigned b = (e * 65 + next() % 60) % 196;
            blk[b / 8] ^= (uint8_t)(0x80u >> (b % 8));
        }
        int m = p25t_trellis_decode(blk, back);
        bool ok = m >= 0 && !memcmp(a, back, 12);
        if (errs < 3) assert(ok);
        else {
            tried3++;
            fixed3 += ok;
        }
    }
    fprintf(stderr, "trellis: %u of %u blocks with 3 bit errors recovered (all with 0-2)\n", fixed3, tried3);
    assert(fixed3 * 100 >= tried3 * 99);

    /* TSDU frames in a stream: 1, 2 and 3 blocks, any polarity, status symbols reported */
    p25t_rx rx;
    p25t_rx_init(&rx, 0x293, on_tsbk, on_frame, on_status, NULL);
    uint8_t blocks[3][12], frame[P25T_TSDU_MAX_BYTES];
    for (unsigned n = 1; n <= 3; n++) {
        for (unsigned b = 0; b < n; b++) {
            for (int i = 0; i < 10; i++) blocks[b][i] = (uint8_t)next();
            blocks[b][0] &= 0x3f;
        }
        unsigned bits = p25t_tsdu_encode(0x293, blocks, n, P25T_STATUS_IDLE, frame);
        assert(bits == 48 + 64 + 196 * n + 2 * ((48 + 64 + 196 * n) / 70));
        n_got = 0;
        feed(&rx, frame, bits, n == 2);
        assert(n_got == n);
        for (unsigned b = 0; b < n; b++) {
            assert(!memcmp(got[b] + 1, blocks[b] + 1, 9) && (got[b][0] & 0x3f) == blocks[b][0]);
            assert(((got[b][0] & 0x80) != 0) == (b + 1 == n));
        }
    }
    assert(n_status > 0 && status_idle == n_status);
    /* another NAC is not ours */
    n_got = 0;
    p25t_tsdu_encode(0x5a5, blocks, 1, P25T_STATUS_IDLE, frame);
    feed(&rx, frame, 316, false);
    assert(n_got == 0 && rx.bad_nid == 1);
    /* errors in a block within the code's reach still deliver it */
    unsigned bits = p25t_tsdu_encode(0x293, blocks, 1, P25T_STATUS_BUSY, frame);
    frame[20] ^= 0x10;
    frame[30] ^= 0x01;
    n_got = 0;
    feed(&rx, frame, bits, false);
    assert(n_got == 1);
    printf("PASS P25 TSBK: CRC, trellis Viterbi (0-3 errors), TSDU of 1-3 blocks either polarity, status symbols, NAC\n");
    return 0;
}
