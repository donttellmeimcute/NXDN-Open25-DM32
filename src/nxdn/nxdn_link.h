/* SPDX-License-Identifier: GPL-3.0-or-later */
/* NXDN48 between the radio's 8 kHz FM audio and 72-bit AMBE+2 voice slots.
 *
 * Receive: audio samples -> demodulator (nxdn_modem.h) -> call layer
 * (nxdn_call.h) -> the slot sink. Transmit: voice slots from a source ->
 * call frames -> modulator -> a sample ring that the board's interrupt
 * drains at 8 kHz (nxdn_link_pull).
 *
 * The radio task calls everything except nxdn_link_pull, which the board's
 * interrupt calls; the ring is single producer / single consumer. No RTOS,
 * no allocation, no hardware: host tests run it unchanged. */
#ifndef NXDN_LINK_H
#define NXDN_LINK_H
#include "nxdn_call.h"
#include "nxdn_modem.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Samples a frame takes at 8 kHz: 192 symbols x 10/3 */
#define NXDN_LINK_FRAME_SAMPLES 640u
/* The transmit ring: two frames and a half (200 ms) */
#define NXDN_LINK_RING 2048u
/* Below this many queued samples (40 ms) a missing voice slot is sent as silence
 * rather than letting the ring run dry: an empty ring is a gap on air */
#define NXDN_LINK_LOW_WATER 320u

/* The radio's audio path, as measured on it (0 = flat) */
typedef struct {
    uint16_t rx_hpf_hz, rx_deemph_us, tx_hpf_hz;
    int32_t tx_gain_q8;        /* modulator output LSB per Hz of deviation, x 256 */
} nxdn_audio_path;

/* The next 20 ms voice slot to send (72-bit AMBE+2 air frame); false: none ready yet */
typedef bool (*nxdn_slot_source)(void *ctx, uint8_t slot[9]);

enum { NXDN_LINK_IDLE, NXDN_LINK_TX, NXDN_LINK_TX_DRAIN };

typedef struct {
    nxdn_audio_path path;
    /* receive */
    nxdn_demod demod;
    nxdn_rx rx;
    /* transmit: the task fills ring[head], the interrupt reads ring[tail] */
    nxdn_tx tx;
    nxdn_mod mod;
    int16_t ring[NXDN_LINK_RING];
    volatile uint16_t head, tail;   /* free running */
    uint8_t state;
    bool flushed;
    /* counters */
    uint32_t rx_samples, tx_samples, tx_frames, tx_silence_slots, underruns;
} nxdn_link;

void nxdn_link_init(nxdn_link *l, const nxdn_audio_path *path, uint8_t ran, uint16_t group, uint16_t unit,
                    nxdn_slot_sink sink, void *ctx);

/* Receive audio, any count of samples */
void nxdn_link_rx_audio(nxdn_link *l, const int16_t *pcm, size_t n);
/* The squelch opened, or the receiver was retuned: acquire again */
void nxdn_link_rx_restart(nxdn_link *l);

/* Transmit: preamble, header, voice from fill(), release after nxdn_link_tx_end */
void nxdn_link_tx_begin(nxdn_link *l, const nxdn_tx_cfg *cfg);
/* Keep the ring ahead of the interrupt: call every 10-20 ms while transmitting */
void nxdn_link_tx_fill(nxdn_link *l, nxdn_slot_source next, void *ctx);
/* PTT released: the last frame is padded, TX_REL follows, then the tail */
void nxdn_link_tx_end(nxdn_link *l);
/* Everything, the tail included, has left the ring: the transmitter may drop */
bool nxdn_link_tx_done(const nxdn_link *l);
/* Abort at once (a fault): the ring is emptied */
void nxdn_link_tx_abort(nxdn_link *l);
/* Interrupt side: n samples into pcm; silence once the ring is empty */
uint16_t nxdn_link_pull(nxdn_link *l, int16_t *pcm, uint16_t n);
/* Samples queued */
static inline uint16_t nxdn_link_queued(const nxdn_link *l) { return (uint16_t)(l->head - l->tail); }

#ifdef __cplusplus
}
#endif
#endif
