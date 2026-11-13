/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tmsg.h"
#include <string.h>

/* Field `width` bits from bit `at` (bit 0 = octet 0 bit 7) */
static uint32_t get(const uint8_t *t, unsigned at, unsigned width)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < width; i++) v = v << 1 | ((t[(at + i) >> 3] >> (7 - ((at + i) & 7))) & 1u);
    return v;
}

static void put(uint8_t *t, unsigned at, unsigned width, uint32_t v)
{
    for (unsigned i = 0; i < width; i++) {
        unsigned b = at + i, bit = (v >> (width - 1 - i)) & 1u;
        uint8_t m = (uint8_t)(0x80u >> (b & 7));
        t[b >> 3] = (uint8_t)(bit ? t[b >> 3] | m : t[b >> 3] & ~m);
    }
}

static void head(uint8_t *t, uint8_t opcode, bool protect)
{
    memset(t, 0, P25T_TSBK_BYTES);
    t[0] = (uint8_t)((protect ? 0x40u : 0) | (opcode & 0x3fu));
    t[1] = 0;   /* standard */
}

/* IDEN_UP_TDMA's channel types: FDMA 12.5 (half rate), FDMA 12.5, FDMA 6.25, TDMA 2 slots, TDMA 4 slots, H-D8PSK 2 */
static const struct { uint8_t slots; uint32_t bw; bool tdma; } chan_type[6] = {
    { 1, 12500, false }, { 1, 12500, false }, { 1, 6250, false }, { 2, 12500, true }, { 4, 25000, true }, { 2, 12500, true },
};

