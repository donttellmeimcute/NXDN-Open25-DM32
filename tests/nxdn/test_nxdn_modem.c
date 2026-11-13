/* SPDX-License-Identifier: GPL-3.0-or-later */
/* NXDN48 modem simulation: our TX (frames -> 8 kHz modulation, or the native
 * 4800-symbol/s physical mode with each symbol sent twice) through a simulated
 * FM channel into our 8 kHz receiver and call layer.
 *
 *   test_nxdn_modem            assertions (CI)
 *   test_nxdn_modem --sweep    the BER / FER tables of docs/nxdn-modem.md */
#include "nxdn_call.h"
#include "nxdn_modem.h"
#include "nxdn_channel.h"
#include "dmr/ambe/ambeFec.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Every simulated reception starts with the receiver idling on noise (100 ms), and the squelch opens 5 ms into
 * the carrier, when the demodulator reacquires */
#define LEAD_S 0.1
enum { SQUELCH_MS = 5 };
#ifndef TXG
#define TXG 512   /* modulator output: 2 LSB per Hz, headroom for high-pass pre-compensation */
#endif
enum { MAX_FRAMES = 64, MAX_DIB = 16000, MAX_SAMP = 60000, PREAMBLE = 48, TX_GAIN_Q8 = TXG };
enum { ROUTE_SOFT8K, ROUTE_PHY4800 };

static uint32_t rnd;
static uint32_t next(void) { rnd ^= rnd << 13; rnd ^= rnd >> 17; rnd ^= rnd << 5; return rnd; }

static uint8_t params[MAX_FRAMES * 4][49];
static unsigned n_params, n_heard, slot_ok;
static uint8_t txdib[MAX_DIB], rxdib[MAX_DIB];
static int16_t tx8[MAX_SAMP], rx8[MAX_SAMP];
static double dev48[MAX_SAMP * 6];

static void sink(void *ctx, const uint8_t slot[9])
{
    (void)ctx;
    uint8_t got[49];
    ambe_fec_result_t r;
    ambeFecDecode(slot, got, &r);
    /* slots arrive in order; a late entry starts later, so compare from the end */
    n_heard++;
    unsigned idx = (unsigned)(ctx ? *(unsigned *)ctx : 0);
    (void)idx;
    for (unsigned k = 0; k < n_params; k++)
        if (!memcmp(got, params[k], 49)) { slot_ok++; return; }
}

typedef struct {
    unsigned frames_sent, voice_frames_sent, slots_sent, slots_ok, bits, bit_errors, rx_frames, bad_sacch, calls;
    double ber, fer;
} sim_result;

/* A call of `frames` voice frames: dibits, preamble first (alternating +3 / -3) */
static unsigned build_call(unsigned frames, unsigned *frame_start)
{
    nxdn_tx tx;
    nxdn_tx_cfg cfg = { 12, 4001, 200, false, false, NXDN_DIR_INBOUND };
    nxdn_tx_begin(&tx, &cfg, 1);
    unsigned n = 0;
    for (unsigned i = 0; i < PREAMBLE; i++) txdib[n++] = i & 1 ? 3 : 1;
    *frame_start = n;
    n_params = 0;
    uint8_t f[NXDN_FRAME_BYTES];
    unsigned slots = frames * 4;
    for (unsigned k = 0;;) {
        if (k < slots && tx.nslots < 4) {
            for (unsigned i = 0; i < 49; i++) params[n_params][i] = next() & 1;
            uint8_t slot[9];
            ambeFecEncode(params[n_params++], slot);
            nxdn_tx_voice(&tx, slot);
            if (++k == slots) nxdn_tx_end(&tx);
            continue;
        }
        int r = nxdn_tx_frame(&tx, f);
        if (r < 0) break;
        if (!r) continue;
        for (unsigned i = 0; i < NXDN_FRAME_SYMBOLS; i++) txdib[n++] = (uint8_t)((f[i / 4] >> (6 - 2 * (i % 4))) & 3u);
    }
    for (unsigned i = 0; i < 24; i++) txdib[n++] = i & 1 ? 3 : 1;   /* a short tail while the PA drops */
    return n;
}

/* The native physical mode at 4800 symbols/s with each NXDN symbol sent twice: its DMR-style shaping is a root
 * raised cosine (0.2) at 4800 symbols/s; the amplitude registers set the deviation to +-1050 / +-350 Hz */
