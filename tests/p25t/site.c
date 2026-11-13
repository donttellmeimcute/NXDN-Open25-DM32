/* SPDX-License-Identifier: GPL-3.0-or-later */
/* site.h: a simulated P25 trunked site for host tests. */
#include "site.h"
#include <string.h>

enum { V_FREE, V_TALKER, V_UNIT, V_HANG };
enum { HANG_MS = 500, UNIT_LATE_MS = 2000, GRANT_REPEATS = 3, GRANT_GAP_MS = 75 };

static uint32_t srnd(site *s)
{
    s->rnd ^= s->rnd << 13;
    s->rnd ^= s->rnd >> 17;
    s->rnd ^= s->rnd << 5;
    return s->rnd;
}

static uint32_t ms_ticks(uint32_t ms)
{
    return ms * (SITE_TICK_HZ / 1000);
}

uint32_t site_ms(const site *s)
{
    return s->tick / (SITE_TICK_HZ / 1000);
}

static int find_unit(const site *s, uint32_t unit)
{
    for (int i = 0; i < SITE_UNITS; i++)
        if (s->registered[i] == unit) return i;
    return -1;
}

bool site_is_registered(const site *s, uint32_t unit)
{
    return unit && find_unit(s, unit) >= 0;
}

uint16_t site_affiliation(const site *s, uint32_t unit)
{
    int i = unit ? find_unit(s, unit) : -1;
    return i < 0 ? 0 : s->affiliated[i];
}

static bool allowed(const site *s, uint32_t unit)
{
    for (int i = 0; i < SITE_UNITS && s->cfg.allowed[i]; i++)
        if (s->cfg.allowed[i] == unit) return true;
    return false;
}

static void send_after(site *s, const p25t_osp *m, uint32_t ms)
{
    if (s->nq >= SITE_QUEUE) return;
    p25t_osp_build(m, s->q[s->nq].tsbk);
    s->q[s->nq].ready = s->tick + ms_ticks(ms);
    s->nq++;
}

static void deny(site *s, uint8_t service, uint8_t reason, uint32_t target)
{
    p25t_osp d = { .opcode = P25T_OSP_DENY_RSP };
    d.u.deny.service = service;
    d.u.deny.reason = reason;
    d.u.deny.target = target;
    send_after(s, &d, SITE_PROCESS_MS);
}

/* ---- voice channels ---- */

static void unpack(const uint8_t *bytes, unsigned dibits, uint8_t *out)
{
    for (unsigned i = 0; i < dibits; i++) out[i] = (uint8_t)((bytes[i >> 2] >> (6 - 2 * (i & 3))) & 3u);
}

static bool start_call(site *s, uint16_t group, uint32_t source, bool unit, uint32_t ms)
{
    site_voice *v = NULL;
    for (int i = 0; i < SITE_VOICE; i++)
        if (s->v[i].state == V_FREE) {
            v = &s->v[i];
            break;
        }
    if (!v || s->n_calls >= SITE_LOG) return false;
    uint16_t channel = v->channel;
    uint32_t down = 0, up = 0;
    uint8_t slot = 0;
    bool tdma = false;
    p25t_channel_freq(s->bands, channel, &down, &up, &slot, &tdma);
    v->state = unit ? V_UNIT : V_TALKER;
    v->group = group;
    v->source = source;
    v->down_hz = down;
    v->up_hz = up;
    v->slot = slot;
    v->tdma = tdma;
    v->end_tick = unit ? UINT32_MAX : s->tick + ms_ticks(ms);   /* a unit's call ends on its terminator */
    v->last_up_tick = s->tick;
    v->pos = v->frame_dibits = 0;
    v->seq = 0;
    v->rq_head = v->rq_count = 0;
    v->sym = -1;
    p25_rx_init(&v->up, s->cfg.nac, 2);
    v->log = s->n_calls++;
    site_call_log *c = &s->calls[v->log];
    memset(c, 0, sizeof *c);
    c->group = group;
    c->source = source;
    c->channel = channel;
    c->unit = unit;
    p25t_osp g = { .opcode = P25T_OSP_GRP_V_CH_GRANT };
    g.u.grant.svc = 0x04;
    g.u.grant.channel = channel;
    g.u.grant.group = group;
    g.u.grant.source = source;
    for (int k = 0; k < GRANT_REPEATS; k++) send_after(s, &g, SITE_PROCESS_MS + (uint32_t)k * GRANT_GAP_MS);
    return true;
}

