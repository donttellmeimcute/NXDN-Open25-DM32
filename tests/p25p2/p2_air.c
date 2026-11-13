/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A Phase 2 voice channel's outbound stream from src/p25p2, for tools/p25p2_validate.py: dibits one per byte (the
 * OP25 / dsd-fme symbol capture format), and the calls put in as JSON on stdout.
 *
 * Three idle superframes, then a call on each logical channel (different groups and sources), each voice frame
 * an AMBE+2 frame made from known parameter bits by the radio's DMR FEC encoder (core/dmr/ambe/ambeFec.c), then
 * the ends and idle superframes. stdout: a line with the calls, then a line a channel with its frames' bits.
 *   p2_air OUT.bin WACN SYSID NAC FRAMES */
#include "ambeFec.h"
#include "p2_call.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rnd = 0x5EED1234u;
static uint32_t next(void) { rnd ^= rnd << 13; rnd ^= rnd >> 17; rnd ^= rnd << 5; return rnd; }

int main(int argc, char **argv)
{
    if (argc != 6) return 2;
    FILE *out = fopen(argv[1], "wb");
    uint32_t wacn = (uint32_t)strtoul(argv[2], NULL, 16);
    uint16_t sysid = (uint16_t)strtoul(argv[3], NULL, 16), nac = (uint16_t)strtoul(argv[4], NULL, 16);
    unsigned frames = (unsigned)atoi(argv[5]);
    uint8_t mask[P2_MASK_BYTES];
    p2_mask(wacn, sysid, nac, mask);
    p2_tx t[2];
    p2_tx *ch[2] = { &t[0], &t[1] };
    p2_tx_init(&t[0], nac);
    p2_tx_init(&t[1], nac);
    static const uint16_t group[2] = { 1234, 4321 };
    static const uint32_t source[2] = { 0xABCDE, 777 };
    unsigned sent[2] = { 0, 0 };
    printf("{\"calls\": [");
    for (unsigned c = 0; c < 2; c++)
        printf("%s{\"channel\": %u, \"group\": %u, \"source\": %u}", c ? ", " : "", c, group[c], source[c]);
    printf("]}\n");
    /* the parameter bits of every frame, by channel, printed after the stream is written */
    uint8_t (*params)[2][49] = calloc(frames, sizeof *params);
    unsigned total_bursts = 0;
    for (unsigned n = 0; n < 6 + (frames * 20 + 359) / 360 + 4; n++)
        for (unsigned b = 0; b < 12; b++, total_bursts++) {
            if (n == 3 && b == 0) {
                p2_tx_begin(ch[0], group[0], source[0]);
                p2_tx_begin(ch[1], group[1], source[1]);
            }
            for (unsigned c = 0; c < 2 && n >= 3; c++) {
                while (sent[c] < frames && sent[c] * 2 < ((n - 3) * 12 + b) * 3 + 6) {
                    for (int i = 0; i < 49; i++) params[sent[c]][c][i] = (uint8_t)(next() & 1);
                    uint8_t f[AMBE_FRAME_BYTES];
                    ambeFecEncode(params[sent[c]][c], f);
                    if (!p2_tx_voice(ch[c], f)) break;
                    sent[c]++;
                }
                if (sent[c] == frames && !ch[c]->ending) p2_tx_end(ch[c]);
            }
            uint8_t burst[P2_BURST_DIBITS];
            p2_outbound_burst(ch, b, n, mask, burst);
            fwrite(burst, 1, sizeof burst, out);
        }
    fclose(out);
    /* the frames as parameter bit strings, one line a channel */
    for (unsigned c = 0; c < 2; c++) {
        fprintf(stderr, "channel %u: %u frames, %u padded with silence\n", c, sent[c], t[c].padded);
        printf("{\"channel\": %u, \"params\": [", c);
        for (unsigned i = 0; i < frames; i++) {
            printf("%s\"", i ? ", " : "");
            for (int k = 0; k < 49; k++) putchar('0' + params[i][c][k]);
            putchar('"');
        }
        printf("], \"padded\": %u}\n", t[c].padded);
    }
    free(params);
    return 0;
}
