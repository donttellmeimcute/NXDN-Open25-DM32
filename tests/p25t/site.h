/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A simulated P25 trunked site for host tests: a control channel that
 * broadcasts the system's identity and band plan and answers registration,
 * affiliation, de-registration and group voice requests (grant, queue, deny,
 * acknowledge), and voice channels that carry calls from simulated talkers
 * or repeat the calls units make on their inbound frequencies.
 *
 * With cfg.tdma the voice channels are Phase 2 timeslots (src/p25p2): two a
 * carrier, outbound bursts from each slot's p2_tx, inbound bursts read at the
 * slot's positions of the site's own superframe timing, and a unit's voice
 * repeated outbound.
 *
 * Time is a 24 kHz tick: Phase 1 carriers (control channel, FDMA voice) move
 * one symbol every SITE_FDMA_TICKS, Phase 2 carriers every SITE_TDMA_TICKS. Each tick: site_begin(), then every
 * radio reads the carrier it is tuned to (site_rx) and puts its symbol on
 * the frequency it transmits on (site_tx), then site_end(). Two radios on
 * one inbound frequency in the same symbol collide: the site hears noise.
 *
 * Built only into host tests; never into the radio. */
#ifndef P25T_SITE_H
#define P25T_SITE_H
#include <stdbool.h>
#include <stdint.h>
#include "p25.h"
#include "p2_call.h"
#include "tmsg.h"

enum {
    SITE_TICK_HZ = 24000,
    SITE_FDMA_TICKS = 5,            /* 4800 symbols/s */
    SITE_TDMA_TICKS = 4,            /* 6000 symbols/s */
    SITE_UNITS = 8,
    SITE_VOICE = 3,                 /* voice channels (with cfg.tdma: timeslots) */
    SITE_TDMA_CARRIERS = 2,
    SITE_QUEUE = 32,                /* outbound packets waiting for a TSDU */
    SITE_LOG = 128,
    SITE_CC_CHANNEL = 0x1000,       /* band 1, channel 0 */
    SITE_FDMA_FIRST = 0x100A,       /* voice channels: band 1, channels 10.. */
    SITE_TDMA_FIRST = 0x2014,       /* band 2 (two slots a carrier): carrier 10 slots 0 and 1, carrier 11 slot 0 */
    SITE_PROCESS_MS = 20,           /* the site's delay before an answer is ready */
    SITE_SYNC_SYMBOLS = 24,         /* an inbound carrier this long marks the inbound channel busy */
};

#define SITE_FDMA_BASE_HZ 851006250u
#define SITE_TDMA_BASE_HZ 852006250u
#define SITE_SPACING_HZ 12500u
#define SITE_TX_OFFSET_HZ (-45000000)

typedef struct {
    uint16_t nac;
    uint32_t wacn;
    uint16_t sysid;
    uint8_t rfss, site;
    uint32_t allowed[SITE_UNITS];   /* unit IDs the administrator assigned; 0 ends the list */
    uint16_t deny_group;            /* voice requests on this group are denied */
    uint16_t queue_group;           /* ... are queued, then granted queue_ms later */
    uint32_t queue_ms;
    uint32_t ack_ms;                /* nonzero: voice requests are acknowledged, then answered this much later */
    bool silent;                    /* hears inbound packets, never answers them */
    bool tdma;                      /* voice grants on the TDMA band */
    bool no_cc;                     /* the control channel is off the air */
    uint32_t seed;
} site_cfg;

typedef struct {
    uint8_t opcode;
    uint32_t source, wacn;
    uint16_t group, sysid;
    uint32_t tick;
} site_isp_log;

typedef struct {
    uint16_t group;
    uint32_t source;                /* the unit granted, or the simulated talker */
    uint16_t channel;
    bool unit;                      /* a unit's call (granted on request), else a talker's */
    uint32_t hdus, ldu1s, ldu2s, tdus;   /* frames heard on the inbound frequency (unit calls) */
    uint32_t lc_source;             /* the source and group in the unit's LDU1 link control (TDMA: MAC_PTT) */
    uint16_t lc_group;
    uint32_t sent;                  /* frames sent on the outbound frequency */
    uint32_t mac_ptts, vcws, mac_ends;   /* TDMA: what the unit's inbound bursts carried */
} site_call_log;

