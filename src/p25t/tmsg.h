/* SPDX-License-Identifier: GPL-3.0-or-later */
/* P25 Phase 1 trunking messages carried in TSBKs (tsbk.h): the outbound
 * signalling packets (OSP) a subscriber follows and the inbound ones (ISP) it
 * sends. Field positions are bit numbers in the 96-bit TSBK (octet 0 bit 7 =
 * bit 0), as sdrtrunk's decoder lists them (docs/protocol-sources.json).
 *
 * Only what conventional group voice on a trunked system needs: identifier
 * updates (the band plan), group voice grants and updates, registration,
 * affiliation, acknowledge / queued / deny, and the RFSS and network status
 * broadcasts that identify the site. Emergency, supplementary services,
 * individual calls and data are out of scope. */
#ifndef P25T_TMSG_H
#define P25T_TMSG_H
#include <stdbool.h>
#include <stdint.h>
#include "tsbk.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    /* outbound */
    P25T_OSP_GRP_V_CH_GRANT = 0x00,
    P25T_OSP_GRP_V_CH_GRANT_UPDT = 0x02,
    P25T_OSP_ACK_RSP_FNE = 0x20,
    P25T_OSP_QUE_RSP = 0x21,
    P25T_OSP_DENY_RSP = 0x27,
    P25T_OSP_GRP_AFF_RSP = 0x28,
    P25T_OSP_U_REG_RSP = 0x2C,
    P25T_OSP_U_DE_REG_ACK = 0x2F,
    P25T_OSP_IDEN_UP_TDMA = 0x33,
    P25T_OSP_IDEN_UP_VU = 0x34,
    P25T_OSP_RFSS_STS_BCST = 0x3A,
    P25T_OSP_NET_STS_BCST = 0x3B,
    P25T_OSP_IDEN_UP = 0x3D,
    /* inbound */
    P25T_ISP_GRP_V_REQ = 0x00,
    P25T_ISP_GRP_AFF_REQ = 0x28,
    P25T_ISP_U_DE_REG_REQ = 0x2B,
    P25T_ISP_U_REG_REQ = 0x2C,
};

/* Response values of U_REG_RSP and GRP_AFF_RSP */
enum { P25T_RSP_ACCEPT = 0, P25T_RSP_FAIL = 1, P25T_RSP_DENY = 2, P25T_RSP_REFUSED = 3 };

/* A band's identifier update (IDEN_UP, IDEN_UP_VU, IDEN_UP_TDMA) */
typedef struct {
    bool valid;
    bool tdma;
    uint8_t slots;            /* time slots a carrier has: 1 (FDMA), 2 or 4 */
    uint8_t channel_type;     /* IDEN_UP_TDMA's access type */
    uint32_t bandwidth_hz;
    uint32_t spacing_hz;
    uint32_t base_hz;
    int32_t tx_offset_hz;
    bool tx_ok;               /* the band has a subscriber transmit frequency */
} p25t_iden;

typedef struct {
    uint8_t opcode, mfid;
    bool protect;
    union {
        struct { uint8_t svc; uint16_t channel, group; uint32_t source; } grant;
        struct { uint16_t channel[2], group[2]; } update;
        struct { uint8_t response; uint16_t sysid; uint32_t source_id, source_address; } reg;
        struct { bool global; uint8_t response; uint16_t announce_group, group; uint32_t target; } aff;
        struct { bool aiv; uint8_t service, reason; uint32_t info, target; } deny;   /* DENY_RSP and QUE_RSP */
        struct { bool aiv, ex; uint8_t service; uint32_t wacn; uint16_t sysid; uint32_t source, target; } ack;
        struct { uint8_t lra; bool active; uint16_t sysid; uint8_t rfss, site; uint16_t channel; uint8_t ssc; } rfss;
        struct { uint8_t lra; uint32_t wacn; uint16_t sysid; uint16_t channel; uint8_t ssc; } net;
        struct { uint32_t wacn; uint16_t sysid; uint32_t target; } dereg;
        struct { uint8_t id; p25t_iden band; } iden;
    } u;
} p25t_osp;

typedef struct {
    uint8_t opcode;
    union {
        struct { uint8_t svc; uint16_t group; uint32_t source; } voice;
        struct { uint16_t sysid, group; uint32_t source; } aff;
        struct { bool emergency; uint8_t capability; uint32_t wacn; uint16_t sysid; uint32_t source; } reg;
        struct { uint32_t wacn; uint16_t sysid; uint32_t source; } dereg;
    } u;
} p25t_isp;

/* Into a TSBK (standard MFID 0, LB and P clear, CRC sealed). false: an opcode not covered */
bool p25t_osp_build(const p25t_osp *m, uint8_t tsbk[P25T_TSBK_BYTES]);
bool p25t_isp_build(const p25t_isp *m, uint8_t tsbk[P25T_TSBK_BYTES]);
/* From a TSBK whose CRC checked. false: another manufacturer's, or an opcode not covered */
bool p25t_osp_parse(const uint8_t tsbk[P25T_TSBK_BYTES], p25t_osp *m);
bool p25t_isp_parse(const uint8_t tsbk[P25T_TSBK_BYTES], p25t_isp *m);

/* A 16-bit channel field (band identifier, channel number) to frequencies and slot. false: no such band yet */
bool p25t_channel_freq(const p25t_iden bands[16], uint16_t channel, uint32_t *rx_hz, uint32_t *tx_hz, uint8_t *slot,
                       bool *tdma);

#ifdef __cplusplus
}
#endif
#endif
