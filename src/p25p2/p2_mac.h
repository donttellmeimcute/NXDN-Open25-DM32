/* SPDX-License-Identifier: GPL-3.0-or-later */
/* P25 Phase 2 MAC PDUs on a voice channel's FACCH (18 octets) and SACCH (21
 * octets), as OP25's p25p2_tdma.cc and dsd-fme's p25p2_xcch.c / p25p2_vpdu.c
 * read them. Octet 0: opcode (bits 7-5), offset (4-2).
 *   MAC_PTT      1-9 message indicator, 10 algorithm ID, 11-12 key ID,
 *                13-15 source, 16-17 group
 *   MAC_END_PTT  1-2 colour code (the NAC, 12 bits), 13-15 source, 16-17 group
 *   MAC_IDLE, MAC_ACTIVE, MAC_HANGTIME, MAC_SIGNAL: messages from octet 1;
 *                here the one a voice channel needs, Group Voice Channel User
 *                (abbreviated, opcode 0x01: service options, group, source),
 *                and Null fill.
 * The offset of MAC_PTT and MAC_ACTIVE counts the bursts to the first 4V. */
#ifndef P25P2_P2_MAC_H
#define P25P2_P2_MAC_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { P2_MAC_SIGNAL = 0, P2_MAC_PTT = 1, P2_MAC_END_PTT = 2, P2_MAC_IDLE = 3, P2_MAC_ACTIVE = 4, P2_MAC_HANGTIME = 6 };
enum { P2_MCO_NULL = 0x00, P2_MCO_GRP_V_CH_USR = 0x01 };
#define P2_ALGID_CLEAR 0x80u
#define P2_SVC_ENCRYPTED 0x40u

typedef struct {
    uint8_t opcode, offset;
    /* MAC_PTT */
    uint8_t mi[9];
    uint8_t algid;
    uint16_t keyid;
    /* MAC_PTT, MAC_END_PTT */
    uint32_t source;
    uint16_t group;
    uint16_t nac;                   /* MAC_END_PTT's colour code */
    /* Group Voice Channel User in MAC_IDLE / ACTIVE / HANGTIME */
    bool user;
    uint8_t svc;
    uint16_t user_group;
    uint32_t user_source;
} p2_mac;

/* Into bytes (P2_FACCH_BYTES or P2_SACCH_BYTES); false for an opcode not covered */
bool p2_mac_build(const p2_mac *m, uint8_t *pdu, unsigned bytes);
bool p2_mac_parse(const uint8_t *pdu, unsigned bytes, p2_mac *m);

#ifdef __cplusplus
}
#endif
#endif
