/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The radio core's side of a digital voice channel: P25 on the modem's native
 * physical layer (radio_dv.c, p25_lab.c), NXDN48 on the board's flat FM
 * audio paths (radio_nxdn.c), P25 trunking on P25's path (radio_p25t.c). radio_sm.c calls these from the radio task
 * only; none blocks. The radio core keeps the policy (TX rules, time-out,
 * busy lockout, scan, display); this side keeps the modem, the codec and
 * the frames. Host tests supply their own implementation. */
#ifndef RADIO_DV_H
#define RADIO_DV_H
#include <stdbool.h>
#include <stdint.h>

/* A frame heard this recently counts as channel activity (busy, scan). P25
 * sends a frame at least every 180 ms during a call, NXDN every 80 ms. */
#define RADIO_DV_BUSY_MS 400u

#define RADIO_DV_NAC_ANY 0xF7Eu  /* nac_rx that hears every NAC (CODEPLUG_NAC_ANY) */

/* How a trunking PTT ended without a call (radio_dv_status_t.tx_reason; above the NT_* values) */
enum { RADIO_DV_TRUNK_REFUSED = 0x40, RADIO_DV_TRUNK_NO_TX = 0x41, RADIO_DV_TRUNK_NO_ANSWER = 0x42 };

typedef struct {
    uint8_t mode;               /* radio_mode_t: RADIO_MODE_P25, _NXDN or _P25_TRUNK */
    uint32_t rx_hz, tx_hz;      /* trunking: the control channel and its inbound */
    uint8_t power;              /* board power level for TX */
    uint16_t nac_rx, nac_tx, talkgroup;   /* NXDN: the RAN in nac_*, the group in talkgroup */
    uint32_t unit_id;
    bool hear_all;
    uint32_t wacn;              /* trunking: the system */
    uint16_t sysid;
} radio_dv_cfg_t;

typedef struct {
    bool active;                /* the channel's receiver runs */
    bool sync;                  /* frames of any NAC in the last RADIO_DV_BUSY_MS */
    bool sync_ours;             /* ... the last of them with our NAC (or the channel hears any) */
    bool playing;               /* a call that passed the filters plays */
    bool encrypted, emergency;  /* of the call heard */
    uint16_t nac, talkgroup;    /* of the last frames / call heard (NXDN: the RAN in nac) */
    uint32_t source;
    bool transmitting;          /* PTT's session pending or on air, terminator included */
    bool keyed;                 /* the PA is on */
    uint32_t tx_calls;          /* transmissions ended so far */
    uint8_t tx_reason;          /* how the last one ended: NT_* (native_test.h) */
    uint32_t tx_blocks;         /* P25: 30 ms blocks it sent; NXDN: 80 ms frames */
    uint8_t trunk_state;        /* trunking: p25t_su_state (hunting, control channel, voice, request, queued) */
    bool trunk_registered, trunk_affiliated;
} radio_dv_status_t;

/* NULL leaves the channel and hands the board back in FM before returning.
 * false: the mode is not built in, or a bench session holds the modem. */
bool radio_dv_select(const radio_dv_cfg_t *cfg);
/* down: start a transmission (false: refused); up: end the call (the
 * terminator goes out, then the PA drops). */
bool radio_dv_ptt(bool down);
void radio_dv_status(radio_dv_status_t *out, uint32_t now_ms);
/* Every radio tick: the work a channel does in the radio task (NXDN's modem) */
void radio_dv_service(uint32_t now_ms);
/* How the last transmission ended: 0 normally, else a radio core TXDENY_* value. */
uint8_t radio_dv_tx_deny(uint8_t nt_reason);

#endif
