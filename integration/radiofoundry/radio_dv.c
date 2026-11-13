/* SPDX-License-Identifier: GPL-3.0-or-later */
/* radio_dv.h on the native P25 channel: native_lab.c owns the modem
 * sessions, p25_lab.c's worker runs the codec and frames. */
#include "radio_dv.h"
#include "config.h"
#include "hal_radio.h"
#include "radio/radio.h"
#include "native_test.h"
#include <string.h>
#if CONFIG_P25_NATIVE
#include "p25_lab.h"
#include "native_modem/native_lab.h"

static radio_dv_cfg_t current;
static bool selected;

bool radio_dv_select(const radio_dv_cfg_t *cfg)
{
    if (cfg && selected && !memcmp(cfg, &current, sizeof current)) return true;
    if (selected) {
        /* Another channel, or new call settings: the receiver starts over. */
        native_lab_channel(NULL);
        selected = false;
    }
    if (!cfg) return true;
    if (cfg->mode != RADIO_MODE_P25) return false;
    p25_lab_channel c = { cfg->nac_rx, cfg->nac_tx, cfg->talkgroup, cfg->unit_id, cfg->hear_all };
    p25_lab_channel_config(&c);
    native_channel n = { cfg->rx_hz, cfg->tx_hz, cfg->power };
    if (!native_lab_channel(&n)) return false;
    current = *cfg;
    selected = true;
    return true;
}

bool radio_dv_ptt(bool down)
{
    return selected && native_lab_channel_ptt(down);
}

void radio_dv_status(radio_dv_status_t *o, uint32_t now_ms)
{
    p25_lab_rx_status rx;
    native_channel_state ch;
    memset(o, 0, sizeof *o);
    if (!selected) return;
    p25_lab_rx_status_get(&rx);
    native_lab_channel_state(&ch);
    bool recent = rx.last_frame_ms && now_ms - rx.last_frame_ms < RADIO_DV_BUSY_MS;
    o->active = ch.enabled && ch.receiving;
    o->sync = recent;
    o->sync_ours = recent && rx.ours;
    o->playing = recent && rx.playing && !rx.ended && !ch.transmitting;
    o->encrypted = rx.encrypted;
    o->emergency = rx.emergency;
    o->nac = rx.nac;
    o->talkgroup = rx.talkgroup;
    o->source = rx.source;
    o->transmitting = ch.transmitting || ch.tx_pending;
    o->keyed = ch.keyed;
    o->tx_calls = ch.calls;
    o->tx_reason = ch.reason;
    o->tx_blocks = ch.blocks;
}
#endif

uint8_t radio_dv_tx_deny(uint8_t nt_reason)
{
    switch (nt_reason) {
    case NT_OK:
    case NT_TX_COMPLETE:
    case NT_USER_STOP:
        return TXDENY_NONE;
    case NT_DENIED:
        return TXDENY_REJECTED;    /* the board refused: band, calibration, battery, gate */
    case NT_LEASE:
    case NT_DEADLINE:
        return TXDENY_TIMEOUT;
    default:
        return TXDENY_NO_DMR;      /* the modem failed: shown as "modem" */
    }
}
