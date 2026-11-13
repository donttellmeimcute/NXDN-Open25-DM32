/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "nxdn_modem.h"
#include <string.h>
#include "nxdn_modem_tables.inc"

enum { SPAN = (NXDN_TX_TAPS - 1) / 2 };

void nxdn_hpf_inverse_init(nxdn_hpf_inverse *h, uint16_t corner_hz, uint8_t leak_shift)
{
    memset(h, 0, sizeof *h);
    h->leak_shift = leak_shift;
    if (!corner_hz) return;
    /* 1/a = 1 + 2 pi fc / fs, Q15: 2 pi / 8000 = 0.000785398 = 25.736 / 32768 */
    h->inv_a_q15 = 32768 + (int32_t)((uint32_t)corner_hz * 25736u / 1000u);
}

int16_t nxdn_hpf_inverse_step(nxdn_hpf_inverse *h, int16_t y)
{
    if (!h->inv_a_q15) return y;
    int32_t gain = (int32_t)(((int64_t)y * h->inv_a_q15) >> 15);
    /* state in Q4 */
    h->x_q4 = h->x_q4 - (h->x_q4 >> h->leak_shift) + ((gain - h->y_prev) * 16);
    h->y_prev = y;
    int32_t x = h->x_q4 >> 4;
    return (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
}

void nxdn_deemph_inverse_init(nxdn_deemph_inverse *e, uint16_t tau_us)
{
    memset(e, 0, sizeof *e);
    if (!tau_us) return;
    /* 2 tau fs = 16 tau_us / 1000: a = (16 tau - 1000) / (16 tau + 1000), 1 / (1 - a) = (16 tau + 1000) / 2000 */
    int32_t k = 16 * (int32_t)tau_us;
    if (k <= 1000) return;          /* a corner above 2.5 kHz: nothing to undo in the symbol band */
    e->a_q15 = (int32_t)(((int64_t)(k - 1000) << 15) / (k + 1000));
    e->inv_q12 = (int32_t)(((int64_t)(k + 1000) << 12) / 2000);
}

int16_t nxdn_deemph_inverse_step(nxdn_deemph_inverse *e, int16_t y)
{
    if (!e->a_q15) return y;
    int32_t d = (int32_t)y - (int32_t)(((int64_t)e->prev * e->a_q15) >> 15);
    e->prev = y;
    int32_t x = (int32_t)(((int64_t)d * e->inv_q12) >> 12);
    return (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
}

static const int8_t level[4] = { 1, 3, -1, -3 };   /* dibit 00 +1, 01 +3, 10 -1, 11 -3 */

void nxdn_mod_init(nxdn_mod *m, int32_t gain_q8)
{
    memset(m, 0, sizeof *m);
    m->gain_q8 = gain_q8;
}

static size_t emit(nxdn_mod *m, int16_t *out)
{
    /* Symbol k = symbols - 1 just arrived: sample n is complete when floor(0.3 n) + SPAN <= k */
    size_t w = 0;
    int32_t k = (int32_t)m->symbols - 1;
    for (;;) {
        uint32_t n = m->samples;
        int32_t base = (int32_t)(n * 3u / 10u);
        if (base + SPAN > k) break;
        const int16_t *tab = nxdn_tx_pulse[n % 10u];
        int32_t acc = 0;
        for (int j = -SPAN; j <= SPAN; j++) {
            int32_t idx = base - j;
            if (idx < 0) continue;
            acc += (int32_t)m->sym[idx & 15] * tab[j + SPAN];
        }
        /* acc: levels (units of 350 Hz) x Q14; out = acc * 350 Hz * gain / 2^14 / 2^8 */
        int64_t v = (int64_t)acc * NXDN_INNER_HZ * m->gain_q8 >> 22;
        out[w++] = nxdn_hpf_inverse_step(&m->pre, (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v));
        m->samples++;
    }
    return w;
}

size_t nxdn_mod_dibit(nxdn_mod *m, uint8_t dibit, int16_t out[4])
{
    m->sym[m->symbols & 15u] = level[dibit & 3u];
    m->symbols++;
    return emit(m, out);
}

size_t nxdn_mod_flush(nxdn_mod *m, int16_t out[16])
{
    size_t w = 0;
    for (unsigned i = 0; i < SPAN; i++) {
        m->sym[m->symbols & 15u] = 0;
        m->symbols++;
        w += emit(m, out + w);
    }
    return w;
}

/* --- Receive --- */
#define SPS_Q16 ((int64_t)NXDN_FS * 65536 / NXDN_RS)   /* 10/3 samples */
/* Acquisition: the first symbols after nxdn_demod_init / nxdn_demod_reacquire (a
 * transmission's preamble alternates +3 / -3) set the offset from the mean of
 * symbol pairs and the timing with a fast loop; after them the loops slow down,
 * since on data Gardner's self-noise jitters a fast loop (docs/nxdn-modem.md). */
enum { AMP_MIN = 64, ACQ_SYMBOLS = 32, TIMING_ACQ_SHIFT = 4, TIMING_TRACK_SHIFT = 8 };

void nxdn_demod_init(nxdn_demod *d)
{
    memset(d, 0, sizeof *d);
    d->t_on = (int64_t)NXDN_RX_HALF << 16;
    d->amp = 0;
}

void nxdn_demod_reacquire(nxdn_demod *d)
{
    d->symbols = 0;
}

/* Matched filter at instant t (samples, Q16) */
static int32_t mf(const nxdn_demod *d, int64_t t)
{
    int32_t i = (int32_t)(t >> 16);
    unsigned ph = (unsigned)((t & 0xFFFF) * NXDN_RX_PHASES >> 16);
    const int16_t *h = nxdn_rx_filter[ph];
    int32_t acc = 0;
    for (int m = -NXDN_RX_HALF; m <= NXDN_RX_HALF; m++) acc += (int32_t)d->ring[(uint32_t)(i + m) & 63u] * h[m + NXDN_RX_HALF];
    return acc >> 14;
}

static uint8_t slice(const nxdn_demod *d, int32_t y)
{
    int32_t v = y - d->dc, a = d->amp;
    if (v > a) return 1;          /* +3 */
    if (v > 0) return 0;          /* +1 */
    if (v > -a) return 2;         /* -1 */
    return 3;                     /* -3 */
}

int32_t nxdn_demod_soft(const nxdn_demod *d)
{
    int32_t a = d->amp > AMP_MIN ? d->amp : AMP_MIN;
    return (int32_t)(((int64_t)(d->prev_on - d->dc) << 9) / a);   /* the outer level (2 x amp / ... ) lands near 3 x 256 */
}

size_t nxdn_demod_samples(nxdn_demod *d, const int16_t *in, size_t n, uint8_t *dibits, size_t cap)
{
    size_t w = 0;
    for (size_t s = 0; s < n; s++) {
        d->ring[d->count & 63u] = nxdn_hpf_inverse_step(&d->post, nxdn_deemph_inverse_step(&d->deemph, in[s]));
        d->count++;
        /* every instant whose filter span is now in the ring */
        while (w < cap && (d->t_on >> 16) + NXDN_RX_HALF <= (int64_t)d->count - 1) {
            int32_t on = mf(d, d->t_on);
            int32_t mid = mf(d, d->t_on - SPS_Q16 / 2);
            /* Levels, decision directed: after the symbol is sliced, the offset moves by the
             * difference from that symbol's ideal level and the outer level (+-3 units) by the
             * outer symbols' magnitude; the threshold sits at 2/3 of it. Fast while acquiring,
             * then slow. The first symbols seed the outer level from their magnitude. */
            unsigned shift = d->symbols < ACQ_SYMBOLS ? 3 : d->symbols < 400 ? 5 : 6;
            int32_t dev = on - d->dc, mag = dev < 0 ? -dev : dev;
            if (d->symbols < 4) {
                d->dc += (on - d->dc) >> 1;
                d->outer += (mag - d->outer) >> 1;
            }
            d->amp = d->outer * 2 / 3;
            uint8_t s = slice(d, on);
            dibits[w++] = s;
            static const int8_t lv[4] = { 1, 3, -1, -3 };
            /* While acquiring, the offset is the mean of the last two symbols (exact on the
             * alternating preamble), which a wrong early decision can't run away with */
            if (d->symbols < ACQ_SYMBOLS) d->dc += ((on + d->prev_on) / 2 - d->dc) >> shift;
            else d->dc += (on - (d->dc + lv[s] * d->outer / 3)) >> shift;
            if (s == 1 || s == 3) d->outer += (mag - d->outer) >> shift;
            /* Gardner: (y_k - y_k-1) * y_k-1/2, normalized by the level squared */
            int32_t a = d->amp > AMP_MIN ? d->amp : AMP_MIN;
            int64_t e = (int64_t)(on - d->prev_on) * (mid - d->dc);
            int64_t norm = e * 65536 / ((int64_t)a * a);
            if (norm > (4 << 16)) norm = 4 << 16;
            if (norm < -(4 << 16)) norm = -(4 << 16);
            d->last_error = (int32_t)norm;
            d->t_on += SPS_Q16 - (norm >> (d->symbols < ACQ_SYMBOLS ? TIMING_ACQ_SHIFT : TIMING_TRACK_SHIFT));
            d->prev_on = on;
            d->symbols++;
        }
    }
    return w;
}