bool site_talk(site *s, uint16_t group, uint32_t source, uint32_t ms)
{
    return start_call(s, group, source, false, ms);
}

static void talker_frame(site *s, site_voice *v)
{
    p25_call call = { .nac = s->cfg.nac, .talkgroup = v->group, .source_id = v->source, .status_symbol = 1 };
    static const uint16_t silence[P25_IMBE_FRAMES][8];
    static const uint8_t lsd[2];
    uint8_t buf[P25_LDU_BYTES];
    unsigned bytes;
    if (s->tick >= v->end_tick) {
        if (v->seq >= 1000) {           /* both terminators went out */
            v->state = V_HANG;
            v->end_tick = s->tick + ms_ticks(HANG_MS);
            v->frame_dibits = 0;
            return;
        }
        p25_encode_tdu(&call, buf);
        bytes = P25_TDU_BYTES;
        v->seq = v->seq >= 999 ? 1000 : 999;
    } else if (v->seq == 0) {
        p25_encode_hdu(&call, buf);
        bytes = P25_HDU_BYTES;
        v->seq = 1;
    } else {
        p25_encode_ldu(&call, (v->seq & 1) ? P25_DUID_LDU1 : P25_DUID_LDU2, silence, lsd, buf);
        bytes = P25_LDU_BYTES;
        v->seq++;
    }
    unpack(buf, bytes * 4, v->frame);
    v->frame_dibits = (uint16_t)(bytes * 4);
    v->pos = 0;
    s->calls[v->log].sent++;
}

static void repeat_frame(site *s, site_voice *v)
{
    if (!v->rq_count) {
        v->frame_dibits = 0;
        return;
    }
    uint8_t *f = v->repeat[v->rq_head];
    unsigned bytes = v->repeat_bytes[v->rq_head];
    v->rq_head = (uint8_t)((v->rq_head + 1) % 4);
    v->rq_count--;
    unpack(f, bytes * 4, v->frame);
    v->frame_dibits = (uint16_t)(bytes * 4);
    v->pos = 0;
    s->calls[v->log].sent++;
}

static void voice_down(site *s, site_voice *v)
{
    v->sym = -1;
    if (v->state == V_HANG && s->tick >= v->end_tick) v->state = V_FREE;
    if (v->state != V_TALKER && v->state != V_UNIT) return;
    if (v->pos >= v->frame_dibits) {
        if (v->state == V_TALKER) talker_frame(s, v);
        else repeat_frame(s, v);
    }
    if (v->pos < v->frame_dibits) v->sym = v->frame[v->pos++];
}

static void voice_up(site *s, site_voice *v)
{
    if (v->state != V_UNIT) {
        v->up_count = 0;
        return;
    }
    bool carrier = v->up_count > 0;
    uint8_t d = v->up_count == 1 ? v->up_dibit : (uint8_t)(srnd(s) & 3u);
    v->up_count = 0;
    if (carrier) v->last_up_tick = s->tick;
    if (p25_rx_push_dibit(&v->up, d) == 1) {
        site_call_log *c = &s->calls[v->log];
        unsigned bytes = (unsigned)p25_frame_bytes(v->up.nid.duid);
        p25_decoded r;
        if (p25_decode_frame(v->up.frame, bytes, &r) == 0) {
            switch (r.nid.duid) {
            case P25_DUID_HDU: c->hdus++; break;
            case P25_DUID_LDU1:
                c->ldu1s++;
                if (r.metadata_valid) {
                    c->lc_group = r.talkgroup;
                    c->lc_source = r.source_id;
                }
                break;
            case P25_DUID_LDU2: c->ldu2s++; break;
            default: c->tdus++; break;
            }
        }
        if (v->rq_count < 4) {
            unsigned at = (v->rq_head + v->rq_count) % 4u;
            memcpy(v->repeat[at], v->up.frame, bytes);
            v->repeat_bytes[at] = (uint16_t)bytes;
            v->rq_count++;
        }
        if (v->up.nid.duid == P25_DUID_TDU || v->up.nid.duid == P25_DUID_TDULC)
            v->end_tick = s->tick;   /* the call ends once the terminator is repeated */
    }
}

/* ---- control channel ---- */

