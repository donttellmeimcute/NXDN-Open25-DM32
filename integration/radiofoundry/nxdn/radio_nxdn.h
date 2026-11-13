/* SPDX-License-Identifier: GPL-3.0-or-later */
/* radio_dv.h on an NXDN48 channel: the board's flat FM audio paths
 * (hal_audio_tap.h fm_rx_flat / fm_tx_inject), the software modem and call
 * layer (nxdn_link.h) and the DMR voice bridge (core/dmr/ambe/ambeVoice.h:
 * the same vocoder, PCM stream and AMBE+2 FEC as DMR). Radio task only;
 * nothing blocks. radio_dv.c dispatches here for RADIO_MODE_NXDN. */
#ifndef RADIO_NXDN_H
#define RADIO_NXDN_H
#include "radio_dv.h"

bool radio_nxdn_select(const radio_dv_cfg_t *cfg);
bool radio_nxdn_ptt(bool down);
void radio_nxdn_status(radio_dv_status_t *out, uint32_t now_ms);
/* Every radio tick (10 ms): receive audio in, transmit ring topped up, voice paced to the speaker */
void radio_nxdn_service(uint32_t now_ms);

#endif
