/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef P25_VOICE_H
#define P25_VOICE_H
#include "p25.h"
#include "dm32_imbe.h"

/* Conventional, clear, group voice; half duplex. All calls and the PCM sink
 * run in ONE worker task, never in an ISR. No dynamic allocation. The sink
 * must consume/copy 160 samples before returning. See docs/conventional.md.
 * A transport supplies dibits in on-air order; this does not assert that a
 * particular chip can expose that stream, or implement RF modulation.
 */
typedef void (*p25_pcm_sink)(void *ctx, const int16_t pcm[160]);
enum { P25_VOICE_QUEUE = 3 };
typedef struct {
    uint8_t data[P25_LDU_BYTES];
    uint16_t bytes;
} p25_voice_packet;
typedef struct {
    dm32_imbe_context codec; /* RX and TX share it; every direction change resets it */
    p25_rx framer;
    p25_call call;
    p25_pcm_sink sink;
    void *sink_ctx;
    uint16_t group_filter; /* 0 = all group voice; implementation policy */
    uint16_t active_nac;
    uint16_t active_group;
    uint32_t active_source;
    uint16_t voice[9][8];
    p25_voice_packet queue[P25_VOICE_QUEUE];
    uint16_t packet_symbol;
    uint8_t head, tail, count, voice_count, next_duid;
    uint8_t transmitting, started, stopping, terminator_pending, fault;
    uint8_t crypto_clear, have_group;
    uint32_t rx_frames, rx_muted, rx_erasures, tx_frames, tx_underruns;
    uint32_t (*micros)(void); /* optional task-level profiling, no clock assumption */
    uint32_t fec_max_us, imbe_max_us;
} p25_voice;

void p25_voice_init(p25_voice *v, uint16_t nac, uint16_t group,
                    p25_pcm_sink sink, void *ctx);
/* Clear RX admission/timing after tuning, a discontinuity, or carrier loss.
 * Caller owns the receive hang timer; no time unit is guessed here. */
void p25_voice_rx_reset(p25_voice *v);
void p25_voice_rx_dibit(p25_voice *v, uint8_t dibit);
int p25_voice_tx_begin(p25_voice *v, const p25_call *call);
/* Exactly one 8 kHz / 20 ms microphone block; mutable for the codec.
 * Returns 0 on invalid state, codec failure or queue overflow. Abort/unkey.
 * Producer must be paced by microphone time, not called in an unbounded loop. */
int p25_voice_tx_pcm(p25_voice *v, int16_t pcm[160]);
/* Release: pad only the incomplete nine-frame LDU with encoded silence,
 * then a TDU. Empty, unstarted calls are canceled. Task context only. */
int p25_voice_tx_end(p25_voice *v);
/* Up to capacity dibits, including across CAI unit boundaries. Zero means
 * prefill waiting or completely drained; inspect tx_done(). No fabricated
 * idle/CACH/sync is inserted. An active underrun sets fault and ends TX.
 * Initial prefill holds HDU until the first complete LDU is queued. */
size_t p25_voice_tx_read(p25_voice *v, uint8_t *dibits, size_t capacity);
int p25_voice_tx_done(const p25_voice *v);
void p25_voice_tx_abort(p25_voice *v);
#endif