bool p25t_osp_build(const p25t_osp *m, uint8_t *t)
{
    head(t, m->opcode, m->protect);
    switch (m->opcode) {
    case P25T_OSP_GRP_V_CH_GRANT:
        put(t, 16, 8, m->u.grant.svc);
        put(t, 24, 16, m->u.grant.channel);
        put(t, 40, 16, m->u.grant.group);
        put(t, 56, 24, m->u.grant.source);
        break;
    case P25T_OSP_GRP_V_CH_GRANT_UPDT:
        put(t, 16, 16, m->u.update.channel[0]);
        put(t, 32, 16, m->u.update.group[0]);
        put(t, 48, 16, m->u.update.channel[1]);
        put(t, 64, 16, m->u.update.group[1]);
        break;
    case P25T_OSP_U_REG_RSP:
        put(t, 18, 2, m->u.reg.response);
        put(t, 20, 12, m->u.reg.sysid);
        put(t, 32, 24, m->u.reg.source_id);
        put(t, 56, 24, m->u.reg.source_address);
        break;
    case P25T_OSP_GRP_AFF_RSP:
        put(t, 16, 1, m->u.aff.global);
        put(t, 22, 2, m->u.aff.response);
        put(t, 24, 16, m->u.aff.announce_group);
        put(t, 40, 16, m->u.aff.group);
        put(t, 56, 24, m->u.aff.target);
        break;
    case P25T_OSP_DENY_RSP:
    case P25T_OSP_QUE_RSP:
        put(t, 16, 1, m->u.deny.aiv);
        put(t, 18, 6, m->u.deny.service);
        put(t, 24, 8, m->u.deny.reason);
        put(t, 32, 24, m->u.deny.info);
        put(t, 56, 24, m->u.deny.target);
        break;
    case P25T_OSP_ACK_RSP_FNE:
        put(t, 16, 1, m->u.ack.aiv);
        put(t, 17, 1, m->u.ack.ex);
        put(t, 18, 6, m->u.ack.service);
        if (m->u.ack.ex) {
            put(t, 24, 20, m->u.ack.wacn);
            put(t, 44, 12, m->u.ack.sysid);
        } else {
            put(t, 32, 24, m->u.ack.source);
        }
        put(t, 56, 24, m->u.ack.target);
        break;
    case P25T_OSP_RFSS_STS_BCST:
        put(t, 16, 8, m->u.rfss.lra);
        put(t, 27, 1, m->u.rfss.active);
        put(t, 28, 12, m->u.rfss.sysid);
        put(t, 40, 8, m->u.rfss.rfss);
        put(t, 48, 8, m->u.rfss.site);
        put(t, 56, 16, m->u.rfss.channel);
        put(t, 72, 8, m->u.rfss.ssc);
        break;
    case P25T_OSP_NET_STS_BCST:
        put(t, 16, 8, m->u.net.lra);
        put(t, 24, 20, m->u.net.wacn);
        put(t, 44, 12, m->u.net.sysid);
        put(t, 56, 16, m->u.net.channel);
        put(t, 72, 8, m->u.net.ssc);
        break;
    case P25T_OSP_U_DE_REG_ACK:
        put(t, 24, 20, m->u.dereg.wacn);
        put(t, 44, 12, m->u.dereg.sysid);
        put(t, 56, 24, m->u.dereg.target);
        break;
    case P25T_OSP_IDEN_UP:
    case P25T_OSP_IDEN_UP_VU:
    case P25T_OSP_IDEN_UP_TDMA: {
        const p25t_iden *b = &m->u.iden.band;
        uint32_t spacing = b->spacing_hz / 125u;
        put(t, 16, 4, m->u.iden.id);
        put(t, 38, 10, spacing);
        put(t, 48, 32, b->base_hz / 5u);
        uint32_t off = (uint32_t)(b->tx_offset_hz < 0 ? -b->tx_offset_hz : b->tx_offset_hz);
        if (m->opcode == P25T_OSP_IDEN_UP) {
            put(t, 20, 9, b->bandwidth_hz / 125u);
            put(t, 29, 1, b->tx_offset_hz >= 0);
            put(t, 30, 8, b->tx_ok ? off / 250000u : 0x80u);
        } else {
            if (m->opcode == P25T_OSP_IDEN_UP_VU) put(t, 20, 4, b->bandwidth_hz == 6250 ? 4u : 5u);
            else put(t, 20, 4, b->channel_type);
            put(t, 24, 1, b->tx_offset_hz >= 0);
            put(t, 25, 13, spacing ? off / (spacing * 125u) : 0);
        }
        break;
    }
    default:
        return false;
    }
    p25t_tsbk_seal(t);
    return true;
}

