/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The firmware's P25 transmit chain on the host, up to the modem's input:
 * 8 kHz PCM -> fixed-point IMBE encoder -> CAI (HDU, LDU1/LDU2, TDU) -> the
 * 36-byte native PHY blocks the modem is handed (p25_phy_tx_block). Writes
 * the blocks' dibits, one per byte (0..3), for tools/p25_validate.py, which
 * modulates them and asks an independent decoder (dsd-fme) what it hears.
 *
 *   p25_tx_air OUT.dibits NAC TALKGROUP SOURCE SECONDS
 */
#include "phy_stream.h"
#include "voice.h"
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static p25_voice v;

/* Speech-like test audio: a voiced vowel (pitch with jitter, three formant
 * bands) with syllable envelopes, deterministic. */
static void speech(int16_t pcm[160], unsigned block)
{
    static double phase, f1, f2, f3;
    for (unsigned i = 0; i < 160; i++) {
        double t = (block * 160.0 + i) / 8000.0;
        double env = 0.5 + 0.5 * sin(2 * M_PI * 3.1 * t);
        double pitch = 120.0 + 20.0 * sin(2 * M_PI * 0.7 * t);
        phase += 2 * M_PI * pitch / 8000.0;
        f1 += 2 * M_PI * 700.0 / 8000.0;
        f2 += 2 * M_PI * 1200.0 / 8000.0;
        f3 += 2 * M_PI * 2500.0 / 8000.0;
        double glottal = sin(phase) > 0.9 ? 1.0 : 0.0;
        double s = env * (0.6 * sin(f1) + 0.3 * sin(f2) + 0.1 * sin(f3)) * (0.4 + glottal);
        pcm[i] = (int16_t)lrint(9000.0 * s);
    }
}

int main(int argc, char **argv)
{
    if (argc != 6 && argc != 7) {
        fprintf(stderr, "usage: %s OUT.dibits NAC TALKGROUP SOURCE SECONDS [OUT.imbe]\n", argv[0]);
        return 2;
    }
    FILE *out = fopen(argv[1], "wb");
    FILE *imbe = argc == 7 ? fopen(argv[6], "w") : NULL;
    if (!out || (argc == 7 && !imbe)) return 1;
    p25_call call = { (uint16_t)strtoul(argv[2], NULL, 16), (uint16_t)atoi(argv[3]),
                      (uint32_t)strtoul(argv[4], NULL, 10), 0, 0, 0 };
    unsigned blocks = (unsigned)(atof(argv[5]) * 50.0);
    p25_voice_init(&v, call.nac, call.talkgroup, NULL, NULL);
    if (!p25_voice_tx_begin(&v, &call)) return 1;
    uint8_t ram[36];
    unsigned sent = 0, pcm_blocks = 0, total_symbols = 0;
    int16_t pcm[160];
    /* The worker's pacing: 20 ms of microphone per encoder call, a 30 ms
     * PHY block whenever a full one is queued, the terminator at release. */
    for (unsigned ms = 0; ; ms += 10) {
        if (ms % 20 == 0 && pcm_blocks < blocks) {
            speech(pcm, pcm_blocks++);
            if (!p25_voice_tx_pcm(&v, pcm)) return 1;
            if (imbe) {
                /* The encoder's parameters as sent: u0-u3 12 bits, u4-u6 11, u7 7, one hex line of 88 bits */
                const uint16_t *u = v.voice[(v.voice_count + 8u) % 9u];
                static const unsigned width[8] = { 12, 12, 12, 12, 11, 11, 11, 7 };
                uint8_t bits[88];
                unsigned n = 0;
                for (unsigned k = 0; k < 8; k++)
                    for (int b = (int)width[k] - 1; b >= 0; b--) bits[n++] = (u[k] >> b) & 1u;
                for (unsigned i = 0; i < 88; i += 4)
                    fprintf(imbe, "%X", bits[i] << 3 | bits[i + 1] << 2 | bits[i + 2] << 1 | bits[i + 3]);
                fputc('\n', imbe);
            }
            if (pcm_blocks == blocks && !p25_voice_tx_end(&v)) return 1;
        }
        if (ms % 30 == 0) {
            size_t n = p25_phy_tx_block(&v, ram);
            if (v.fault) return 1;
            if (n) {
                for (unsigned i = 0; i < n; i++) fputc((ram[i / 4] >> (6 - 2 * (i % 4))) & 3, out);
                total_symbols += (unsigned)n;
                sent++;
            } else if (pcm_blocks == blocks && p25_voice_tx_done(&v)) break;
        }
        if (ms > blocks * 20 + 10000) return 1;
    }
    fclose(out);
    if (imbe) fclose(imbe);
    printf("{\"phy_blocks\": %u, \"symbols\": %u, \"imbe_frames\": %u, \"tx_frames\": %u}\n",
           sent, total_symbols, pcm_blocks, (unsigned)v.tx_frames);
    return 0;
}