static bool active_call(const site *s, unsigned i)
{
    return s->v[i].state == V_TALKER || s->v[i].state == V_UNIT;
}

static void broadcast(site *s, uint8_t t[P25T_TSBK_BYTES])
{
    for (;;) {
        unsigned k = s->rotation++ % 6;
        p25t_osp m = { 0 };
        switch (k) {
        case 0:
            m.opcode = P25T_OSP_NET_STS_BCST;
            m.u.net.lra = 1;
            m.u.net.wacn = s->cfg.wacn;
            m.u.net.sysid = s->cfg.sysid;
            m.u.net.channel = SITE_CC_CHANNEL;
            m.u.net.ssc = 0x70;
            break;
        case 1:
            m.opcode = P25T_OSP_RFSS_STS_BCST;
            m.u.rfss.lra = 1;
            m.u.rfss.active = true;
            m.u.rfss.sysid = s->cfg.sysid;
            m.u.rfss.rfss = s->cfg.rfss;
            m.u.rfss.site = s->cfg.site;
            m.u.rfss.channel = SITE_CC_CHANNEL;
            m.u.rfss.ssc = 0x70;
            break;
        case 2:
            m.opcode = P25T_OSP_IDEN_UP_VU;
            m.u.iden.id = 1;
            m.u.iden.band = s->bands[1];
            break;
        case 3:
            if (!s->cfg.tdma) continue;
            m.opcode = P25T_OSP_IDEN_UP_TDMA;
            m.u.iden.id = 2;
            m.u.iden.band = s->bands[2];
            break;
        default: {
            /* grant updates for the calls in progress, two a packet */
            unsigned n = 0;
            m.opcode = P25T_OSP_GRP_V_CH_GRANT_UPDT;
            for (unsigned i = (k - 4) * 2; i < SITE_VOICE && n < 2; i++)
                if (active_call(s, i)) {
                    m.u.update.channel[n] = s->v[i].channel;
                    m.u.update.group[n] = s->v[i].group;
                    n++;
                }
            if (!n) continue;
            if (n == 1) {
                m.u.update.channel[1] = m.u.update.channel[0];
                m.u.update.group[1] = m.u.update.group[0];
            }
            break;
        }
        }
        p25t_osp_build(&m, t);
        return;
    }
}

static void next_tsdu(site *s)
{
    uint8_t t[3][P25T_TSBK_BYTES], frame[P25T_TSDU_MAX_BYTES];
    unsigned n = 0;
    for (unsigned i = 0; i < s->nq && n < 3;) {
        if ((int32_t)(s->tick - s->q[i].ready) >= 0) {
            memcpy(t[n++], s->q[i].tsbk, P25T_TSBK_BYTES);
            memmove(&s->q[i], &s->q[i + 1], (s->nq - i - 1) * sizeof s->q[0]);
            s->nq--;
        } else {
            i++;
        }
    }
    while (n < 3) broadcast(s, t[n++]);
    unsigned bits = p25t_tsdu_encode(s->cfg.nac, t, 3, P25T_STATUS_IDLE, frame);
    unpack(frame, bits / 2, s->cc);
    s->cc_pos = 0;
}