bool p25t_osp_parse(const uint8_t *t, p25t_osp *m)
{
    memset(m, 0, sizeof *m);
    m->opcode = t[0] & 0x3fu;
    m->protect = (t[0] & 0x40u) != 0;
    m->mfid = t[1];
    if (m->mfid != 0) return false;
    switch (m->opcode) {
    case P25T_OSP_GRP_V_CH_GRANT:
        m->u.grant.svc = (uint8_t)get(t, 16, 8);
        m->u.grant.channel = (uint16_t)get(t, 24, 16);
        m->u.grant.group = (uint16_t)get(t, 40, 16);
        m->u.grant.source = get(t, 56, 24);
        return true;
    case P25T_OSP_GRP_V_CH_GRANT_UPDT:
        m->u.update.channel[0] = (uint16_t)get(t, 16, 16);
        m->u.update.group[0] = (uint16_t)get(t, 32, 16);
        m->u.update.channel[1] = (uint16_t)get(t, 48, 16);
        m->u.update.group[1] = (uint16_t)get(t, 64, 16);
        return true;
    case P25T_OSP_U_REG_RSP:
        m->u.reg.response = (uint8_t)get(t, 18, 2);
        m->u.reg.sysid = (uint16_t)get(t, 20, 12);
        m->u.reg.source_id = get(t, 32, 24);
        m->u.reg.source_address = get(t, 56, 24);
        return true;
    case P25T_OSP_GRP_AFF_RSP:
        m->u.aff.global = get(t, 16, 1);
        m->u.aff.response = (uint8_t)get(t, 22, 2);
        m->u.aff.announce_group = (uint16_t)get(t, 24, 16);
        m->u.aff.group = (uint16_t)get(t, 40, 16);
        m->u.aff.target = get(t, 56, 24);
        return true;
    case P25T_OSP_DENY_RSP:
    case P25T_OSP_QUE_RSP:
        m->u.deny.aiv = get(t, 16, 1);
        m->u.deny.service = (uint8_t)get(t, 18, 6);
        m->u.deny.reason = (uint8_t)get(t, 24, 8);
        m->u.deny.info = get(t, 32, 24);
        m->u.deny.target = get(t, 56, 24);
        return true;
    case P25T_OSP_ACK_RSP_FNE:
        m->u.ack.aiv = get(t, 16, 1);
        m->u.ack.ex = get(t, 17, 1);
        m->u.ack.service = (uint8_t)get(t, 18, 6);
        if (m->u.ack.ex) {
            m->u.ack.wacn = get(t, 24, 20);
            m->u.ack.sysid = (uint16_t)get(t, 44, 12);
        } else {
            m->u.ack.source = get(t, 32, 24);
        }
        m->u.ack.target = get(t, 56, 24);
        return true;
    case P25T_OSP_RFSS_STS_BCST:
        m->u.rfss.lra = (uint8_t)get(t, 16, 8);
        m->u.rfss.active = get(t, 27, 1);
        m->u.rfss.sysid = (uint16_t)get(t, 28, 12);
        m->u.rfss.rfss = (uint8_t)get(t, 40, 8);
        m->u.rfss.site = (uint8_t)get(t, 48, 8);
        m->u.rfss.channel = (uint16_t)get(t, 56, 16);
        m->u.rfss.ssc = (uint8_t)get(t, 72, 8);
        return true;
    case P25T_OSP_NET_STS_BCST:
        m->u.net.lra = (uint8_t)get(t, 16, 8);
        m->u.net.wacn = get(t, 24, 20);
        m->u.net.sysid = (uint16_t)get(t, 44, 12);
        m->u.net.channel = (uint16_t)get(t, 56, 16);
        m->u.net.ssc = (uint8_t)get(t, 72, 8);
        return true;
    case P25T_OSP_U_DE_REG_ACK:
        m->u.dereg.wacn = get(t, 24, 20);
        m->u.dereg.sysid = (uint16_t)get(t, 44, 12);
        m->u.dereg.target = get(t, 56, 24);
        return true;
    case P25T_OSP_IDEN_UP:
    case P25T_OSP_IDEN_UP_VU:
    case P25T_OSP_IDEN_UP_TDMA: {
        p25t_iden *b = &m->u.iden.band;
        m->u.iden.id = (uint8_t)get(t, 16, 4);
        b->valid = true;
        b->spacing_hz = get(t, 38, 10) * 125u;
        b->base_hz = get(t, 48, 32) * 5u;
        b->slots = 1;
        if (m->opcode == P25T_OSP_IDEN_UP) {
            uint32_t off = get(t, 30, 8);
            b->bandwidth_hz = get(t, 20, 9) * 125u;
            b->tx_ok = off != 0x80u;
            b->tx_offset_hz = (int32_t)(off * 250000u) * (get(t, 29, 1) ? 1 : -1);
        } else {
            uint32_t off = get(t, 25, 13);
            b->tx_ok = off != 0;
            b->tx_offset_hz = (int32_t)(off * b->spacing_hz) * (get(t, 24, 1) ? 1 : -1);
            if (m->opcode == P25T_OSP_IDEN_UP_VU) {
                uint32_t bw = get(t, 20, 4);
                b->bandwidth_hz = bw == 4 ? 6250u : bw == 5 ? 12500u : 0;
            } else {
                b->channel_type = (uint8_t)get(t, 20, 4);
                if (b->channel_type < 6) {
                    b->slots = chan_type[b->channel_type].slots;
                    b->bandwidth_hz = chan_type[b->channel_type].bw;
                    b->tdma = chan_type[b->channel_type].tdma;
                } else {
                    b->valid = false;
                }
            }
        }
        return true;
    }
    default:
        return false;
    }
}

