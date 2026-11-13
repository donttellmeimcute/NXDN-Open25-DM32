/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "nxdn_link.h"
#include <string.h>

enum { TAIL_SYMBOLS = 24 };   /* dotting after the release frame, while the PA drops */

void nxdn_link_init(nxdn_link *l, const nxdn_audio_path *path, uint8_t ran, uint16_t group, uint16_t unit,
                    nxdn_slot_sink sink, void *ctx)
{
    memset(l, 0, sizeof *l);
    l->path = *path;
    nxdn_demod_init(&l->demod);
    nxdn_hpf_inverse_init(&l->demod.post, path->rx_hpf_hz, NXDN_RX_LEAK);
    nxdn_deemph_inverse_init(&l->demod.deemph, path->rx_deemph_us);
    nxdn_rx_init(&l->rx, ran, group, unit, sink, ctx);
}

void nxdn_link_rx_audio(nxdn_link *l, const int16_t *pcm, size_t n)
{
    uint8_t dib[48];
    while (n) {
        /* at most 3 dibits per 10 samples, rounded up */
        size_t chunk = n > 150 ? 150 : n;
        size_t nd = nxdn_demod_samples(&l->demod, pcm, chunk, dib, sizeof dib);
        for (size_t i = 0; i < nd; i++) nxdn_rx_push_dibit(&l->rx, dib[i]);
        /* The radio's receive audio has no squelch edge to restart on: while the frame layer hunts,
         * the demodulator acquires again at a carrier's onset (nxdn_demod.onset_reacquire) */
        l->demod.onset_reacquire = l->rx.state == NXDN_RX_HUNT;
        pcm += chunk;
        n -= chunk;
        l->rx_samples += (uint32_t)chunk;
    }
}

void nxdn_link_rx_restart(nxdn_link *l)
{
    nxdn_demod_reacquire(&l->demod);
    nxdn_rx_reset(&l->rx);
}

static uint16_t space(const nxdn_link *l)
{
    return (uint16_t)(NXDN_LINK_RING - nxdn_link_queued(l));
}

static void put_samples(nxdn_link *l, const int16_t *s, size_t n)
{
    uint16_t h = l->head;
    for (size_t i = 0; i < n; i++) l->ring[(uint16_t)(h + i) % NXDN_LINK_RING] = s[i];
    /* the samples are in place before the interrupt can see the new head */
    __atomic_store_n(&l->head, (uint16_t)(h + n), __ATOMIC_RELEASE);
    l->tx_samples += (uint32_t)n;
}

static void symbol(nxdn_link *l, uint8_t dibit)
{
    int16_t out[4];
    put_samples(l, out, nxdn_mod_dibit(&l->mod, dibit, out));
}

void nxdn_link_tx_begin(nxdn_link *l, const nxdn_tx_cfg *cfg)
{
    nxdn_mod_init(&l->mod, l->path.tx_gain_q8);
    nxdn_hpf_inverse_init(&l->mod.pre, l->path.tx_hpf_hz, NXDN_TX_LEAK);
    nxdn_tx_begin(&l->tx, cfg, 1);
    __atomic_store_n(&l->tail, l->head, __ATOMIC_RELAXED);
    l->tail_queued = l->flushed = false;
    l->staged = 0;
    l->state = NXDN_LINK_TX;
    for (unsigned i = 0; i < NXDN_PREAMBLE_SYMBOLS; i++) l->pend[i] = nxdn_preamble_dibit(i, NXDN_PREAMBLE_SYMBOLS);
    l->pos = 0;
    l->len = NXDN_PREAMBLE_SYMBOLS;
}

void nxdn_link_tx_fill(nxdn_link *l, nxdn_slot_source next, void *ctx)
{
    if (l->state == NXDN_LINK_IDLE || l->flushed) return;
    uint8_t f[NXDN_FRAME_BYTES];
    while (space(l) >= 4) {
        if (l->pos < l->len) {
            symbol(l, l->pend[l->pos++]);
            continue;
        }
        if (l->tail_queued) {
            /* the tail is out: what the modulator still holds, and the transmission is over */
            int16_t out[16];
            if (space(l) < 16) return;
            put_samples(l, out, nxdn_mod_flush(&l->mod, out));
            l->flushed = true;
            l->state = NXDN_LINK_TX_DRAIN;
            return;
        }
        int r = nxdn_tx_frame(&l->tx, f);
        if (r > 0) {
            for (unsigned i = 0; i < NXDN_FRAME_SYMBOLS; i++) l->pend[i] = (uint8_t)((f[i / 4] >> (6 - 2 * (i % 4))) & 3u);
            l->pos = 0;
            l->len = NXDN_FRAME_SYMBOLS;
            l->tx_frames++;
            continue;
        }
        if (r < 0) {
            /* the release frame is out: dotting while the PA drops */
            for (unsigned i = 0; i < TAIL_SYMBOLS; i++) l->pend[i] = i & 1 ? 3 : 1;
            l->pos = 0;
            l->len = TAIL_SYMBOLS;
            l->tail_queued = true;
            continue;
        }
        /* the next frame waits for its four voice slots */
        while (l->staged < 4 && next && next(ctx, l->stage[l->staged])) l->staged++;
        if (l->staged < 4 && nxdn_link_queued(l) >= NXDN_LINK_LOW_WATER) return;
        /* late vocoder: silence first, then the voice there is, so a talk spurt is never cut */
        for (unsigned k = l->staged; k < 4; k++) {
            nxdn_tx_voice(&l->tx, nxdn_silence_slot);
            l->tx_silence_slots++;
        }
        for (unsigned k = 0; k < l->staged; k++) nxdn_tx_voice(&l->tx, l->stage[k]);
        l->staged = 0;
    }
}

void nxdn_link_tx_end(nxdn_link *l)
{
    if (l->state != NXDN_LINK_TX || l->tx.state >= NXDN_TX_ENDING) return;
    /* the voice already taken goes out; the call layer pads its frame */
    for (unsigned k = 0; k < l->staged; k++) nxdn_tx_voice(&l->tx, l->stage[k]);
    l->staged = 0;
    nxdn_tx_end(&l->tx);
}

bool nxdn_link_tx_done(const nxdn_link *l)
{
    return l->state == NXDN_LINK_IDLE || (l->flushed && nxdn_link_queued(l) == 0);
}

void nxdn_link_tx_abort(nxdn_link *l)
{
    l->state = NXDN_LINK_IDLE;
    l->flushed = true;
    __atomic_store_n(&l->tail, l->head, __ATOMIC_RELEASE);
}

uint16_t nxdn_link_pull(nxdn_link *l, int16_t *pcm, uint16_t n)
{
    uint16_t t = l->tail, h = __atomic_load_n(&l->head, __ATOMIC_ACQUIRE);
    uint16_t have = (uint16_t)(h - t), k = have < n ? have : n;
    for (uint16_t i = 0; i < k; i++) pcm[i] = l->ring[(uint16_t)(t + i) % NXDN_LINK_RING];
    for (uint16_t i = k; i < n; i++) pcm[i] = 0;
    if (k < n && l->state == NXDN_LINK_TX) l->underruns++;
    __atomic_store_n(&l->tail, (uint16_t)(t + k), __ATOMIC_RELEASE);
    return k;
}