typedef struct {
    uint8_t state;                  /* 0 free, 1 a talker's call, 2 a unit's call, 3 hang */
    uint16_t channel;
    uint32_t down_hz, up_hz;
    uint8_t slot;
    bool tdma;
    uint16_t group;
    uint32_t source;
    uint32_t end_tick, last_up_tick;
    unsigned log;                   /* index in calls[] */
    /* outbound */
    uint8_t frame[P25_LDU_BYTES * 4];   /* one dibit a byte */
    uint16_t frame_dibits, pos;
    unsigned seq;
    uint8_t repeat[4][P25_LDU_BYTES];
    uint16_t repeat_bytes[4];
    uint8_t rq_head, rq_count;
    int sym;
    /* inbound */
    p25_rx up;
    uint8_t up_count, up_dibit;
    /* TDMA: the carrier and the voice put out (talkers' frames) */
    uint8_t carrier;
    uint32_t next_frame_tick;
} site_voice;

struct site;
typedef struct {
    struct site *owner;
    uint8_t index;
    uint32_t down_hz, up_hz;
    p2_tx ch[2];
    unsigned superframe, burst, pos;
    unsigned cur_burst, cur_pos;     /* where the symbol on the air this tick is */
    uint8_t out[P2_BURST_DIBITS];
    int sym;
    /* inbound: the burst window's dibits, by the site's timing */
    p2_rx in;
    uint8_t in_buf[P2_BURST_DIBITS];
    uint8_t up_count, up_dibit;
    unsigned in_symbols;
} site_tdma;

typedef struct site {
    site_cfg cfg;
    uint32_t tick, rnd;
    p25t_iden bands[16];
    /* control channel */
    uint32_t cc_down_hz, cc_up_hz;
    uint8_t cc[360];
    uint16_t cc_pos;
    int cc_sym;
    unsigned rotation;
    struct { uint8_t tsbk[P25T_TSBK_BYTES]; uint32_t ready; } q[SITE_QUEUE];
    unsigned nq;
    p25t_rx cc_up;
    uint8_t cc_up_count, cc_up_dibit;
    uint32_t cc_up_run;
    uint32_t busy_status, idle_status;
    /* scheduled grants (queued or acknowledged requests) */
    struct { uint16_t group; uint32_t source, tick; bool active; } later[SITE_UNITS];
    /* units */
    uint32_t registered[SITE_UNITS];
    uint16_t affiliated[SITE_UNITS];   /* group, by registered[] index */
    site_voice v[SITE_VOICE];
    site_tdma t[SITE_TDMA_CARRIERS];
    uint8_t mask[P2_MASK_BYTES];
    /* what happened */
    site_isp_log isps[SITE_LOG];
    unsigned n_isps, collisions;
    site_call_log calls[SITE_LOG];
    unsigned n_calls;
} site;

void site_init(site *s, const site_cfg *cfg);
void site_begin(site *s);
/* The symbol on a carrier this tick: -1 when nothing transmits there. *fresh: a new symbol started this tick. */
int site_rx(const site *s, uint32_t hz, bool *fresh);
/* A radio's symbol on an inbound frequency: once per symbol, on that carrier's symbol ticks */
void site_tx(site *s, uint32_t hz, uint8_t dibit);
void site_end(site *s);
/* A simulated talker's group call, granted now on the control channel, ms long */
bool site_talk(site *s, uint16_t group, uint32_t source, uint32_t ms);
bool site_is_registered(const site *s, uint32_t unit);
uint16_t site_affiliation(const site *s, uint32_t unit);   /* 0: none */
uint32_t site_ms(const site *s);

#endif
