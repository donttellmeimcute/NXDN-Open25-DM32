/* SPDX-License-Identifier: GPL-3.0-or-later */
/* NXDN48 calls: our TX to our RX through the dibit stream, with the channel
 * filters, late entry, encryption, errors and lost sync. */
#include "nxdn_call.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t rnd = 0x5EED1234u;
static uint32_t next(void) { rnd ^= rnd << 13; rnd ^= rnd >> 17; rnd ^= rnd << 5; return rnd; }

static uint8_t sent[400][9], heard[400][9];
static unsigned n_sent, n_heard;
static void sink(void *ctx, const uint8_t slot[9]) { (void)ctx; assert(n_heard < 400); memcpy(heard[n_heard++], slot, 9); }

/* A call of `slots` voice slots as dibits; returns the dibit count */
static unsigned make_call(uint8_t *dib, const nxdn_tx_cfg *cfg, unsigned slots, uint8_t cipher)
{
    nxdn_tx tx;
    nxdn_tx_begin(&tx, cfg, 1);
    if (cipher) tx.layer3[7] = (uint8_t)(cipher << 6 | 5);
    unsigned n = 0, k = 0;
    uint8_t f[NXDN_FRAME_BYTES];
    n_sent = 0;
    for (;;) {
        if (k < slots && tx.nslots < 4) {
            for (unsigned i = 0; i < 9; i++) sent[n_sent][i] = (uint8_t)next();
            assert(nxdn_tx_voice(&tx, sent[n_sent]));
            n_sent++;
            k++;
            if (k == slots) nxdn_tx_end(&tx);
            continue;
        }
        int r = nxdn_tx_frame(&tx, f);
        if (r < 0) break;
        if (r == 0) continue;
        for (unsigned i = 0; i < NXDN_FRAME_SYMBOLS; i++) dib[n++] = (uint8_t)((f[i / 4] >> (6 - 2 * (i % 4))) & 3u);
    }
    return n;
}

static void feed(nxdn_rx *rx, const uint8_t *dib, unsigned n, unsigned from)
{
    for (unsigned i = from; i < n; i++) nxdn_rx_push_dibit(rx, dib[i]);
}

static uint8_t dib[40000];

