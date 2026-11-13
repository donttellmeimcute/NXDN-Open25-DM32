/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host-only FM channel model for the NXDN modem simulation. */
#ifndef NXDN_CHANNEL_H
#define NXDN_CHANNEL_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    double cnr_db;        /* carrier to noise in 12.5 kHz; >= 200 = no noise */
    double offset_hz;     /* carrier frequency error */
    double dev_scale;     /* transmitted deviation / nominal */
    double tx_hpf_hz;     /* first-order high-pass in the transmit audio path, 0 = none */
    double rx_hpf_hz;     /* first-order high-pass in the receive audio path, 0 = none */
    double rx_gain;       /* receive audio LSB per Hz */
    double clip;          /* receive audio clipping level in LSB, 0 = int16 limits */
    double lead_s;        /* noise alone before the carrier, seconds (output grows by lead_s x 8000) */
    uint64_t seed;
} nxdn_channel_cfg;

/* tx8: the modulator's samples (tx_gain LSB per Hz) -> out8 receive audio. Returns samples written (n8 + lead) */
size_t nxdn_channel_run(const nxdn_channel_cfg *c, const int16_t *tx8, size_t n8, double tx_gain, int16_t *out8);
/* The same from a 48 kHz deviation signal in Hz (another transmitter's waveform) */
size_t nxdn_channel_run48(const nxdn_channel_cfg *c, const double *dev48, size_t n48, int16_t *out8);
/* The transmitter's 8 kHz samples as the 48 kHz deviation (Hz) the chip modulates */
size_t nxdn_channel_tx48(const int16_t *tx8, size_t n8, double tx_gain, double tx_hpf_hz, double *dev48);
/* The FM carrier's spectrum: 99% occupied bandwidth, power fraction outside +-half_channel_hz, and each
 * adjacent channel's power (one channel spacing = 2 half_channel_hz away) relative to the wanted one, in dB */
void nxdn_channel_spectrum(const double *dev48, size_t n48, double half_channel_hz, double *obw99_hz,
                           double *outside, double *adjacent_db);
#endif
