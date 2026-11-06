/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "selftest.h"
#include "phy_stream.h"
#include "p25_runtime.h"

static void sink(void *ctx, const int16_t pcm[160])
{
    p25_selftest_result *r = ctx;
    r->pcm_blocks++;
    for (unsigned i = 0; i < 160; i++) {
        r->nonzero_samples += pcm[i] != 0;
        uint16_t sample = (uint16_t)pcm[i];
        for (unsigned j = 0; j < 2; j++) {
            r->pcm_crc ^= (sample >> (8 * j)) & 255u;
            for (unsigned b = 0; b < 8; b++)
                r->pcm_crc = (r->pcm_crc >> 1) ^ (0xedb88320u & (0u - (r->pcm_crc & 1u)));
        }
    }
}

void p25_selftest_run(p25_voice *v, p25_selftest_result *r, uint32_t (*micros)(void), void (*yield)(void))
{
    uint8_t air[576] = {0};
    size_t at = 0, valid_bytes = 0;
    memset(r, 0, sizeof *r);
    r->session_bytes = sizeof *v;
    r->pcm_crc = 0xffffffffu;
    p25_voice_init(v, 0x293, 42, sink, r);
    const p25_call call = {0x293, 42, 123456, 0, 0, 0};
    if (!p25_voice_tx_begin(v, &call)) return;
    for (unsigned f = 0; f < 18; f++) {
        int16_t pcm[160];
        for (unsigned i = 0; i < 160; i++) {
            unsigned phase = (f * 160 + i) % 40;
            pcm[i] = (int16_t)(phase < 20 ? (int)phase * 900 - 9000 : 27000 - (int)phase * 900);
        }
        uint32_t start = micros ? micros() : 0;
        if (!p25_voice_tx_pcm(v, pcm)) return;
        uint32_t elapsed = micros ? micros() - start : 0;
        if (!f) r->tx_first_us = elapsed;
        r->tx_total_us += elapsed;
        r->tx_over_20ms += elapsed > 20000u;
        if (elapsed > r->tx_block_max_us) r->tx_block_max_us = elapsed;
        if (f == 8 || f == 17) {
            /* Drain known complete packets only; microphone pacing has its own tests. */
            unsigned blocks = f == 8 ? 8 : 6;
            for (unsigned n = 0; n < blocks; n++) {
                if (p25_phy_tx_block(v, &air[at]) != 144 || v->fault) return;
                at += 36;
                valid_bytes += 36;
            }
        }
        if (yield) yield();
    }
    if (!p25_voice_tx_end(v)) return;
    for (unsigned i = 0; i < 2; i++) {
        size_t n = p25_phy_tx_block(v, &air[at]);
        if (n != (i ? 36u : 144u) || v->fault) return;
        at += 36;
        valid_bytes += n / 4;
    }
    r->air_bytes = (uint32_t)valid_bytes;
    if (!p25_voice_tx_done(v)) return;
    p25_phy_rx phy = {0};
    for (size_t i = 0; i < at; i += 36) {
        /* Synthetic kind-0 DMR deframer layout: CACH24, sync48, info108x2.
         * This exercises the adapter, not the unproven silicon delivery. */
        uint8_t ram[36] = {0};
        for (unsigned b = 0; b < 288; b++) {
            unsigned from = b < 24 ? b : b < 72 ? b + 108 : b < 180 ? b - 48 : b;
            ram[b / 8] |= (uint8_t)(((air[i + from / 8] >> (7 - from % 8)) & 1u) << (7 - b % 8));
        }
        uint32_t start = micros ? micros() : 0;
        if (p25_phy_rx_record(&phy, v, (uint32_t)(i / 36), 0, ram)) return;
        uint32_t elapsed = micros ? micros() - start : 0;
        if (elapsed > r->rx_unit_max_us) r->rx_unit_max_us = elapsed;
        if (yield) yield();
    }
    r->pcm_crc ^= 0xffffffffu;
    r->ok = valid_bytes == 549 && r->pcm_blocks == 18 && r->nonzero_samples &&
            v->rx_frames == 4 && !v->rx_muted && !v->rx_erasures && !v->fault;
}