bool p25t_isp_build(const p25t_isp *m, uint8_t *t)
{
    head(t, m->opcode, false);
    switch (m->opcode) {
    case P25T_ISP_GRP_V_REQ:
        put(t, 16, 8, m->u.voice.svc);
        put(t, 40, 16, m->u.voice.group);
        put(t, 56, 24, m->u.voice.source);
        break;
    case P25T_ISP_GRP_AFF_REQ:
        put(t, 28, 12, m->u.aff.sysid);
        put(t, 40, 16, m->u.aff.group);
        put(t, 56, 24, m->u.aff.source);
        break;
    case P25T_ISP_U_REG_REQ:
        put(t, 16, 1, m->u.reg.emergency);
        put(t, 17, 7, m->u.reg.capability);
        put(t, 24, 20, m->u.reg.wacn);
        put(t, 44, 12, m->u.reg.sysid);
        put(t, 56, 24, m->u.reg.source);
        break;
    case P25T_ISP_U_DE_REG_REQ:
        put(t, 24, 20, m->u.dereg.wacn);
        put(t, 44, 12, m->u.dereg.sysid);
        put(t, 56, 24, m->u.dereg.source);
        break;
    default:
        return false;
    }
    p25t_tsbk_seal(t);
    return true;
}

bool p25t_isp_parse(const uint8_t *t, p25t_isp *m)
{
    memset(m, 0, sizeof *m);
    m->opcode = t[0] & 0x3fu;
    if (t[1] != 0) return false;
    switch (m->opcode) {
    case P25T_ISP_GRP_V_REQ:
        m->u.voice.svc = (uint8_t)get(t, 16, 8);
        m->u.voice.group = (uint16_t)get(t, 40, 16);
        m->u.voice.source = get(t, 56, 24);
        return true;
    case P25T_ISP_GRP_AFF_REQ:
        m->u.aff.sysid = (uint16_t)get(t, 28, 12);
        m->u.aff.group = (uint16_t)get(t, 40, 16);
        m->u.aff.source = get(t, 56, 24);
        return true;
    case P25T_ISP_U_REG_REQ:
        m->u.reg.emergency = get(t, 16, 1);
        m->u.reg.capability = (uint8_t)get(t, 17, 7);
        m->u.reg.wacn = get(t, 24, 20);
        m->u.reg.sysid = (uint16_t)get(t, 44, 12);
        m->u.reg.source = get(t, 56, 24);
        return true;
    case P25T_ISP_U_DE_REG_REQ:
        m->u.dereg.wacn = get(t, 24, 20);
        m->u.dereg.sysid = (uint16_t)get(t, 44, 12);
        m->u.dereg.source = get(t, 56, 24);
        return true;
    default:
        return false;
    }
}

bool p25t_channel_freq(const p25t_iden bands[16], uint16_t channel, uint32_t *rx_hz, uint32_t *tx_hz, uint8_t *slot,
                       bool *tdma)
{
    const p25t_iden *b = &bands[channel >> 12];
    if (!b->valid || !b->spacing_hz) return false;
    unsigned n = channel & 0xfffu, slots = b->slots ? b->slots : 1;
    uint32_t down = b->base_hz + b->spacing_hz * (n / slots);
    if (rx_hz) *rx_hz = down;
    if (tx_hz) *tx_hz = b->tx_ok ? (uint32_t)((int64_t)down + b->tx_offset_hz) : 0;
    if (slot) *slot = (uint8_t)(n % slots);
    if (tdma) *tdma = b->tdma;
    return true;
}
