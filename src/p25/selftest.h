/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef P25_SELFTEST_H
#define P25_SELFTEST_H
#include "voice.h"
typedef struct {
    uint32_t ok, air_bytes, pcm_blocks, pcm_crc, nonzero_samples;
    uint32_t tx_block_max_us, rx_unit_max_us, session_bytes;
    uint32_t tx_first_us, tx_total_us, tx_over_20ms;
} p25_selftest_result;
/* Offline-in-device test, no RF/audio/flash I/O. Workspace is caller owned.
 * Clock callback may be NULL; host timing must never be called CPU timing.
 * Two LDUs of synthetic PCM exercise both IMBE directions and CAI framing.
 */
void p25_selftest_run(p25_voice *workspace, p25_selftest_result *out,
                      uint32_t (*micros)(void), void (*yield)(void));
#endif
