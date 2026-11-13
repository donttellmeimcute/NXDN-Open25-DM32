/* SPDX-License-Identifier: GPL-3.0-or-later */
/* NXDN48 conventional voice calls on top of the frame layer (nxdn.h).
 *
 * Voice travels as encoded 72-bit AMBE+2 3600x2450 slots (9 bytes each, the
 * same air frame and FEC as DMR's: core/dmr/ambe/ambeFec.h); the caller runs
 * the vocoder and that FEC. Four slots make one 80 ms frame.
 *
 * TX: a header frame (VCALL in both FACCH1 halves), voice frames whose SACCH
 * superframe repeats the VCALL in four 18-bit parts, then a TX_REL frame.
 * RX: frame sync search on a dibit stream, RAN filter, VCALL admission by
 * group or our unit ID, late entry from the SACCH superframe, the end on
 * TX_REL or lost sync. Encrypted calls (cipher type != 0) are reported and
 * never played; nothing is decrypted.
 * No allocation, no RTOS, no hardware: host tests run it unchanged. */
#ifndef NXDN_CALL_H
#define NXDN_CALL_H
#include "nxdn.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The AMBE+2 silence frame in its 72-bit air form (as DMR's) */
extern const uint8_t nxdn_silence_slot[9];

typedef struct {
    uint8_t ran;              /* 0-63 */
    uint16_t source;          /* our unit ID, 1-65519 */
    uint16_t destination;     /* group or unit */
    bool individual;          /* a private call to destination */
    bool emergency;
    uint8_t direction;        /* NXDN_DIR_INBOUND (to a repeater, and in direct mode) */
} nxdn_tx_cfg;

enum { NXDN_TX_IDLE, NXDN_TX_HEADER, NXDN_TX_VOICE, NXDN_TX_ENDING, NXDN_TX_RELEASE, NXDN_TX_DONE };
typedef struct {
    nxdn_tx_cfg cfg;
    uint8_t layer3[NXDN_LAYER3_BYTES];
    uint8_t slots[4][9];
    uint8_t state, nslots, headers_left, part;
    uint32_t frames, voice_frames, padded_slots;
} nxdn_tx;

void nxdn_tx_begin(nxdn_tx *tx, const nxdn_tx_cfg *cfg, unsigned header_frames);
/* One 20 ms slot. 0 when four are already waiting (pull a frame first) */
int nxdn_tx_voice(nxdn_tx *tx, const uint8_t slot[9]);
/* PTT released: the last frame is padded with silence, then TX_REL follows */
void nxdn_tx_end(nxdn_tx *tx);
/* The next complete 80 ms frame (FSW included, scrambled). 1 written, 0 none ready
 * yet (waiting for voice), -1 the call is over. */
int nxdn_tx_frame(nxdn_tx *tx, uint8_t frame[NXDN_FRAME_BYTES]);

/* Receive */
typedef void (*nxdn_slot_sink)(void *ctx, const uint8_t slot[9]);
enum { NXDN_RX_HUNT, NXDN_RX_SYNCED };
typedef struct {
    /* channel settings */
    uint8_t ran;              /* 0 hears every RAN */
    uint16_t group;           /* 0 hears every group */
    uint16_t unit;            /* our unit ID: private calls to it play */
    nxdn_slot_sink sink;
    void *ctx;
    /* sync */
    uint32_t shift;
    uint8_t frame[NXDN_FRAME_BYTES];
    uint16_t pos;             /* bits collected after the FSW start */
    uint8_t state, misses;
    /* the call */
    bool in_call, have_msg, admitted, playing;
    nxdn_call_msg msg;
    uint8_t parts[4][3];      /* SACCH superframe parts, 18 bits each */
    uint8_t part_mask, last_part;
    uint8_t last_ran, last_lich;
    /* counters */
    uint32_t frames, bad_lich, lich_assumed, bad_sacch, foreign_ran, calls, late_entries, releases;
    uint32_t slots_played, slots_muted, sync_losses;
} nxdn_rx;

void nxdn_rx_init(nxdn_rx *rx, uint8_t ran, uint16_t group, uint16_t unit, nxdn_slot_sink sink, void *ctx);
/* Dibits in air order (0..3 as in nxdn.h). Returns 1 when a frame was processed. */
int nxdn_rx_push_dibit(nxdn_rx *rx, uint8_t dibit);
/* Carrier lost or retuned: the call ends, sync is hunted again */
void nxdn_rx_reset(nxdn_rx *rx);

#ifdef __cplusplus
}
#endif
#endif
