/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host-only channel model for the NXDN modem simulation (docs/nxdn-modem.md).
 *
 * The transmitter's 8 kHz modulation samples (or a 48 kHz deviation signal)
 * frequency-modulate a complex carrier at 48 kHz with the deviation scaled by
 * dev_scale and a carrier offset; complex white noise sets the carrier-to-noise
 * ratio in a 12.5 kHz bandwidth; a 12.5 kHz channel filter, an ideal
 * discriminator and a 3.4 kHz audio low-pass follow, then decimation to 8 kHz
 * and the chip's audio chain: a first-order high-pass, gain and clipping to
 * int16. Optional first-order high-pass on the transmit side too, and noise
 * alone for a while before the carrier appears (the receiver idling). */
#include "nxdn_channel.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum { UP = 6, FS48 = 48000 };

static uint64_t state;
static double uniform(void)
{
    state ^= state << 13; state ^= state >> 7; state ^= state << 17;
    return ((state >> 11) + 0.5) / 9007199254740992.0;
}
static double gauss(void) { return sqrt(-2 * log(uniform())) * cos(2 * M_PI * uniform()); }

/* windowed-sinc low-pass, cutoff in Hz at 48 kHz, odd length */
static void lowpass(double *h, int n, double cutoff)
{
    double fc = cutoff / FS48, sum = 0;
    for (int i = 0; i < n; i++) {
        double m = i - (n - 1) / 2.0;
        double s = m == 0 ? 2 * fc : sin(2 * M_PI * fc * m) / (M_PI * m);
        double w = 0.42 - 0.5 * cos(2 * M_PI * i / (n - 1)) + 0.08 * cos(4 * M_PI * i / (n - 1));
        h[i] = s * w;
        sum += h[i];
    }
    for (int i = 0; i < n; i++) h[i] /= sum;
}

static void fir(const double *x, size_t n, const double *h, int taps, double *y)
{
    int half = taps / 2;
    for (size_t i = 0; i < n; i++) {
        double acc = 0;
        for (int k = 0; k < taps; k++) {
            long j = (long)i + half - k;
            if (j >= 0 && (size_t)j < n) acc += h[k] * x[j];
        }
        y[i] = acc;
    }
}

static void highpass(double *x, size_t n, double fc, double fs)
{
    if (fc <= 0) return;
    double a = 1.0 / (1.0 + 2 * M_PI * fc / fs), prev_x = 0, prev_y = 0;
    for (size_t i = 0; i < n; i++) {
        double y = a * (prev_y + x[i] - prev_x);
        prev_x = x[i];
        prev_y = y;
        x[i] = y;
    }
}

size_t nxdn_channel_run48(const nxdn_channel_cfg *c, const double *dev48, size_t n48, int16_t *out8)
{
    state = c->seed ? c->seed : 0x9E3779B97F4A7C15ull;
    /* lead: noise alone before the carrier appears (whole 8 kHz samples) */
    size_t lead = (size_t)(c->lead_s * 8000) * UP, sig = n48;
    n48 += lead;
    double *re = malloc(n48 * sizeof *re), *im = malloc(n48 * sizeof *im);
    double *fre = malloc(n48 * sizeof *fre), *fim = malloc(n48 * sizeof *fim), *disc = malloc(n48 * sizeof *disc);
    double *aud = malloc(n48 * sizeof *aud);
    double h_if[95], h_aud[191];
    lowpass(h_if, 95, 6250);
    lowpass(h_aud, 191, 3400);
    double phase = 0;
    double sigma = sqrt((double)FS48 / 12500.0 * pow(10, -c->cnr_db / 10.0) / 2.0);
    (void)sig;
    for (size_t i = 0; i < n48; i++) {
        double on = i >= lead;
        phase += 2 * M_PI * ((on ? dev48[i - lead] : 0) * c->dev_scale + c->offset_hz) / FS48;
        /* no carrier and no noise would leave the discriminator undefined: noise at -10 dB C/N floor */
        double s = c->cnr_db < 200 ? sigma : on ? 0 : 0.3;
        re[i] = on * cos(phase) + (s ? s * gauss() : 0);
        im[i] = on * sin(phase) + (s ? s * gauss() : 0);
    }
    fir(re, n48, h_if, 95, fre);
    fir(im, n48, h_if, 95, fim);
    disc[0] = 0;
    for (size_t i = 1; i < n48; i++) {
        double a = fre[i] * fre[i - 1] + fim[i] * fim[i - 1];
        double b = fim[i] * fre[i - 1] - fre[i] * fim[i - 1];
        disc[i] = atan2(b, a) * FS48 / (2 * M_PI);
    }
    fir(disc, n48, h_aud, 191, aud);
    size_t n8 = n48 / UP;
    double *a8 = malloc(n8 * sizeof *a8);
    for (size_t i = 0; i < n8; i++) a8[i] = aud[i * UP];
    highpass(a8, n8, c->rx_hpf_hz, 8000);
    double clip = c->clip > 0 ? c->clip : 32767;
    for (size_t i = 0; i < n8; i++) {
        double v = a8[i] * c->rx_gain;
        if (v > clip) v = clip;
        if (v < -clip) v = -clip;
        out8[i] = (int16_t)lrint(v);
    }
    free(re); free(im); free(fre); free(fim); free(disc); free(aud); free(a8);
    return n8;
}

