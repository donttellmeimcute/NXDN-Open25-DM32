/* SPDX-License-Identifier: GPL-3.0-or-later */
/* P25 Phase 2 group voice on one logical channel (timeslot) of a TDMA
 * voice channel, above the physical layer: the bursts a sender puts in its
 * channel's positions of each superframe, and a receiver that finds the
 * superframe in a dibit stream and hands out the channel's MAC PDUs and
 * voice code words.
 *
 * Sender (p2_tx), per call: MAC_PTT on the FACCH (twice), then voice in a
 * 4V 4V 4V 4V 2V cycle (18 frames every 360 ms) with the encryption sync
 * (clear: algorithm 0x80) spread over the 4V ESS-B parts and the 2V ESS-A,
 * MAC_ACTIVE with the Group Voice Channel User message on the SACCH, then
 * MAC_END_PTT (twice), MAC_HANGTIME, MAC_IDLE. A site's outbound and a
 * subscriber's inbound use the same logical bursts; only the physical
 * framing differs (docs/p25-phase2.md: the inbound physical burst is not
 * built here).
 *
 * Receiver (p2_rx): S-ISCH sync, the burst number from the I-ISCH, the
 * DUID, descrambling, SACCH / FACCH decoding, 4V / 2V code words, the ESS.
 * Encrypted voice (an algorithm other than 0x80 in MAC_PTT or the ESS) is
 * reported as such; nothing is decrypted.
 *
 * No allocation, no RTOS, no hardware. */
#ifndef P25P2_P2_CALL_H
#define P25P2_P2_CALL_H
#include <stdbool.h>
#include <stdint.h>
#include "p2.h"
#include "p2_mac.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The AMBE+2 silence frame in its 72-bit air form (as DMR's) */
extern const uint8_t p2_silence_vcw[P2_VCW_BYTES];

enum { P2_TX_IDLE, P2_TX_PTT, P2_TX_VOICE, P2_TX_END, P2_TX_HANG };
enum { P2_TX_QUEUE = 24, P2_TX_PTT_BURSTS = 2, P2_TX_END_BURSTS = 2, P2_TX_HANG_BURSTS = 5 };

typedef struct {
    uint16_t nac;
    uint8_t state;
    uint16_t group;
    uint32_t source;
    uint8_t left;                   /* bursts left in PTT / END / HANG */
    bool ending;
    uint8_t cycle;                  /* the next voice burst: 0-3 a 4V, 4 the 2V */
    uint8_t ess[P2_ESS_HEX];
    uint8_t q[P2_TX_QUEUE][P2_VCW_BYTES];
    uint8_t head, count;
    uint32_t bursts, voice_frames, padded;
} p2_tx;

void p2_tx_init(p2_tx *tx, uint16_t nac);
void p2_tx_begin(p2_tx *tx, uint16_t group, uint32_t source);
/* One 20 ms frame; false when the queue is full */
bool p2_tx_voice(p2_tx *tx, const uint8_t vcw[P2_VCW_BYTES]);
/* PTT up: the queued frames go out (the last 4V / 2V padded with silence), then the end */
void p2_tx_end(p2_tx *tx);
/* The payload for this channel's burst at superframe position b (b must be one of its positions);
 * returns the DUID */
uint8_t p2_tx_burst(p2_tx *tx, unsigned b, uint8_t payload[P2_PAYLOAD_DIBITS]);
static inline bool p2_tx_busy(const p2_tx *tx)
{
    return tx->state != P2_TX_IDLE;
}
/* A site's outbound burst b of superframe n: the ISCH, then the burst of the channel b belongs to, scrambled */
void p2_outbound_burst(p2_tx *ch[2], unsigned b, unsigned n, const uint8_t mask[P2_MASK_BYTES],
                       uint8_t out[P2_BURST_DIBITS]);

/* Receiver */
typedef void (*p2_mac_cb)(void *ctx, uint8_t channel, const p2_mac *m);
typedef void (*p2_voice_cb)(void *ctx, uint8_t channel, const uint8_t vcw[P2_VCW_BYTES], bool encrypted);

enum { P2_RX_FOLLOW_BOTH = 0xFF };

typedef struct {
    uint8_t follow;                 /* 0, 1 or P2_RX_FOLLOW_BOTH */
    bool scrambled;
    uint8_t mask[P2_MASK_BYTES];
    p2_mac_cb on_mac;
    p2_voice_cb on_voice;
    void *ctx;
    /* sync */
    uint64_t shift;
    bool locked, known;
    uint8_t burst, misses;
    uint16_t pos;
    uint8_t buf[P2_BURST_DIBITS];
    /* per channel */
    struct {
        uint8_t ess[P2_ESS_HEX];
        uint8_t v4;                 /* 4V bursts since the last 2V or MAC_PTT */
        uint8_t algid;
        bool encrypted;
    } ch[2];
    uint32_t bursts, mac_ok, mac_bad, vcws, sync_losses, isch_errors;
} p2_rx;

void p2_rx_init(p2_rx *rx, uint8_t follow, uint32_t wacn, uint16_t sysid, uint16_t nac, p2_mac_cb on_mac,
                p2_voice_cb on_voice, void *ctx);
void p2_rx_dibit(p2_rx *rx, uint8_t dibit);
void p2_rx_reset(p2_rx *rx);
/* A burst whose superframe position the caller knows (a site's inbound): decoded as the stream receiver would */
void p2_rx_burst(p2_rx *rx, unsigned b, const uint8_t burst[P2_BURST_DIBITS]);

#ifdef __cplusplus
}
#endif
#endif
