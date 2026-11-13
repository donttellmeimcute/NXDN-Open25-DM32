/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "su.h"
#include <string.h>

enum { NOT_STARTED = 0xFF };

static void clear(p25t_su_out *out)
{
    memset(out, 0, sizeof *out);
}

static uint32_t rnd(p25t_su *su)
{
    su->rnd ^= su->rnd << 13;
    su->rnd ^= su->rnd >> 17;
    su->rnd ^= su->rnd << 5;
    return su->rnd;
}

static void set_state(p25t_su *su, uint8_t s)
{
    su->state = s;
    su->state_ms = su->now;
}

static void tune_cc(p25t_su *su, p25t_su_out *out, bool hunting)
{
    su->cc_rx_hz = su->cfg.cc_hz[su->cc_index];
    out->tune = true;
    out->rx_hz = su->cc_rx_hz;
    out->slot = 0;
    out->tdma = false;
    su->last_tsbk_ms = su->now;
    if (hunting) {
        su->net_ok = su->rfss_ok = false;
        set_state(su, P25T_SU_HUNT);
    } else {
        set_state(su, P25T_SU_CC);
    }
}

static bool system_ok(const p25t_su *su)
{
    return su->net_ok && su->rfss_ok;
}

/* The talkgroups followed: 0 = ours, 1.. = the priority list, 0xFF = hear-all, -1 = not followed */
static int wanted(const p25t_su *su, uint16_t group)
{
    if (group == su->cfg.talkgroup) return 0;
    for (unsigned i = 0; i < su->cfg.priority_count; i++)
        if (su->cfg.priority[i] == group) return (int)i + 1;
    return su->cfg.hear_all ? 0xFF : -1;
}

static void queue_isp(p25t_su *su, const p25t_isp *m)
{
    p25t_isp_build(m, su->isp);
    su->isp_pending = true;
    su->awaiting = false;
    su->isp_tries = 0;
    su->backoff = P25T_SU_WINDOW_SLOTS;
    su->microslots = NOT_STARTED;
}

static void isp_done(p25t_su *su)
{
    su->isp_pending = false;
    su->awaiting = false;
}

static void cc_tx_from_site(p25t_su *su)
{
    uint32_t up = 0;
    bool tdma = false;
    if (su->site_known && p25t_channel_freq(su->bands, su->cc_channel, NULL, &up, NULL, &tdma) && !tdma) su->cc_tx_hz = up;
}

void p25t_su_init(p25t_su *su, const p25t_su_cfg *cfg, uint32_t now_ms, uint32_t seed, p25t_su_out *out)
{
    memset(su, 0, sizeof *su);
    su->cfg = *cfg;
    if (su->cfg.cc_count == 0) su->cfg.cc_count = 1;
    if (su->cfg.cc_count > P25T_SU_CC_MAX) su->cfg.cc_count = P25T_SU_CC_MAX;
    if (su->cfg.priority_count > P25T_SU_PRIORITY_MAX) su->cfg.priority_count = P25T_SU_PRIORITY_MAX;
    /* transmit needs a whole user-entered identity */
    su->cfg.tx_enabled = cfg->tx_enabled && cfg->wacn && cfg->wacn <= 0xFFFFF && cfg->sysid && cfg->sysid <= 0xFFF &&
                         cfg->unit_id && cfg->unit_id <= 0xFFFFFF && cfg->talkgroup;
    su->now = now_ms;
    su->rnd = seed ? seed : 0x2545F491u;
    su->reg_sent_ms = su->aff_sent_ms = now_ms - P25T_SU_AUTO_GAP_MS;
    clear(out);
    tune_cc(su, out, true);
}

static void follow(p25t_su *su, uint16_t channel, uint16_t group, uint32_t source, p25t_su_out *out)
{
    uint32_t rx = 0;
    uint8_t slot = 0;
    bool tdma = false;
    if (!p25t_channel_freq(su->bands, channel, &rx, NULL, &slot, &tdma)) return;
    if (tdma && !su->cfg.tdma_rx) return;
    su->call_group = group;
    su->call_source = source;
    su->call_rx_hz = rx;
    su->call_slot = slot;
    su->call_tdma = tdma;
    su->call_encrypted = false;
    su->last_voice_ms = su->now;
    su->grants_followed++;
    out->tune = true;
    out->rx_hz = rx;
    out->slot = slot;
    out->tdma = tdma;
    set_state(su, P25T_SU_VOICE_RX);
}

