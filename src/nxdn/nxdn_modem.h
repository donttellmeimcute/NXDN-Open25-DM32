/* SPDX-License-Identifier: GPL-3.0-or-later */
/* NXDN48 4FSK modem in software at 8 kHz: 2400 symbols/s, +-1050 / +-350 Hz.
 *
 * The radio's FM paths carry 8 kHz samples: the receiver's discriminator audio
 * (FM RX RAM) and generated modulation (the route APRS uses). 8000/2400 = 10/3
 * samples per symbol, so both directions work with fractional timing:
 *
 *   TX  dibits -> levels -> root-raised-cosine (0.2) x sin(x)/x pulse, 10-phase
 *       polyphase -> int16 samples, `gain` LSB per 1/256 Hz [-> inverse of the
 *       transmit audio path's high-pass]
 *   RX  int16 samples [-> inverse of the receive audio path's high-pass] ->
 *       root-raised cosine x x/sin(x) evaluated at interpolated
 *       instants -> Gardner timing loop -> DC (frequency offset) and level
 *       tracking -> hard dibits
 *
 * Integer arithmetic only (the CK803S has no FPU). Tables from
 * tools/gen_nxdn_tables.py. No allocation, no RTOS, no hardware. */
#ifndef NXDN_MODEM_H
#define NXDN_MODEM_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { NXDN_FS = 8000, NXDN_RS = 2400, NXDN_OUTER_HZ = 1050, NXDN_INNER_HZ = 350 };

/* Undoing a first-order high-pass in the radio's audio path (its corner is
 * measured on the radio): x[n] = leak * x[n-1] + y[n] / a - y[n-1], with
 * a = 1 / (1 + 2 pi fc / fs). The leak (1 - 2^-leak_shift per sample, a
 * corner of 8000 / (2 pi 2^leak_shift) Hz) keeps an offset from integrating
 * and bounds the gain below it to fc / leak corner. */
typedef struct {
    int32_t inv_a_q15;     /* 0 = off */
    int32_t x_q4, y_prev;
    uint8_t leak_shift;
} nxdn_hpf_inverse;
/* Receive: leak_shift NXDN_RX_LEAK. Transmit: NXDN_TX_LEAK, since what the
 * transmitter adds below the leak corner moves the carrier (the gain there
 * must stay small enough not to clip or leave the channel). */
enum { NXDN_RX_LEAK = 10, NXDN_TX_LEAK = 7 };
void nxdn_hpf_inverse_init(nxdn_hpf_inverse *h, uint16_t corner_hz, uint8_t leak_shift);
int16_t nxdn_hpf_inverse_step(nxdn_hpf_inverse *h, int16_t y);

typedef struct {
    int8_t sym[16];        /* the last symbols, ring indexed by count */
    uint32_t symbols;      /* symbols pushed */
    uint32_t samples;      /* samples produced */
    int32_t gain_q8;       /* output LSB per Hz, Q8 */
    nxdn_hpf_inverse pre;  /* pre-compensation of the transmit audio path's high-pass */
} nxdn_mod;

/* gain_q8: output units per Hz of deviation x 256 (256 = 1 LSB per Hz) */
void nxdn_mod_init(nxdn_mod *m, int32_t gain_q8);
/* One dibit in; writes the samples now complete (0, 3 or 4) to out. The
 * pulse looks four symbols ahead: samples lag the symbols by four symbols. */
size_t nxdn_mod_dibit(nxdn_mod *m, uint8_t dibit, int16_t out[4]);
/* Four zero symbols to push the last samples out */
size_t nxdn_mod_flush(nxdn_mod *m, int16_t out[16]);

typedef struct {
    int16_t ring[64];      /* input samples, indexed by count & 63 */
    uint32_t count;        /* samples in */
    int64_t t_on;          /* next symbol instant, samples in Q16 */
    int32_t prev_on;       /* matched filter output at the last instant */
    int32_t dc, outer, amp;  /* level tracking: offset, outer level (3 units), threshold (2 units) */
    uint32_t symbols;
    int32_t last_error;    /* Gardner error of the last symbol (diagnostic) */
    nxdn_hpf_inverse post; /* compensation of the receive audio path's high-pass */
} nxdn_demod;

void nxdn_demod_init(nxdn_demod *d);
/* A new transmission may start (the squelch opened, or the frame layer lost
 * sync): fast offset, level and timing acquisition again */
void nxdn_demod_reacquire(nxdn_demod *d);
/* Samples in; hard dibits out (at most cap). Returns the dibits written. */
size_t nxdn_demod_samples(nxdn_demod *d, const int16_t *in, size_t n, uint8_t *dibits, size_t cap);
/* The soft value of the last symbol relative to the tracked levels: +-3 / +-1 = +-3*256 / +-1*256 */
int32_t nxdn_demod_soft(const nxdn_demod *d);

#ifdef __cplusplus
}
#endif
#endif