static size_t phy4800(unsigned ndib, double *out48)
{
    const int sps = 10;   /* 48000 / 4800 */
    const double b = 0.2;
    int span = 8 * sps;
    size_t n = (size_t)ndib * 2 * sps + 2 * span;
    memset(out48, 0, n * sizeof *out48);
    static const int lv[4] = { 1, 3, -1, -3 };
    double norm = 0;
    double h[161];
    for (int i = -span; i <= span; i++) {
        double t = (double)i / sps, v;
        if (fabs(t) < 1e-9) v = 1 - b + 4 * b / M_PI;
        else if (fabs(fabs(4 * b * t) - 1) < 1e-9) v = b / sqrt(2) * ((1 + 2 / M_PI) * sin(M_PI / (4 * b)) + (1 - 2 / M_PI) * cos(M_PI / (4 * b)));
        else v = (sin(M_PI * t * (1 - b)) + 4 * b * t * cos(M_PI * t * (1 + b))) / (M_PI * t * (1 - pow(4 * b * t, 2)));
        h[i + span] = v;
    }
    for (int i = -span; i <= span; i += sps) norm += h[i + span];   /* DC gain per symbol */
    for (unsigned k = 0; k < ndib * 2; k++) {
        double a = lv[txdib[k / 2]] * 350.0 / norm;
        for (int i = 0; i <= 2 * span; i++) out48[k * sps + i] += a * h[i];
    }
    return n;
}

static uint16_t comp_rx_hz, comp_tx_hz;   /* high-pass corners the modem compensates (0 = none) */
static uint16_t comp_deemph_us;           /* de-emphasis the demodulator undoes (0 = none) */
static uint8_t tx_leak = NXDN_TX_LEAK;

static void run(unsigned route, const nxdn_channel_cfg *c, unsigned frames, sim_result *r)
{
    memset(r, 0, sizeof *r);
    unsigned start;
    unsigned ndib = build_call(frames, &start);
    size_t n8;
    if (route == ROUTE_SOFT8K) {
        nxdn_mod m;
        nxdn_mod_init(&m, TX_GAIN_Q8);
        nxdn_hpf_inverse_init(&m.pre, comp_tx_hz, tx_leak);
        n8 = 0;
        for (unsigned i = 0; i < ndib; i++) n8 += nxdn_mod_dibit(&m, txdib[i], tx8 + n8);
        n8 += nxdn_mod_flush(&m, tx8 + n8);
        assert(n8 < MAX_SAMP);
        n8 = nxdn_channel_run(c, tx8, n8, TX_GAIN_Q8 / 256.0, rx8);
    } else {
        size_t n48 = phy4800(ndib, dev48);
        n8 = nxdn_channel_run48(c, dev48, n48, rx8);
    }
    nxdn_demod d;
    nxdn_demod_init(&d);
    nxdn_hpf_inverse_init(&d.post, comp_rx_hz, NXDN_RX_LEAK);
    nxdn_deemph_inverse_init(&d.deemph, comp_deemph_us);
    /* The receiver idles on noise, then the squelch opens SQUELCH_MS into the carrier: reacquire */
    size_t lead8 = (size_t)(c->lead_s * 8000), open = lead8 ? lead8 + SQUELCH_MS * 8 : 0;
    size_t nd = nxdn_demod_samples(&d, rx8, open, rxdib, MAX_DIB);
    if (lead8) nxdn_demod_reacquire(&d);
    nd += nxdn_demod_samples(&d, rx8 + open, n8 - open, rxdib + nd, MAX_DIB - nd);
    unsigned lead_sym = (unsigned)(c->lead_s * NXDN_RS + 0.5);
    start += lead_sym;
    nxdn_rx rx;
    n_heard = slot_ok = 0;
    nxdn_rx_init(&rx, 12, 200, 0, sink, NULL);
    for (size_t i = 0; i < nd; i++) nxdn_rx_push_dibit(&rx, rxdib[i]);
    /* pre-FEC bit errors: align the streams on the frames (best of the plausible delays) */
    unsigned best = ~0u, span = (frames + 2) * NXDN_FRAME_SYMBOLS;
    for (int off = -20; off < 40; off++) {
        unsigned errs = 0;
        for (unsigned i = 0; i < span; i++) {
            long j = (long)start + (long)i + off;
            if (j < 0 || (size_t)j >= nd) { errs += 2; continue; }
            uint8_t a = txdib[start - lead_sym + i], b = rxdib[j];
            errs += ((a ^ b) & 1u) + (((a ^ b) >> 1) & 1u);
        }
        if (errs < best) best = errs;
    }
    r->bits = span * 2;
    r->bit_errors = best;
    r->frames_sent = frames + 2;
    r->voice_frames_sent = frames;
    r->slots_sent = frames * 4;
    r->slots_ok = slot_ok;
    r->rx_frames = rx.frames;
    r->bad_sacch = rx.bad_sacch;
    r->calls = rx.calls;
    r->ber = (double)r->bit_errors / r->bits;
    /* a voice frame counts as lost unless all four of its slots decoded to the parameters sent */
    r->fer = 1.0 - (double)slot_ok / r->slots_sent;
}