size_t nxdn_channel_tx48(const int16_t *tx8, size_t n8, double tx_gain, double tx_hpf_hz, double *dev48)
{
    /* The chip's FM modulator interpolates its 8 kHz samples: an ideal 3.6 kHz interpolator */
    size_t n48 = n8 * UP;
    double *x = calloc(n48, sizeof *x), *t8 = malloc(n8 * sizeof *t8);
    for (size_t i = 0; i < n8; i++) t8[i] = tx8[i] / tx_gain;
    highpass(t8, n8, tx_hpf_hz, 8000);
    for (size_t i = 0; i < n8; i++) x[i * UP] = t8[i] * UP;
    double h[191];
    lowpass(h, 191, 3600);
    fir(x, n48, h, 191, dev48);
    free(x); free(t8);
    return n48;
}

size_t nxdn_channel_run(const nxdn_channel_cfg *c, const int16_t *tx8, size_t n8, double tx_gain, int16_t *out8)
{
    double *y = malloc(n8 * UP * sizeof *y);
    size_t n48 = nxdn_channel_tx48(tx8, n8, tx_gain, c->tx_hpf_hz, y);
    size_t r = nxdn_channel_run48(c, y, n48, out8);
    free(y);
    return r;
}

/* in-place radix-2 FFT */
static void fft(double *re, double *im, unsigned n)
{
    for (unsigned i = 1, j = 0; i < n; i++) {
        unsigned bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            double t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (unsigned len = 2; len <= n; len <<= 1) {
        double ang = -2 * M_PI / len;
        for (unsigned i = 0; i < n; i += len)
            for (unsigned k = 0; k < len / 2; k++) {
                double wr = cos(ang * k), wi = sin(ang * k);
                double ur = re[i + k], ui = im[i + k];
                double vr = re[i + k + len / 2] * wr - im[i + k + len / 2] * wi;
                double vi = re[i + k + len / 2] * wi + im[i + k + len / 2] * wr;
                re[i + k] = ur + vr; im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
            }
    }
}

void nxdn_channel_spectrum(const double *dev48, size_t n48, double half_channel_hz, double *obw99_hz,
                           double *outside, double *adjacent_db)
{
    enum { N = 8192 };
    double *psd = calloc(N, sizeof *psd), *re = malloc(N * sizeof *re), *im = malloc(N * sizeof *im);
    double phase = 0;
    unsigned segs = 0;
    /* Welch: Blackman-Harris windows, half overlap, over the FM carrier at 48 kHz */
    double *ph = malloc(n48 * sizeof *ph);
    for (size_t i = 0; i < n48; i++) {
        phase += 2 * M_PI * dev48[i] / FS48;
        ph[i] = phase;
    }
    for (size_t s = 0; s + N <= n48; s += N / 2, segs++) {
        for (unsigned i = 0; i < N; i++) {
            double x = 2 * M_PI * i / (N - 1);
            double w = 0.35875 - 0.48829 * cos(x) + 0.14128 * cos(2 * x) - 0.01168 * cos(3 * x);
            re[i] = cos(ph[s + i]) * w;
            im[i] = sin(ph[s + i]) * w;
        }
        fft(re, im, N);
        for (unsigned i = 0; i < N; i++) psd[i] += re[i] * re[i] + im[i] * im[i];
    }
    double total = 0, in = 0, adj = 0, df = (double)FS48 / N;
    for (unsigned i = 0; i < N; i++) {
        double f = (i < N / 2 ? (double)i : (double)i - N) * df;
        total += psd[i];
        if (fabs(f) <= half_channel_hz) in += psd[i];
        /* the upper and lower adjacent channels: same width, one channel spacing away */
        if (fabs(fabs(f) - 2 * half_channel_hz) <= half_channel_hz) adj += psd[i];
    }
    /* 99% occupied bandwidth: grow a centred band until it holds 99% */
    double acc = psd[0];
    unsigned k = 0;
    while (acc < 0.99 * total && k < N / 2 - 1) {
        k++;
        acc += psd[k] + psd[N - k];
    }
    *obw99_hz = (2 * k + 1) * df;
    *outside = 1 - in / total;
    *adjacent_db = 10 * log10(adj / 2 / in);   /* each adjacent channel relative to the wanted one */
    free(psd); free(re); free(im); free(ph);
}
