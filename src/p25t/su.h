/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A P25 Phase 1 trunked subscriber's logic: finding the configured system's
 * control channel, the band plan, registration and group affiliation,
 * following group voice grants to voice channels (FDMA, or a TDMA slot) and
 * back, and group calls on PTT (request, queued, deny, grant). No RTOS, no
 * hardware: the caller feeds it what the receiver hears and the time, and
 * carries out what it asks (tune, send an inbound packet, transmit voice).
 *
 * Safeguards (docs/p25-trunking.md):
 * - It transmits only with tx_enabled (CONFIG_P25_TRUNK_TX builds, a system
 *   whose WACN, System ID and unit ID the user entered), and only on that
 *   system: the control channel's network and RFSS status must carry the
 *   configured WACN and System ID first.
 * - Every inbound packet carries the configured unit ID. IDs heard on the air
 *   are only shown; nothing here can put one into a transmission.
 * - Without PTT it sends only registration, group affiliation and
 *   de-registration on the configured system, each at most every
 *   P25T_SU_AUTO_GAP_MS. Emergency and supplementary services are not built.
 *
 * Inbound random access: the outbound status symbols mark the inbound channel
 * idle or busy every microslot (7.5 ms). A pending packet waits for an idle
 * microslot, then a random number of access slots (P25T_SU_SLOT_MICROSLOTS
 * each) within a window that doubles on every retry, then goes out at the
 * next idle slot boundary; without a response in P25T_SU_RESPONSE_MS it is
 * retried, P25T_SU_TRIES times in all. These values are this implementation's,
 * not TIA-102.AABD's (not available here): check them against the standard
 * before any transmission on a real system. */
#ifndef P25T_SU_H
#define P25T_SU_H
#include <stdbool.h>
#include <stdint.h>
#include "tmsg.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    P25T_SU_CC_MAX = 4,             /* control channel frequencies to hunt */
    P25T_SU_PRIORITY_MAX = 8,       /* talkgroups in the scan list, in priority order */
    P25T_SU_HUNT_MS = 3000,         /* a control channel without our system's broadcasts: the next */
    P25T_SU_CC_LOST_MS = 2000,      /* no TSBK on the control channel this long: hunt again */
    P25T_SU_VOICE_HANG_MS = 1500,   /* no voice frame this long on a voice channel: back to the control channel */
    P25T_SU_RESPONSE_MS = 1500,
    P25T_SU_TRIES = 4,
    P25T_SU_SLOT_MICROSLOTS = 6,
    P25T_SU_WINDOW_SLOTS = 4,       /* first random window, doubling per retry */
    P25T_SU_QUEUED_MS = 10000,      /* a queued or acknowledged call waits this long for its grant */
    P25T_SU_AUTO_GAP_MS = 30000,    /* automatic registration / affiliation: at most this often */
    P25T_SU_REG_RETRY_MS = 60000,   /* after a refused registration */
};

typedef struct {
    uint32_t wacn;                  /* 20 bits, user-entered */
    uint16_t sysid;                 /* 12 bits, user-entered */
    uint32_t unit_id;               /* 24 bits, assigned by the system's administrator, user-entered */
    uint16_t nac;                   /* the control channel's NAC, or P25_NAC_ANY */
    uint32_t cc_hz[P25T_SU_CC_MAX];
    uint8_t cc_count;
    uint16_t talkgroup;             /* where PTT talks; affiliated to */
    uint16_t priority[P25T_SU_PRIORITY_MAX];   /* followed too, earlier = higher priority */
    uint8_t priority_count;
    bool hear_all;                  /* follow every group grant */
    bool tx_enabled;                /* CONFIG_P25_TRUNK_TX and a configured system */
    bool tdma_rx;                   /* can follow Phase 2 TDMA voice channels */
    bool tdma_tx;                   /* can talk on them (a Phase 2 transmitter; this radio has none) */
} p25t_su_cfg;