int main(void)
{
    nxdn_tx_cfg cfg = { 12, 4001, 200, false, false, NXDN_DIR_INBOUND };
    nxdn_rx rx;
    /* 40 slots: a header, 10 voice frames, TX_REL */
    unsigned n = make_call(dib, &cfg, 40, 0);
    assert(n == 12 * NXDN_FRAME_SYMBOLS);
    n_heard = 0;
    nxdn_rx_init(&rx, 12, 200, 9, sink, NULL);
    feed(&rx, dib, n, 0);
    assert(rx.frames == 12 && rx.calls == 1 && rx.releases == 1 && !rx.in_call && !rx.bad_lich && !rx.bad_sacch);
    assert(n_heard == 40 && !memcmp(heard, sent, 40 * 9));
    assert(rx.msg.source == 4001 && rx.msg.destination == 200 && rx.msg.call_type == NXDN_CALL_GROUP);

    /* RAN 0 on RX hears any RAN; another RAN is refused */
    n_heard = 0;
    nxdn_rx_init(&rx, 0, 200, 9, sink, NULL);
    feed(&rx, dib, n, 0);
    assert(n_heard == 40);
    n_heard = 0;
    nxdn_rx_init(&rx, 5, 200, 9, sink, NULL);
    feed(&rx, dib, n, 0);
    assert(n_heard == 0 && rx.foreign_ran == 12 && rx.last_ran == 12);

    /* Group filter: another group is muted; 0 hears all; All Call plays everywhere */
    n_heard = 0;
    nxdn_rx_init(&rx, 12, 300, 9, sink, NULL);
    feed(&rx, dib, n, 0);
    assert(n_heard == 0 && rx.slots_muted == 40 && rx.calls == 1);
    n_heard = 0;
    nxdn_rx_init(&rx, 12, 0, 9, sink, NULL);
    feed(&rx, dib, n, 0);
    assert(n_heard == 40);
    nxdn_tx_cfg all = cfg;
    all.destination = NXDN_GROUP_ALL;
    n = make_call(dib, &all, 8, 0);
    n_heard = 0;
    nxdn_rx_init(&rx, 12, 300, 9, sink, NULL);
    feed(&rx, dib, n, 0);
    assert(n_heard == 8);

    /* A private call plays only on the unit it calls */
    nxdn_tx_cfg priv = cfg;
    priv.individual = true;
    priv.destination = 9;
    n = make_call(dib, &priv, 8, 0);
    n_heard = 0;
    nxdn_rx_init(&rx, 12, 200, 9, sink, NULL);
    feed(&rx, dib, n, 0);
    assert(n_heard == 8 && rx.msg.call_type == NXDN_CALL_INDIVIDUAL);
    n_heard = 0;
    nxdn_rx_init(&rx, 12, 200, 10, sink, NULL);
    feed(&rx, dib, n, 0);
    assert(n_heard == 0);

    /* Encrypted: reported, never played */
    n = make_call(dib, &cfg, 16, 2);
    n_heard = 0;
    nxdn_rx_init(&rx, 12, 200, 9, sink, NULL);
    feed(&rx, dib, n, 0);
    assert(n_heard == 0 && rx.msg.cipher_type == 2 && rx.calls == 1 && rx.slots_muted == 16);

    /* Late entry: join after the header and part of a superframe. The VCALL is
     * rebuilt from a whole superframe (four SACCH parts in order) */
    n = make_call(dib, &cfg, 40, 0);
    n_heard = 0;
    nxdn_rx_init(&rx, 12, 200, 9, sink, NULL);
    unsigned skip = 2 * NXDN_FRAME_SYMBOLS + 37;     /* the header, one voice frame and a bit */
    feed(&rx, dib, n, skip);
    assert(rx.late_entries == 1 && rx.calls == 1);
    /* the frames before the superframe completed are muted: frames 3-5 then superframe from frame 6 */
    assert(n_heard > 0 && n_heard < 40 && !memcmp(heard, sent[40 - n_heard], n_heard * 9));

    /* Channel errors: 1 in 200 bits flipped after the FSWs, everything still decodes */
    n = make_call(dib, &cfg, 40, 0);
    unsigned flips = 0;
    for (unsigned i = 0; i < n; i++) {
        if (i % NXDN_FRAME_SYMBOLS < 10) continue;
        if (next() % 100 == 0) { dib[i] ^= (uint8_t)(1u << (next() & 1)); flips++; }
    }
    n_heard = 0;
    nxdn_rx_init(&rx, 12, 200, 9, sink, NULL);
    feed(&rx, dib, n, 0);
    assert(rx.calls == 1 && rx.releases == 1 && n_heard == 40 && rx.bad_lich == rx.lich_assumed);

    /* Lost sync: a gap of noise longer than the flywheel ends the call */
    n = make_call(dib, &cfg, 40, 0);
    for (unsigned i = 3 * NXDN_FRAME_SYMBOLS; i < 8 * NXDN_FRAME_SYMBOLS; i++) dib[i] = (uint8_t)(next() & 3);
    n_heard = 0;
    nxdn_rx_init(&rx, 12, 200, 9, sink, NULL);
    feed(&rx, dib, 8 * NXDN_FRAME_SYMBOLS, 0);
    assert(rx.sync_losses == 1 && !rx.in_call);

    /* PTT released during the header: the call is still announced and released */
    nxdn_tx tx;
    nxdn_tx_begin(&tx, &cfg, 2);
    uint8_t f[NXDN_FRAME_BYTES];
    assert(nxdn_tx_frame(&tx, f) == 1);
    nxdn_tx_end(&tx);
    assert(nxdn_tx_frame(&tx, f) == 1 && nxdn_tx_frame(&tx, f) == 1 && nxdn_tx_frame(&tx, f) == -1);
    assert(tx.frames == 3 && !tx.voice_frames);

    printf("PASS NXDN calls: TX->RX, RAN/group/all-call/private filters, encrypted muted, late entry, %u bit errors, "
           "sync loss, release during the header\n", flips);
    return 0;
}