static void deny(p25t_su *su, uint8_t why, uint8_t reason, p25t_su_out *out)
{
    su->deny = why;
    su->deny_reason = reason;
    isp_done(su);
    if (su->state == P25T_SU_REQUEST || su->state == P25T_SU_QUEUED) set_state(su, P25T_SU_CC);
    (void)out;
}

static void own_grant(p25t_su *su, uint16_t channel, p25t_su_out *out)
{
    uint32_t rx = 0, tx = 0;
    uint8_t slot = 0;
    bool tdma = false;
    isp_done(su);
    if (!su->ptt) {
        set_state(su, P25T_SU_CC);   /* released while waiting: nothing to send */
        return;
    }
    if (!p25t_channel_freq(su->bands, channel, &rx, &tx, &slot, &tdma) || !tx || (tdma && !su->cfg.tdma_tx)) {
        /* no transmit frequency, or a Phase 2 channel and no Phase 2 transmitter */
        deny(su, P25T_SU_DENY_BAND, 0, out);
        return;
    }
    su->call_group = su->cfg.talkgroup;
    su->call_source = su->cfg.unit_id;
    su->call_rx_hz = rx;
    su->call_slot = slot;
    su->call_tdma = tdma;
    out->tune = true;
    out->rx_hz = rx;
    out->slot = slot;
    out->tdma = tdma;
    out->voice_tx_start = true;
    out->tx_hz = tx;
    out->voice_group = su->cfg.talkgroup;
    out->voice_source = su->cfg.unit_id;
    set_state(su, P25T_SU_VOICE_TX);
}

void p25t_su_tsbk(p25t_su *su, uint16_t nac, const uint8_t tsbk[P25T_TSBK_BYTES], p25t_su_out *out)
{
    clear(out);
    if (su->state == P25T_SU_VOICE_RX || su->state == P25T_SU_VOICE_TX) return;   /* not on the control channel */
    if (su->cfg.nac != P25_NAC_ANY && nac != su->cfg.nac) return;
    p25t_osp m;
    if (!p25t_osp_parse(tsbk, &m)) return;
    su->cc_nac = nac;
    su->tsbks++;
    su->last_tsbk_ms = su->now;
    uint32_t me = su->cfg.unit_id;
    switch (m.opcode) {
    case P25T_OSP_NET_STS_BCST:
        su->net_ok = m.u.net.wacn == su->cfg.wacn && m.u.net.sysid == su->cfg.sysid;
        if (!su->net_ok && su->state != P25T_SU_HUNT) set_state(su, P25T_SU_HUNT);   /* another system here */
        break;
    case P25T_OSP_RFSS_STS_BCST:
        su->rfss_ok = m.u.rfss.sysid == su->cfg.sysid;
        if (su->rfss_ok) {
            su->rfss = m.u.rfss.rfss;
            su->site = m.u.rfss.site;
            su->cc_channel = m.u.rfss.channel;
            su->site_known = true;
            cc_tx_from_site(su);
        }
        break;
    case P25T_OSP_IDEN_UP:
    case P25T_OSP_IDEN_UP_VU:
    case P25T_OSP_IDEN_UP_TDMA:
        su->bands[m.u.iden.id & 15] = m.u.iden.band;
        cc_tx_from_site(su);
        break;
    case P25T_OSP_U_REG_RSP:
        if (m.u.reg.source_address != me && m.u.reg.source_id != me) break;
        isp_done(su);
        su->registered = m.u.reg.response == P25T_RSP_ACCEPT;
        su->reg_refused = !su->registered;
        su->reg_refused_ms = su->now;
        if (su->registered) su->aff_sent_ms = su->now - P25T_SU_AUTO_GAP_MS;   /* affiliate next */
        break;
    case P25T_OSP_GRP_AFF_RSP:
        if (m.u.aff.target != me) break;
        isp_done(su);
        su->affiliated = m.u.aff.response == P25T_RSP_ACCEPT && m.u.aff.group == su->cfg.talkgroup;
        break;
    case P25T_OSP_U_DE_REG_ACK:
        if (m.u.dereg.target != me) break;
        isp_done(su);
        su->registered = su->affiliated = false;
        break;
    case P25T_OSP_ACK_RSP_FNE:
        /* the system has our packet: no more retries; the real answer follows */
        if (m.u.ack.target == me && su->awaiting) isp_done(su);
        break;
    case P25T_OSP_QUE_RSP:
        if (m.u.deny.target != me || su->state != P25T_SU_REQUEST) break;
        isp_done(su);
        set_state(su, P25T_SU_QUEUED);
        break;
    case P25T_OSP_DENY_RSP:
        if (m.u.deny.target != me) break;
        if (su->state == P25T_SU_REQUEST || su->state == P25T_SU_QUEUED) deny(su, P25T_SU_DENY_SYSTEM, m.u.deny.reason, out);
        else isp_done(su);
        break;
    case P25T_OSP_GRP_V_CH_GRANT:
        if (su->state == P25T_SU_REQUEST || su->state == P25T_SU_QUEUED) {
            if (m.u.grant.source == me && m.u.grant.group == su->cfg.talkgroup) own_grant(su, m.u.grant.channel, out);
            break;
        }
        if (su->state == P25T_SU_CC && wanted(su, m.u.grant.group) >= 0)
            follow(su, m.u.grant.channel, m.u.grant.group, m.u.grant.source, out);
        break;
    case P25T_OSP_GRP_V_CH_GRANT_UPDT:
        if (su->state != P25T_SU_CC) break;
        {
            int best = -1, w;
            for (int i = 0; i < 2; i++)
                if ((w = wanted(su, m.u.update.group[i])) >= 0 && (best < 0 || w < wanted(su, m.u.update.group[best]))) best = i;
            if (best >= 0) follow(su, m.u.update.channel[best], m.u.update.group[best], 0, out);
        }
        break;
    default:
        break;
    }
    if (su->state == P25T_SU_HUNT && system_ok(su)) set_state(su, P25T_SU_CC);
}

