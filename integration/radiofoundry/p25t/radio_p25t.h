/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A P25 trunking channel (RADIO_MODE_P25_TRUNK) on the radio: src/p25t's
 * subscriber logic in the radio task, its control channel decoder on the
 * native P25 receive stream (a tap in the P25 worker), FDMA voice through
 * the P25 channel path, inbound packets as short native transmissions.
 *
 * The channel record's rx_hz is the control channel; its WACN, System ID,
 * unit ID (the channel's override or the radio's DMR ID) and talkgroup
 * identify the radio on that system. Without CONFIG_P25_TRUNK_TX, or without
 * all of them, the channel only listens: it follows calls, never registers,
 * never transmits. Phase 2 (TDMA) voice channels are not followed: this
 * radio has no Phase 2 physical layer (docs/p25-phase2.md).
 *
 * radio_dv.c calls these from the radio task only. */
#ifndef RADIO_P25T_H
#define RADIO_P25T_H
#include <stdbool.h>
#include "radio_dv.h"

bool radio_p25t_select(const radio_dv_cfg_t *cfg);
bool radio_p25t_ptt(bool down);
void radio_p25t_status(radio_dv_status_t *out, uint32_t now_ms);
void radio_p25t_service(uint32_t now_ms);

#endif