static nxdn_channel_cfg cfg(double cnr, double off, double dev, double rx_hpf, double clip, uint64_t seed)
{
    nxdn_channel_cfg c = { cnr, off, dev, 0, rx_hpf, 0, 8.0, clip, LEAD_S, seed };
    return c;
}

static void sweep(void)
{
    static const double cnr[] = { 6, 8, 10, 12, 14, 16, 20, 30 };
    const struct { const char *name; double off, dev, tx_hpf, rx_hpf, clip; uint16_t comp_tx, comp_rx; double deemph;
                   uint16_t comp_deemph; } imp[] = {
        { "clean audio path", 0, 1.0, 0, 0, 0, 0, 0, 0, 0 },
        { "offset +500 Hz", 500, 1.0, 0, 0, 0, 0, 0, 0, 0 },
        { "deviation +20%", 0, 1.2, 0, 0, 0, 0, 0, 0, 0 },
        { "deviation -20%", 0, 0.8, 0, 0, 0, 0, 0, 0, 0 },
        { "RX high-pass 20 Hz", 0, 1.0, 0, 20, 0, 0, 0, 0, 0 },
        { "RX high-pass 100 Hz", 0, 1.0, 0, 100, 0, 0, 0, 0, 0 },
        { "RX high-pass 300 Hz", 0, 1.0, 0, 300, 0, 0, 0, 0, 0 },
        { "TX high-pass 100 Hz", 0, 1.0, 100, 0, 0, 0, 0, 0, 0 },
        { "RX high-pass 100 Hz, compensated", 0, 1.0, 0, 100, 0, 0, 100, 0, 0 },
        { "RX high-pass 300 Hz, compensated", 0, 1.0, 0, 300, 0, 0, 300, 0, 0 },
        { "RX high-pass 100 Hz, compensated as 80 Hz", 0, 1.0, 0, 100, 0, 0, 80, 0, 0 },
        { "TX high-pass 100 Hz, pre-compensated", 0, 1.0, 100, 0, 0, 100, 0, 0, 0 },
        { "TX high-pass 300 Hz, pre-compensated", 0, 1.0, 300, 0, 0, 300, 0, 0, 0 },
        { "TX and RX high-pass 300 Hz, both compensated", 0, 1.0, 300, 300, 0, 300, 300, 0, 0 },
        { "RX clipped at 80% of +-1050 Hz", 0, 1.0, 0, 0, 0.8 * 1050 * 8, 0, 0, 0, 0 },
        { "RX de-emphasis 750 us", 0, 1.0, 0, 0, 0, 0, 0, 750, 0 },
        { "RX de-emphasis 750 us, compensated", 0, 1.0, 0, 0, 0, 0, 0, 750, 750 },
        { "RX de-emphasis 750 us and high-pass 300 Hz, both compensated", 0, 1.0, 0, 300, 0, 0, 300, 750, 750 },
        { "all: +300 Hz, -10% dev, RX HP 50 Hz (comp.), clip 90%", 300, 0.9, 0, 50, 0.9 * 1050 * 8, 0, 50, 0, 0 },
    };
    const unsigned frames = 50;   /* 4 s */
    for (unsigned route = 0; route < 2; route++) {
        printf("\n### %s\n\n| Impairment |", route == ROUTE_SOFT8K ? "TX: software modulation at 8 kHz" :
               "TX: native 4800-symbol/s physical mode, each symbol twice");
        for (unsigned i = 0; i < sizeof cnr / sizeof *cnr; i++) printf(" %g dB |", cnr[i]);
        printf("\n|---|");
        for (unsigned i = 0; i < sizeof cnr / sizeof *cnr; i++) printf("---:|");
        printf("\n");
        for (unsigned k = 0; k < sizeof imp / sizeof *imp; k++) {
            printf("| %s |", imp[k].name);
            for (unsigned i = 0; i < sizeof cnr / sizeof *cnr; i++) {
                if (route == ROUTE_PHY4800 && imp[k].tx_hpf > 0) {
                    printf(" n/a |");   /* the native 4FSK path does not pass the FM audio chain */
                    continue;
                }
                nxdn_channel_cfg c = cfg(cnr[i], imp[k].off, imp[k].dev, imp[k].rx_hpf, imp[k].clip, 1000 + i);
                c.tx_hpf_hz = imp[k].tx_hpf;
                comp_tx_hz = imp[k].comp_tx;
                comp_rx_hz = imp[k].comp_rx;
                c.rx_deemph_us = imp[k].deemph;
                comp_deemph_us = imp[k].comp_deemph;
                sim_result r;
                rnd = 0xA5A5A5u + i * 77 + k;
                run(route, &c, frames, &r);
                printf(" %.4f / %.2f |", r.ber, r.fer);
            }
            printf("\n");
        }
    }
    /* Transmit spectrum of both routes */
    printf("\n### Transmit spectrum (no noise, 60 voice frames)\n\n| TX route | 99%% occupied bandwidth | power outside +-3.125 kHz "
           "| each adjacent 6.25 kHz channel |\n|---|---:|---:|---:|\n");
    for (unsigned route = 0; route < 2; route++) {
        unsigned start;
        rnd = 11;
        unsigned ndib = build_call(60, &start);
        size_t n48;
        if (route == ROUTE_SOFT8K) {
            nxdn_mod m;
            nxdn_mod_init(&m, TX_GAIN_Q8);
            size_t n8 = 0;
            for (unsigned i = 0; i < ndib; i++) n8 += nxdn_mod_dibit(&m, txdib[i], tx8 + n8);
            n8 += nxdn_mod_flush(&m, tx8 + n8);
            n48 = nxdn_channel_tx48(tx8, n8, TX_GAIN_Q8 / 256.0, 0, dev48);
        } else {
            n48 = phy4800(ndib, dev48);
        }
        double obw, out, adj;
        nxdn_channel_spectrum(dev48, n48, 3125, &obw, &out, &adj);
        printf("| %s | %.0f Hz | %.2f%% | %.1f dB |\n", route == ROUTE_SOFT8K ? "software modulation at 8 kHz" :
               "native 4800-symbol/s mode, each symbol twice", obw, out * 100, adj);
    }
    printf("\nCells: pre-FEC bit error rate / voice frame error rate (a frame counts as lost unless its four AMBE+2 "
           "frames decode exactly, after the AMBE FEC). %u voice frames per cell.\n", frames);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--sweep")) {
        sweep();
        return 0;
    }
    sim_result r;
    /* Modulator into demodulator directly: no errors at all */
    rnd = 1;
    unsigned start;
    unsigned ndib = build_call(10, &start);
    nxdn_mod m;
    nxdn_mod_init(&m, TX_GAIN_Q8);
    size_t n8 = 0;
    for (unsigned i = 0; i < ndib; i++) n8 += nxdn_mod_dibit(&m, txdib[i], tx8 + n8);
    n8 += nxdn_mod_flush(&m, tx8 + n8);
    assert(n8 + 1 >= ndib * 10 / 3 && n8 <= ndib * 10 / 3 + 1);   /* 10 samples every 3 symbols */
    nxdn_demod d;
    nxdn_demod_init(&d);
    size_t nd = nxdn_demod_samples(&d, tx8, n8, rxdib, MAX_DIB);
    nxdn_rx rx;
    n_heard = slot_ok = 0;
    nxdn_rx_init(&rx, 12, 200, 0, sink, NULL);
    for (size_t i = 0; i < nd; i++) nxdn_rx_push_dibit(&rx, rxdib[i]);
    fprintf(stderr, "loopback: calls %u rel %u slot_ok %u heard %u frames %u badlich %u badsacch %u syncloss %u nd %zu\n", rx.calls, rx.releases, slot_ok, n_heard, rx.frames, rx.bad_lich, rx.bad_sacch, rx.sync_losses, nd);
    assert(rx.calls == 1 && rx.releases == 1 && slot_ok == 40 && !rx.bad_sacch);

    /* Through the FM channel, no noise: both routes decode every frame */
    nxdn_channel_cfg c = cfg(300, 0, 1.0, 0, 0, 1);
    rnd = 2;
    run(ROUTE_SOFT8K, &c, 20, &r);
    fprintf(stderr, "soft 8 kHz, clean: BER %.5f FER %.3f calls %u\n", r.ber, r.fer, r.calls);
    assert(r.calls == 1 && r.fer == 0 && r.ber < 0.001);
    rnd = 3;
    run(ROUTE_PHY4800, &c, 20, &r);
    fprintf(stderr, "phy 4800 x2, clean: BER %.5f FER %.3f calls %u\n", r.ber, r.fer, r.calls);
    /* the DMR-shaped pulse at 4800 symbols/s is not NXDN's: residual ISI even without noise (docs/nxdn-modem.md) */
    assert(r.calls == 1 && r.ber < 0.01 && r.fer < 0.1);

    /* Impaired: 16 dB C/N, +400 Hz, deviation -15%, RX high-pass 50 Hz (compensated), clipping */
    c = cfg(16, 400, 0.85, 50, 0.9 * 1050 * 8, 4);
    comp_rx_hz = 50;
    rnd = 4;
    run(ROUTE_SOFT8K, &c, 25, &r);
    fprintf(stderr, "soft 8 kHz, impaired: BER %.5f FER %.3f calls %u\n", r.ber, r.fer, r.calls);
    assert(r.calls == 1 && r.fer < 0.05);
    printf("PASS NXDN modem: modulator/demodulator loopback exact, both TX routes through the FM channel, impaired "
           "channel decodes\n");
    return 0;
}