void p25t_su_status(p25t_su *su, uint8_t status, p25t_su_out *out)
{
    clear(out);
    if (!su->isp_pending || su->awaiting || !su->cfg.tx_enabled || !su->cc_tx_hz) return;
    if (su->state == P25T_SU_VOICE_RX || su->state == P25T_SU_VOICE_TX || su->state == P25T_SU_HUNT) return;
    if (status != P25T_STATUS_IDLE) return;     /* only idle microslots count */
    if (su->microslots == NOT_STARTED) {
        su->microslots = (uint8_t)((rnd(su) % su->backoff) * P25T_SU_SLOT_MICROSLOTS);
        if (su->microslots) return;
    } else if (su->microslots) {
        su->microslots--;
        if (su->microslots % P25T_SU_SLOT_MICROSLOTS) return;   /* on slot boundaries only */
        if (su->microslots) return;
    }
    out->send_isp = true;
    memcpy(out->isp, su->isp, sizeof out->isp);
    out->nac = su->cc_nac;
    out->tx_hz = su->cc_tx_hz;
    su->awaiting = true;
    su->sent_ms = su->now;
    su->isp_tries++;
    su->isps_sent++;
}

void p25t_su_voice(p25t_su *su, uint16_t group, uint32_t source, bool encrypted, bool ended, p25t_su_out *out)
{
    clear(out);
    if (su->state != P25T_SU_VOICE_RX) return;
    if (group && group != su->call_group) return;   /* another call's frames */
    su->last_voice_ms = su->now;
    if (source) su->call_source = source;
    su->call_encrypted = encrypted;
    if (ended) tune_cc(su, out, false);
}

void p25t_su_ptt(p25t_su *su, bool down, p25t_su_out *out)
{
    clear(out);
    su->ptt = down;
    if (!down) {
        if (su->state == P25T_SU_REQUEST || su->state == P25T_SU_QUEUED) {
            isp_done(su);
            set_state(su, P25T_SU_CC);
        } else if (su->state == P25T_SU_VOICE_TX) {
            out->voice_tx_stop = true;
            /* hang on the channel for the replies, as a call followed */
            su->last_voice_ms = su->now;
            set_state(su, P25T_SU_VOICE_RX);
        }
        return;
    }
    su->deny = P25T_SU_DENY_NONE;
    if (!su->cfg.tx_enabled) {
        su->deny = P25T_SU_DENY_NO_TX;
        return;
    }
    if (su->state == P25T_SU_VOICE_RX) tune_cc(su, out, false);   /* a request goes on the control channel */
    if (su->state != P25T_SU_CC || !system_ok(su)) {
        su->deny = P25T_SU_DENY_SYSTEM;
        return;
    }
    if (!su->registered) {
        su->deny = P25T_SU_DENY_NOT_REGISTERED;
        return;
    }
    if (!su->cc_tx_hz) {
        su->deny = P25T_SU_DENY_BAND;
        return;
    }
    p25t_isp r = { .opcode = P25T_ISP_GRP_V_REQ };
    r.u.voice.svc = 0x04;          /* priority 4, no emergency */
    r.u.voice.group = su->cfg.talkgroup;
    r.u.voice.source = su->cfg.unit_id;
    queue_isp(su, &r);
    su->calls_made++;
    set_state(su, P25T_SU_REQUEST);
}