static void on_isp(void *ctx, uint16_t nac, const uint8_t tsbk[P25T_TSBK_BYTES])
{
    site *s = ctx;
    (void)nac;
    p25t_isp m;
    if (!p25t_isp_parse(tsbk, &m)) return;
    if (s->n_isps < SITE_LOG) {
        site_isp_log *l = &s->isps[s->n_isps++];
        memset(l, 0, sizeof *l);
        l->opcode = m.opcode;
        l->tick = s->tick;
        switch (m.opcode) {
        case P25T_ISP_GRP_V_REQ: l->source = m.u.voice.source; l->group = m.u.voice.group; break;
        case P25T_ISP_GRP_AFF_REQ: l->source = m.u.aff.source; l->group = m.u.aff.group; l->sysid = m.u.aff.sysid; break;
        case P25T_ISP_U_REG_REQ: l->source = m.u.reg.source; l->wacn = m.u.reg.wacn; l->sysid = m.u.reg.sysid; break;
        case P25T_ISP_U_DE_REG_REQ: l->source = m.u.dereg.source; l->wacn = m.u.dereg.wacn; l->sysid = m.u.dereg.sysid; break;
        default: break;
        }
    }
    if (s->cfg.silent) return;
    switch (m.opcode) {
    case P25T_ISP_U_REG_REQ: {
        uint32_t u = m.u.reg.source;
        bool ok = m.u.reg.wacn == s->cfg.wacn && m.u.reg.sysid == s->cfg.sysid && allowed(s, u);
        if (ok && find_unit(s, u) < 0) {
            int i = find_unit(s, 0);
            if (i >= 0) {
                s->registered[i] = u;
                s->affiliated[i] = 0;
            } else {
                ok = false;
            }
        }
        p25t_osp r = { .opcode = P25T_OSP_U_REG_RSP };
        r.u.reg.response = ok ? P25T_RSP_ACCEPT : P25T_RSP_REFUSED;
        r.u.reg.sysid = s->cfg.sysid;
        r.u.reg.source_id = u;
        r.u.reg.source_address = u;
        send_after(s, &r, SITE_PROCESS_MS);
        break;
    }
    case P25T_ISP_GRP_AFF_REQ: {
        int i = find_unit(s, m.u.aff.source);
        if (i < 0) {
            deny(s, P25T_ISP_GRP_AFF_REQ, 0x10, m.u.aff.source);   /* requesting unit not valid */
            break;
        }
        s->affiliated[i] = m.u.aff.group;
        p25t_osp r = { .opcode = P25T_OSP_GRP_AFF_RSP };
        r.u.aff.response = P25T_RSP_ACCEPT;
        r.u.aff.announce_group = m.u.aff.group;
        r.u.aff.group = m.u.aff.group;
        r.u.aff.target = m.u.aff.source;
        send_after(s, &r, SITE_PROCESS_MS);
        break;
    }
    case P25T_ISP_U_DE_REG_REQ: {
        int i = find_unit(s, m.u.dereg.source);
        if (i >= 0) s->registered[i] = 0, s->affiliated[i] = 0;
        p25t_osp r = { .opcode = P25T_OSP_U_DE_REG_ACK };
        r.u.dereg.wacn = s->cfg.wacn;
        r.u.dereg.sysid = s->cfg.sysid;
        r.u.dereg.target = m.u.dereg.source;
        send_after(s, &r, SITE_PROCESS_MS);
        break;
    }
    case P25T_ISP_GRP_V_REQ: {
        uint32_t u = m.u.voice.source;
        uint16_t g = m.u.voice.group;
        if (find_unit(s, u) < 0) {
            deny(s, P25T_ISP_GRP_V_REQ, 0x10, u);
            break;
        }
        if (g == s->cfg.deny_group) {
            deny(s, P25T_ISP_GRP_V_REQ, 0x31, u);   /* target group not authorized for service */
            break;
        }
        uint32_t later = 0;
        if (s->cfg.ack_ms) {
            p25t_osp a = { .opcode = P25T_OSP_ACK_RSP_FNE };
            a.u.ack.service = P25T_ISP_GRP_V_REQ;
            a.u.ack.target = u;
            send_after(s, &a, SITE_PROCESS_MS);
            later = s->cfg.ack_ms;
        }
        if (g == s->cfg.queue_group) {
            p25t_osp q = { .opcode = P25T_OSP_QUE_RSP };
            q.u.deny.service = P25T_ISP_GRP_V_REQ;
            q.u.deny.reason = 0x40;                 /* channel resources unavailable */
            q.u.deny.target = u;
            send_after(s, &q, SITE_PROCESS_MS + later);
            later += s->cfg.queue_ms;
        }
        if (!later) {
            if (!start_call(s, g, u, true, 0)) deny(s, P25T_ISP_GRP_V_REQ, 0xFF, u);
            break;
        }
        for (int i = 0; i < SITE_UNITS; i++)
            if (!s->later[i].active) {
                s->later[i].active = true;
                s->later[i].group = g;
                s->later[i].source = u;
                s->later[i].tick = s->tick + ms_ticks(later);
                break;
            }
        break;
    }
    default:
        break;
    }
}

