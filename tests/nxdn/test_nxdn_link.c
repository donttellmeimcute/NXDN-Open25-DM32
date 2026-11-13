/* SPDX-License-Identifier: GPL-3.0-or-later */
/* nxdn_link: the transmit ring driven as the radio drives it (the task fills
 * every 10 ms, the interrupt pulls 16 samples every 2 ms, the vocoder makes a
 * slot every 20 ms), and its samples received by another link in 256-sample
 * halves as the FM RX RAM delivers them. */
#include "nxdn_link.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t rnd = 0xC0FFEEu;
static uint32_t next_rnd(void) { rnd ^= rnd << 13; rnd ^= rnd >> 17; rnd ^= rnd << 5; return rnd; }

enum { MAX_SLOTS = 400, MAX_SAMPLES = 8000 * 12 };
static uint8_t sent[MAX_SLOTS][9], heard[MAX_SLOTS][9];
static unsigned n_sent, n_heard, n_made;
static int16_t air[MAX_SAMPLES];
static size_t n_air;

/* the vocoder: one slot per 20 ms of simulated time after `ready_ms`, up to `limit` slots */
static uint32_t now_us, ready_ms;
static unsigned limit;
static bool source(void *ctx, uint8_t slot[9])
{
    (void)ctx;
    unsigned due = now_us / 1000 >= ready_ms ? (now_us / 1000 - ready_ms) / 20 + 1 : 0;
    if (n_sent >= limit || n_sent >= due) return false;
    if (n_sent == n_made) {
        for (unsigned i = 0; i < 9; i++) sent[n_made][i] = (uint8_t)next_rnd();
        n_made++;
    }
    memcpy(slot, sent[n_sent++], 9);
    return true;
}

static void sink(void *ctx, const uint8_t slot[9])
{
    (void)ctx;
    assert(n_heard < MAX_SLOTS);
    memcpy(heard[n_heard++], slot, 9);
}

static const nxdn_audio_path flat = { 0, 0, 0, 2048 };   /* 8 LSB per Hz */

/* Transmit `slots` voice slots, PTT released when the source has given them all */
static void transmit(nxdn_link *tx, unsigned slots, uint32_t first_slot_ms)
{
    nxdn_tx_cfg cfg = { 7, 1234, 55, false, false, NXDN_DIR_INBOUND };
    n_sent = n_made = 0;
    limit = slots;
    ready_ms = first_slot_ms;
    now_us = 0;
    n_air = 0;
    nxdn_link_tx_begin(tx, &cfg);
    nxdn_link_tx_fill(tx, source, NULL);
    bool ended = false;
    while (!nxdn_link_tx_done(tx)) {
        /* 2 ms interrupt: 16 samples */
        uint16_t got = nxdn_link_pull(tx, air + n_air, 16);
        (void)got;
        n_air += 16;
        assert(n_air + 16 < MAX_SAMPLES);
        now_us += 2000;
        if (now_us % 10000 == 0) {
            if (!ended && n_sent == slots) {
                nxdn_link_tx_end(tx);
                ended = true;
            }
            nxdn_link_tx_fill(tx, source, NULL);
        }
    }
}