void p25t_su_tick(p25t_su *su, uint32_t now_ms, p25t_su_out *out)
{
    clear(out);
    su->now = now_ms;
    switch (su->state) {
    case P25T_SU_HUNT:
        if (now_ms - su->state_ms >= P25T_SU_HUNT_MS) {
            su->cc_index = (uint8_t)((su->cc_index + 1) % su->cfg.cc_count);
            tune_cc(su, out, true);
        }
        return;
    case P25T_SU_VOICE_RX:
        if (now_ms - su->last_voice_ms >= P25T_SU_VOICE_HANG_MS) tune_cc(su, out, false);
        return;
    case P25T_SU_VOICE_TX:
        return;
    case P25T_SU_QUEUED:
        if (now_ms - su->state_ms >= P25T_SU_QUEUED_MS) deny(su, P25T_SU_DENY_QUEUE_TIMEOUT, 0, out);
        break;
    case P25T_SU_REQUEST:
        /* acknowledged (ACK_RSP_FNE), but no grant, queue or deny since */
        if (!su->isp_pending && now_ms - su->sent_ms >= P25T_SU_QUEUED_MS) deny(su, P25T_SU_DENY_NO_RESPONSE, 0, out);
        break;
    default:
        break;
    }
    /* on the control channel */
    if (now_ms - su->last_tsbk_ms >= P25T_SU_CC_LOST_MS) {
        isp_done(su);
        tune_cc(su, out, true);
        return;
    }
    if (su->awaiting && now_ms - su->sent_ms >= P25T_SU_RESPONSE_MS) {
        su->awaiting = false;
        if (su->isp_tries >= P25T_SU_TRIES) {
            isp_done(su);
            if (su->state == P25T_SU_REQUEST) deny(su, P25T_SU_DENY_NO_RESPONSE, 0, out);
        } else {
            if (su->backoff < 64) su->backoff = (uint8_t)(su->backoff * 2);
            su->microslots = NOT_STARTED;
        }
    }
    /* automatic registration and affiliation, on our system only, never while a packet is out */
    if (su->state != P25T_SU_CC || !su->cfg.tx_enabled || !system_ok(su) || !su->cc_tx_hz || su->isp_pending) return;
    if (!su->registered) {
        if (su->reg_refused && now_ms - su->reg_refused_ms < P25T_SU_REG_RETRY_MS) return;
        if (now_ms - su->reg_sent_ms < P25T_SU_AUTO_GAP_MS) return;
        p25t_isp r = { .opcode = P25T_ISP_U_REG_REQ };
        r.u.reg.wacn = su->cfg.wacn;
        r.u.reg.sysid = su->cfg.sysid;
        r.u.reg.source = su->cfg.unit_id;
        queue_isp(su, &r);
        su->reg_sent_ms = now_ms;
    } else if (!su->affiliated && now_ms - su->aff_sent_ms >= P25T_SU_AUTO_GAP_MS) {
        p25t_isp r = { .opcode = P25T_ISP_GRP_AFF_REQ };
        r.u.aff.sysid = su->cfg.sysid;
        r.u.aff.group = su->cfg.talkgroup;
        r.u.aff.source = su->cfg.unit_id;
        queue_isp(su, &r);
        su->aff_sent_ms = now_ms;
    }
}

void p25t_su_leave(p25t_su *su, p25t_su_out *out)
{
    clear(out);
    if (su->state == P25T_SU_VOICE_TX) out->voice_tx_stop = true;
    if (su->cfg.tx_enabled && su->registered && su->cc_tx_hz && system_ok(su)) {
        p25t_isp r = { .opcode = P25T_ISP_U_DE_REG_REQ };
        r.u.dereg.wacn = su->cfg.wacn;
        r.u.dereg.sysid = su->cfg.sysid;
        r.u.dereg.source = su->cfg.unit_id;
        p25t_isp_build(&r, out->isp);
        out->send_isp = true;
        out->nac = su->cc_nac;
        out->tx_hz = su->cc_tx_hz;
        su->isps_sent++;
    }
    su->registered = su->affiliated = false;
    isp_done(su);
}