enum p25t_su_state {
    P25T_SU_HUNT,                   /* looking for the control channel */
    P25T_SU_CC,                     /* on the control channel */
    P25T_SU_VOICE_RX,               /* following a call on a voice channel */
    P25T_SU_REQUEST,                /* PTT: requesting a channel */
    P25T_SU_QUEUED,
    P25T_SU_VOICE_TX,               /* granted: transmitting on the voice channel */
};

/* What the caller is asked to do */
typedef struct {
    bool tune;                      /* receive on rx_hz (and slot on a TDMA channel) */
    uint32_t rx_hz;
    uint8_t slot;
    bool tdma;
    bool send_isp;                  /* transmit isp (one TSBK, a TSDU with NAC nac) on tx_hz now */
    uint8_t isp[P25T_TSBK_BYTES];
    uint16_t nac;
    uint32_t tx_hz;
    bool voice_tx_start;            /* start the group call on tx_hz (FDMA, or TDMA in slot) */
    bool voice_tx_stop;             /* end it (terminator) */
    uint16_t voice_group;
    uint32_t voice_source;          /* always cfg.unit_id */
} p25t_su_out;

enum { P25T_SU_DENY_NONE, P25T_SU_DENY_SYSTEM, P25T_SU_DENY_NO_RESPONSE, P25T_SU_DENY_NOT_REGISTERED,
       P25T_SU_DENY_NO_TX, P25T_SU_DENY_QUEUE_TIMEOUT, P25T_SU_DENY_BAND };

typedef struct {
    p25t_su_cfg cfg;
    uint8_t state;
    uint32_t now;
    /* the system */
    uint8_t cc_index;
    uint32_t cc_rx_hz, cc_tx_hz;
    uint16_t cc_nac;                /* the control channel's NAC as heard */
    uint32_t state_ms, last_tsbk_ms;
    bool net_ok, rfss_ok, site_known;
    uint8_t rfss, site;
    uint16_t cc_channel;
    p25t_iden bands[16];
    /* registration and affiliation */
    bool registered, affiliated;
    uint32_t reg_sent_ms, aff_sent_ms, reg_refused_ms;
    bool reg_refused;
    /* the call followed or made */
    uint16_t call_group;
    uint32_t call_source;
    uint32_t call_rx_hz;
    uint8_t call_slot;
    bool call_tdma, call_encrypted;
    uint32_t last_voice_ms;
    uint8_t deny, deny_reason;
    bool ptt;
    /* inbound access */
    bool isp_pending;
    uint8_t isp[P25T_TSBK_BYTES];
    uint8_t isp_tries, backoff, microslots;
    bool awaiting;
    uint32_t sent_ms;
    uint32_t rnd;
    /* counters */
    uint32_t tsbks, grants_followed, isps_sent, calls_made;
} p25t_su;

void p25t_su_init(p25t_su *su, const p25t_su_cfg *cfg, uint32_t now_ms, uint32_t seed, p25t_su_out *out);
/* A TSBK heard on the control channel, in a frame with this NAC */
void p25t_su_tsbk(p25t_su *su, uint16_t nac, const uint8_t tsbk[P25T_TSBK_BYTES], p25t_su_out *out);
/* An outbound status symbol on the control channel (every microslot) */
void p25t_su_status(p25t_su *su, uint8_t status, p25t_su_out *out);
/* A voice frame on the voice channel: its group and source when it carries them (0: not in this frame), ended on a
 * terminator */
void p25t_su_voice(p25t_su *su, uint16_t group, uint32_t source, bool encrypted, bool ended, p25t_su_out *out);
void p25t_su_ptt(p25t_su *su, bool down, p25t_su_out *out);
void p25t_su_tick(p25t_su *su, uint32_t now_ms, p25t_su_out *out);
/* Leaving the trunked channel: de-registration when registered and transmit is enabled */
void p25t_su_leave(p25t_su *su, p25t_su_out *out);

#ifdef __cplusplus
}
#endif
#endif
