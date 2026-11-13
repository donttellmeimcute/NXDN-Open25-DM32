/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef P25_PHY_STREAM_H
#define P25_PHY_STREAM_H
#include "voice.h"
enum { P25_PHY_BYTES = 36, P25_PHY_SYMBOLS = 144 };
/* HR_C7000 continuous PHY experiment, not a verified P25 hardware driver.
 * HR_C7000 guide Figure 70; field-map provenance is in docs/modem.md.
 * No registers, PA control, DMR coding, CACH insertion or sync substitution.
 * All 288 bits are retained; RC layout 3 is deliberately unsupported.
 */
int p25_phy_rx_air(const uint8_t ram[36], uint8_t kind, uint8_t air[36]);
/* An optional second consumer of the receive stream (P25 trunking's control
 * channel decoder): every air dibit in order, P25_PHY_TAP_GAP at a gap. */
enum { P25_PHY_TAP_GAP = 0xFF };
typedef void (*p25_phy_tap)(void *ctx, uint8_t dibit);
typedef struct {
    uint32_t next_sequence, records, gaps, unsupported;
    uint8_t sequenced;
    p25_phy_tap tap;
    void *tap_ctx;
} p25_phy_rx;
/* Task-level consumer of copied IRQ records. Sequence counts EVERY completion,
 * including records dropped by a full ISR ring. A gap/unknown layout discards
 * admission and framer state before later data; never conceal lost bits. */
int p25_phy_rx_record(p25_phy_rx *phy, p25_voice *v, uint32_t sequence,
                      uint8_t kind, const uint8_t ram[36]);
/* Pack the next 30 ms hardware block. Return valid symbol count (0..144).
 * Short final blocks contain an explicitly NON-CAI zero-dibit physical tail.
 * Caller must account for it and measured hardware drain before unkeying.
 * A fault invalidates the whole block. No write/handoff deadline is assumed.
 */
size_t p25_phy_tx_block(p25_voice *v, uint8_t ram[36]);
/* Up to 144 dibits (one per byte) into the next 30 ms block, the rest zero. Returns the count used. */
size_t p25_phy_tx_pack(const uint8_t *dibits, size_t n, uint8_t ram[36]);
#endif