void site_init(site *s, const site_cfg *cfg)
{
    memset(s, 0, sizeof *s);
    s->cfg = *cfg;
    s->rnd = cfg->seed ? cfg->seed : 0x9E3779B9u;
    s->bands[1] = (p25t_iden){ .valid = true, .slots = 1, .bandwidth_hz = 12500, .spacing_hz = SITE_SPACING_HZ,
                               .base_hz = SITE_FDMA_BASE_HZ, .tx_offset_hz = SITE_TX_OFFSET_HZ, .tx_ok = true };
    s->bands[2] = (p25t_iden){ .valid = true, .tdma = true, .slots = 2, .channel_type = 3, .bandwidth_hz = 12500,
                               .spacing_hz = SITE_SPACING_HZ, .base_hz = SITE_TDMA_BASE_HZ,
                               .tx_offset_hz = SITE_TX_OFFSET_HZ, .tx_ok = true };
    p25t_channel_freq(s->bands, SITE_CC_CHANNEL, &s->cc_down_hz, &s->cc_up_hz, NULL, NULL);
    for (int i = 0; i < SITE_VOICE; i++) {
        s->v[i].channel = (uint16_t)(cfg->tdma ? SITE_TDMA_FIRST + 2 * i : SITE_FDMA_FIRST + i);
        s->v[i].sym = -1;
    }
    p25t_rx_init(&s->cc_up, cfg->nac, on_isp, NULL, NULL, s);
    s->cc_pos = sizeof s->cc;
    s->cc_sym = -1;
}

void site_begin(site *s)
{
    if (s->tick % SITE_FDMA_TICKS) return;
    for (int i = 0; i < SITE_UNITS; i++)
        if (s->later[i].active && (int32_t)(s->tick - s->later[i].tick) >= 0) {
            s->later[i].active = false;
            if (!start_call(s, s->later[i].group, s->later[i].source, true, 0))
                deny(s, P25T_ISP_GRP_V_REQ, 0xFF, s->later[i].source);
        }
    if (s->cfg.no_cc) {
        s->cc_sym = -1;
    } else {
        if (s->cc_pos >= sizeof s->cc) next_tsdu(s);
        uint8_t d = s->cc[s->cc_pos];
        if (s->cc_pos % 36 == 35) {
            bool busy = s->cc_up_run >= SITE_SYNC_SYMBOLS;
            d = busy ? P25T_STATUS_BUSY : P25T_STATUS_IDLE;
            busy ? s->busy_status++ : s->idle_status++;
        }
        s->cc_pos++;
        s->cc_sym = d;
    }
    for (int i = 0; i < SITE_VOICE; i++)
        if (!s->v[i].tdma) voice_down(s, &s->v[i]);
}

int site_rx(const site *s, uint32_t hz, bool *fresh)
{
    *fresh = s->tick % SITE_FDMA_TICKS == 0;
    if (hz == s->cc_down_hz) return s->cc_sym;
    for (int i = 0; i < SITE_VOICE; i++)
        if (!s->v[i].tdma && s->v[i].down_hz == hz && s->v[i].state != V_FREE) return s->v[i].sym;
    return -1;
}

void site_tx(site *s, uint32_t hz, uint8_t dibit)
{
    if (hz == s->cc_up_hz) {
        s->cc_up_count++;
        s->cc_up_dibit = dibit;
        return;
    }
    for (int i = 0; i < SITE_VOICE; i++)
        if (!s->v[i].tdma && s->v[i].up_hz == hz) {
            s->v[i].up_count++;
            s->v[i].up_dibit = dibit;
        }
}

void site_end(site *s)
{
    if (s->tick % SITE_FDMA_TICKS == 0) {
        bool carrier = s->cc_up_count > 0;
        if (s->cc_up_count > 1) s->collisions++;
        uint8_t d = s->cc_up_count == 1 ? s->cc_up_dibit : (uint8_t)(srnd(s) & 3u);
        s->cc_up_count = 0;
        s->cc_up_run = carrier ? s->cc_up_run + 1 : 0;
        p25t_rx_dibit(&s->cc_up, d);
        for (int i = 0; i < SITE_VOICE; i++) {
            site_voice *v = &s->v[i];
            if (v->tdma) continue;
            voice_up(s, v);
            if (v->state == V_UNIT) {
                bool ended = v->end_tick <= s->tick && !v->rq_count && v->pos >= v->frame_dibits &&
                             s->tick - v->last_up_tick > ms_ticks(100);
                if (ended || s->tick - v->last_up_tick >= ms_ticks(UNIT_LATE_MS)) {
                    v->state = V_HANG;
                    v->end_tick = s->tick + ms_ticks(HANG_MS);
                }
            }
        }
    }
    s->tick++;
}
