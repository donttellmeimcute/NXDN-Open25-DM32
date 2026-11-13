/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The firmware's NXDN48 transmit chain on the host, for tools/nxdn_validate.py:
 * AMBE+2 parameters -> AMBE FEC (the DMR voice coding) -> NXDN call frames
 * (src/nxdn/nxdn_call.c) -> the 8 kHz software modulator (nxdn_modem.c).
 *
 *   nxdn_tx_air OUT.s16 OUT.ambe OUT.dibits RAN SOURCE GROUP SECONDS HEADERS
 *
 * OUT.s16: the deviation at 48 kHz (int16, 32767 = 4 kHz): the modulator's
 * 8 kHz samples through the model of the chip's FM modulator, an ideal
 * 3.6 kHz interpolator (tests/nxdn/nxdn_channel.c).
 * OUT.ambe: the 49-bit parameters of every voice frame, one per line as
 * 14 hex digits (the parameters MSB first, shifted left 7: dsd-fme's -Z form).
 * OUT.dibits: the transmitted dibits, one byte each, preamble included.
 * HEADERS: header frames before the voice (the firmware sends 1).
 * stdout: JSON with the frame counts. The parameters are pseudo-random; the
 * check is bit equality, not speech. */
#include "nxdn_call.h"
#include "nxdn_modem.h"
#include "nxdn_channel.h"
#include "dmr/ambe/ambeFec.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

enum { GAIN_Q8 = 512, MAX_SAMPLES = 8000 * 30 };

static uint32_t rnd = 0x12345678u;
static uint32_t next(void)
{
    rnd ^= rnd << 13; rnd ^= rnd >> 17; rnd ^= rnd << 5;
    return rnd;
}

static nxdn_mod mod;
static int16_t pcm8[MAX_SAMPLES + 16];
static double dev48[MAX_SAMPLES * 6];
static size_t samples;
static FILE *dib_out;

static void symbol(uint8_t dibit)
{
    fputc(dibit, dib_out);
    if (samples < MAX_SAMPLES) samples += nxdn_mod_dibit(&mod, dibit, pcm8 + samples);
}

int main(int argc, char **argv)
{
    if (argc != 9) {
        fprintf(stderr, "usage: %s OUT.s16 OUT.ambe OUT.dibits RAN SOURCE GROUP SECONDS HEADERS\n", argv[0]);
        return 2;
    }
    FILE *pcm = fopen(argv[1], "wb");
    FILE *ambe = fopen(argv[2], "w");
    dib_out = fopen(argv[3], "wb");
    if (!pcm || !ambe || !dib_out) return 1;
    nxdn_tx_cfg cfg = { (uint8_t)atoi(argv[4]), (uint16_t)atoi(argv[5]), (uint16_t)atoi(argv[6]), false, false,
                        NXDN_DIR_INBOUND };
    unsigned slots = (unsigned)(atof(argv[7]) / 0.02 + 0.5);
    slots = (slots + 3) / 4 * 4;   /* whole frames */
    unsigned headers = (unsigned)atoi(argv[8]);
    nxdn_mod_init(&mod, GAIN_Q8);
    nxdn_tx tx;
    nxdn_tx_begin(&tx, &cfg, headers);
    for (unsigned i = 0; i < NXDN_PREAMBLE_SYMBOLS; i++) symbol(nxdn_preamble_dibit(i, NXDN_PREAMBLE_SYMBOLS));
    unsigned frames = 0, voice = 0;
    uint8_t f[NXDN_FRAME_BYTES];
    for (unsigned k = 0;;) {
        if (k < slots && tx.nslots < 4) {
            uint8_t params[49], slot[9];
            unsigned long long v = 0;
            for (unsigned i = 0; i < 49; i++) {
                params[i] = next() & 1;
                v = v << 1 | params[i];
            }
            fprintf(ambe, "%014llX\n", v << 7);
            ambeFecEncode(params, slot);
            nxdn_tx_voice(&tx, slot);
            voice++;
            if (++k == slots) nxdn_tx_end(&tx);
            continue;
        }
        int r = nxdn_tx_frame(&tx, f);
        if (r < 0) break;
        if (!r) continue;
        frames++;
        for (unsigned i = 0; i < NXDN_FRAME_SYMBOLS; i++) symbol((uint8_t)((f[i / 4] >> (6 - 2 * (i % 4))) & 3u));
    }
    for (unsigned i = 0; i < 24; i++) symbol(i & 1 ? 3 : 1);   /* the tail while the PA drops */
    samples += nxdn_mod_flush(&mod, pcm8 + samples);
    size_t n48 = nxdn_channel_tx48(pcm8, samples, GAIN_Q8 / 256.0, 0, dev48);
    for (size_t i = 0; i < n48; i++) {
        long v = lrint(dev48[i] / 4000.0 * 32767);
        int16_t s = (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
        fwrite(&s, sizeof s, 1, pcm);
    }
    fclose(pcm);
    fclose(ambe);
    fclose(dib_out);
    printf("{\"tx_frames\": %u, \"header_frames\": %u, \"voice_frames\": %u, \"ambe_frames\": %u, \"samples_8k\": %zu}\n",
           frames, headers, frames - headers - 1, voice, samples);
    return 0;
}
