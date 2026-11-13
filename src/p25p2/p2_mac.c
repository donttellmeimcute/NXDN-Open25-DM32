/* SPDX-License-Identifier: GPL-3.0-or-later */
/* p2_mac.h: Phase 2 voice channel MAC PDUs. */
#include "p2_mac.h"
#include <string.h>

bool p2_mac_build(const p2_mac *m, uint8_t *p, unsigned bytes)
{
    if (bytes < 18) return false;
    memset(p, 0, bytes);
    p[0] = (uint8_t)((m->opcode & 7u) << 5 | (m->offset & 7u) << 2);
    switch (m->opcode) {
    case P2_MAC_PTT:
        memcpy(p + 1, m->mi, 9);
        p[10] = m->algid;
        p[11] = (uint8_t)(m->keyid >> 8);
        p[12] = (uint8_t)m->keyid;
        p[13] = (uint8_t)(m->source >> 16);
        p[14] = (uint8_t)(m->source >> 8);
        p[15] = (uint8_t)m->source;
        p[16] = (uint8_t)(m->group >> 8);
        p[17] = (uint8_t)m->group;
        return true;
    case P2_MAC_END_PTT:
        p[1] = (uint8_t)((m->nac >> 8) & 0x0Fu);
        p[2] = (uint8_t)m->nac;
        p[13] = (uint8_t)(m->source >> 16);
        p[14] = (uint8_t)(m->source >> 8);
        p[15] = (uint8_t)m->source;
        p[16] = (uint8_t)(m->group >> 8);
        p[17] = (uint8_t)m->group;
        return true;
    case P2_MAC_IDLE:
    case P2_MAC_ACTIVE:
    case P2_MAC_HANGTIME:
        if (m->user) {
            p[1] = P2_MCO_GRP_V_CH_USR;
            p[2] = m->svc;
            p[3] = (uint8_t)(m->user_group >> 8);
            p[4] = (uint8_t)m->user_group;
            p[5] = (uint8_t)(m->user_source >> 16);
            p[6] = (uint8_t)(m->user_source >> 8);
            p[7] = (uint8_t)m->user_source;
        }
        /* the rest is Null information (0x00) */
        return true;
    default:
        return false;
    }
}

bool p2_mac_parse(const uint8_t *p, unsigned bytes, p2_mac *m)
{
    memset(m, 0, sizeof *m);
    if (bytes < 18) return false;
    m->opcode = (uint8_t)(p[0] >> 5);
    m->offset = (uint8_t)((p[0] >> 2) & 7u);
    switch (m->opcode) {
    case P2_MAC_PTT:
        memcpy(m->mi, p + 1, 9);
        m->algid = p[10];
        m->keyid = (uint16_t)(p[11] << 8 | p[12]);
        m->source = (uint32_t)p[13] << 16 | (uint32_t)p[14] << 8 | p[15];
        m->group = (uint16_t)(p[16] << 8 | p[17]);
        return true;
    case P2_MAC_END_PTT:
        m->nac = (uint16_t)((p[1] & 0x0Fu) << 8 | p[2]);
        m->source = (uint32_t)p[13] << 16 | (uint32_t)p[14] << 8 | p[15];
        m->group = (uint16_t)(p[16] << 8 | p[17]);
        return true;
    case P2_MAC_IDLE:
    case P2_MAC_ACTIVE:
    case P2_MAC_HANGTIME:
    case P2_MAC_SIGNAL:
        /* messages: only the first is looked at (a voice channel user, when it is one) */
        if (p[1] == P2_MCO_GRP_V_CH_USR) {
            m->user = true;
            m->svc = p[2];
            m->user_group = (uint16_t)(p[3] << 8 | p[4]);
            m->user_source = (uint32_t)p[5] << 16 | (uint32_t)p[6] << 8 | p[7];
        }
        return true;
    default:
        return false;
    }
}