int main(void)
{
    static nxdn_link tx, rx;
    nxdn_link_init(&tx, &flat, 7, 0, 0, NULL, NULL);

    /* The vocoder keeps up (as the radio's: a slot every 20 ms from PTT): no gap on air, every slot heard once */
    transmit(&tx, 100, 0);
    assert(tx.underruns == 0);
    nxdn_link_init(&rx, &flat, 7, 55, 9, sink, NULL);
    n_heard = 0;
    for (size_t i = 0; i < n_air; i += 256) nxdn_link_rx_audio(&rx, air + i, n_air - i < 256 ? n_air - i : 256);
    fprintf(stderr, "on time: frames %u silence %u underruns %u heard %u calls %u rel %u\n", tx.tx_frames,
            tx.tx_silence_slots, tx.underruns, n_heard, rx.rx.calls, rx.rx.releases);
    assert(rx.rx.calls == 1 && rx.rx.releases == 1 && rx.rx.msg.source == 1234 && rx.rx.msg.destination == 55);
    /* voice slots as sent, in order; the last frame padded with silence */
    assert(n_heard >= 100 && !memcmp(heard, sent, 100 * 9));
    for (unsigned i = 100; i < n_heard; i++) assert(!memcmp(heard[i], nxdn_silence_slot, 9));
    assert(tx.tx_silence_slots <= 3);

    /* A late vocoder (first slot 150 ms after PTT): silence slots fill in, the air never runs dry, nothing is lost */
    transmit(&tx, 60, 150);
    assert(tx.underruns == 0 && tx.tx_silence_slots > 0);
    nxdn_link_init(&rx, &flat, 7, 55, 9, sink, NULL);
    n_heard = 0;
    for (size_t i = 0; i < n_air; i += 256) nxdn_link_rx_audio(&rx, air + i, n_air - i < 256 ? n_air - i : 256);
    unsigned k = 0;
    while (k < n_heard && !memcmp(heard[k], nxdn_silence_slot, 9)) k++;
    assert(rx.rx.calls == 1 && rx.rx.releases == 1 && n_heard - k >= 60 && !memcmp(heard[k], sent, 60 * 9));

    /* Abort: the ring empties at once, the interrupt reads silence */
    transmit(&tx, 8, 0);
    nxdn_tx_cfg cfg = { 7, 1234, 55, false, false, NXDN_DIR_INBOUND };
    nxdn_link_tx_begin(&tx, &cfg);
    nxdn_link_tx_fill(&tx, NULL, NULL);
    assert(nxdn_link_queued(&tx) > 0);
    nxdn_link_tx_abort(&tx);
    int16_t s[16];
    assert(nxdn_link_tx_done(&tx) && nxdn_link_pull(&tx, s, 16) == 0 && s[0] == 0 && s[15] == 0);

    /* Receive with the audio path compensated: a de-emphasized, high-passed signal decodes once the link knows it */
    static const nxdn_audio_path curved = { 0, 750, 0, 2048 };
    transmit(&tx, 40, 0);
    /* de-emphasis at 8 kHz (a one-pole low-pass, 750 us) */
    double a = 1.0 - 1.0 / (750e-6 * 8000 + 0.5), y = 0;
    for (size_t i = 0; i < n_air; i++) air[i] = (int16_t)(y = a * y + (1 - a) * air[i]);
    nxdn_link_init(&rx, &flat, 7, 55, 9, sink, NULL);
    n_heard = 0;
    nxdn_link_rx_audio(&rx, air, n_air);
    unsigned plain = n_heard;
    nxdn_link_init(&rx, &curved, 7, 55, 9, sink, NULL);
    n_heard = 0;
    nxdn_link_rx_audio(&rx, air, n_air);
    fprintf(stderr, "de-emphasized: heard %u uncompensated, %u compensated\n", plain, n_heard);
    assert(plain < 40 && n_heard >= 40 && !memcmp(heard, sent, 40 * 9));

    /* No squelch edge on the radio: the link finds a call after noise without being told (here 300 ms of
     * discriminator noise, and a 200 Hz carrier offset) */
    transmit(&tx, 40, 0);
    static int16_t noisy[MAX_SAMPLES];
    size_t lead = 2400, m = 0;
    for (; m < lead; m++) noisy[m] = (int16_t)((int32_t)(next_rnd() % 40001u) - 20000);
    for (size_t i = 0; i < n_air && m < MAX_SAMPLES; i++) noisy[m++] = (int16_t)(air[i] + 200 * 8);
    unsigned from_header = 0;
    for (unsigned trial = 0; trial < 20; trial++) {
        /* a different phase of the noise each time: where the hunt's acquisitions fall against the preamble */
        size_t skip = trial * 37;
        nxdn_link_init(&rx, &flat, 7, 55, 9, sink, NULL);
        n_heard = 0;
        for (size_t i = skip; i < m; i += 256) nxdn_link_rx_audio(&rx, noisy + i, m - i < 256 ? m - i : 256);
        /* every call is heard; one that misses its header joins at its SACCH (late entry), in order */
        assert(rx.rx.calls == 1 && n_heard >= 28 && n_heard <= 40 && !memcmp(heard, sent[40 - n_heard], n_heard * 9));
        from_header += !rx.rx.late_entries && n_heard >= 40;
    }
    fprintf(stderr, "after noise: %u of 20 calls joined at their header\n", from_header);
    assert(from_header >= 18);   /* docs/nxdn-modem.md: about 1 call in 15 after strong noise misses it */

    printf("PASS NXDN link: TX ring on time with the vocoder's pace, late vocoder padded without gaps, abort, RX in "
           "RX RAM halves, receive path compensation, a call found after noise\n");
    return 0;
}
