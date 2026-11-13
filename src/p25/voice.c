/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "voice.h"
#include "p25_runtime.h"

static void admission_reset(p25_voice *v)
{
    v->crypto_clear = v->have_group = 0;
    v->playing = v->emergency = v->encrypted = 0;
    v->active_nac = P25_NAC_ANY;
    v->active_group = 0;
    v->active_source = 0;
    dm32_imbe_init(&v->codec);
}

void p25_voice_rx_reset(p25_voice *v)
{
    /* Stale receive records must not reset the shared active TX codec. */
    if (v->transmitting) return;
    p25_rx_init(&v->framer, P25_NAC_ANY, 2);
    admission_reset(v);
}

void p25_voice_init(p25_voice *v, uint16_t nac, uint16_t group,
                    p25_pcm_sink sink, void *ctx)
{
    memset(v, 0, sizeof *v);
    v->sink = sink;
    v->sink_ctx = ctx;
    v->group_filter = group;
    v->nac_filter = nac == 0xf7e ? P25_NAC_ANY : nac;
    v->last_nac = P25_NAC_ANY;
    p25_rx_init(&v->framer, P25_NAC_ANY, 2);
    admission_reset(v);
}

void p25_voice_rx_dibit(p25_voice *v, uint8_t dibit)
{
    if (v->transmitting || !p25_rx_push_dibit(&v->framer, dibit)) return;
    p25_decoded d;
    uint32_t at=v->micros?v->micros():0;
    int decoded=p25_decode_frame(v->framer.frame, p25_frame_bytes(v->framer.nid.duid), &d);
    uint32_t elapsed=v->micros?v->micros()-at:0;
    if (elapsed>v->fec_max_us)v->fec_max_us=elapsed;
    if (decoded) {
        admission_reset(v);
        return;
    }
    v->last_nac = d.nid.nac;
    v->last_duid = d.nid.duid;
    if (v->nac_filter != P25_NAC_ANY && d.nid.nac != v->nac_filter) {
        v->foreign_frames++;
        if (v->active_nac != P25_NAC_ANY) admission_reset(v);
        return;
    }
    v->rx_frames++;
    if (v->active_nac != d.nid.nac) {
        admission_reset(v);
        v->active_nac = d.nid.nac;
    }
    if (d.nid.duid == P25_DUID_TDU || d.nid.duid == P25_DUID_TDULC) {
        admission_reset(v);
        return;
    }
    if (!d.metadata_valid || (d.nid.duid == P25_DUID_HDU && d.manufacturer_id > 1)) {
        /* Do not reuse permission from a previous good header over damage. */
        v->crypto_clear = v->have_group = 0;
    } else if (d.nid.duid == P25_DUID_HDU) {
        admission_reset(v);
        v->active_nac = d.nid.nac;
        v->crypto_clear = d.clear_voice;
        v->active_group = d.talkgroup;
        v->have_group = d.talkgroup != 0;
    } else if (d.nid.duid == P25_DUID_LDU1) {
        if ((v->have_group && v->active_group != d.talkgroup) ||
            (v->active_source && v->active_source != d.source_id))
            v->crypto_clear = 0;
        v->active_group = d.talkgroup;
        v->active_source = d.source_id;
        v->have_group = d.talkgroup && d.source_id && d.source_id <= 9999999u;
        if (!d.clear_voice) v->crypto_clear = 0;
        v->emergency = (d.service_options & 0x80u) != 0;
        /* LC's unprotected bit alone cannot override an encrypted ALGID.
         * Late entry waits for a valid LDU2 encryption sync. */
    } else if (d.nid.duid == P25_DUID_LDU2) {
        v->crypto_clear = d.clear_voice;
    }
    if (d.nid.duid != P25_DUID_LDU1 && d.nid.duid != P25_DUID_LDU2) return;
    int admitted = v->crypto_clear && v->have_group &&
        (!v->group_filter || v->active_group == v->group_filter || v->active_group == 0xffff);
    v->playing = (uint8_t)admitted;
    v->encrypted = d.metadata_valid && !d.clear_voice;
    for (unsigned i = 0; i < 9; i++) {
        int16_t pcm[160] = {0};
        if (!admitted) v->rx_muted++;
        /* Conservative implementation policy, not a TIA concealment rule.
         * Golay/Hamming can miscorrect beyond t; no audio-quality claim. */
        else {
            at=v->micros?v->micros():0;
            int ok=d.voice_u0_corrected_bits[i] <= 3 && d.voice_corrected_bits[i] <= 10 &&
                   dm32_imbe_decode(&v->codec, d.imbe[i], pcm);
            elapsed=v->micros?v->micros()-at:0;
            if(elapsed>v->imbe_max_us)v->imbe_max_us=elapsed;
            if(!ok) {
                memset(pcm, 0, sizeof pcm);
                v->rx_erasures++;
                dm32_imbe_init(&v->codec);
            }
        }
        if (v->sink) v->sink(v->sink_ctx, pcm);
    }
}

static p25_voice_packet *next_packet(p25_voice *v)
{
    return v->count < P25_VOICE_QUEUE ? &v->queue[v->head] : NULL;
}

static void commit_packet(p25_voice *v, uint16_t bytes)
{
    v->queue[v->head].bytes = bytes;
    v->head = (uint8_t)((v->head + 1) % P25_VOICE_QUEUE);
    v->count++;
    v->tx_frames++;
}

void p25_voice_tx_abort(p25_voice *v)
{
    v->transmitting = v->started = v->stopping = v->terminator_pending = 0;
    v->count = v->head = v->tail = v->voice_count = 0;
    v->packet_symbol = 0;
    p25_voice_rx_reset(v);
}

static int fail(p25_voice *v)
{
    p25_voice_tx_abort(v);
    v->fault = 1;
    return 0;
}

int p25_voice_tx_begin(p25_voice *v, const p25_call *call)
{
    if (!call || v->transmitting || !call->talkgroup ||
        !call->source_id || call->source_id > 9999999u) return 0;
    p25_voice_tx_abort(v);
    v->fault = 0;
    if (p25_encode_hdu(call, v->queue[0].data)) return 0;
    v->call = *call;
    v->transmitting = 1;
    v->next_duid = P25_DUID_LDU1;
    commit_packet(v, P25_HDU_BYTES);
    return 1;
}

int p25_voice_tx_pcm(p25_voice *v, int16_t pcm[160])
{
    if (!v->transmitting || v->stopping || !pcm) return 0;
    if (!dm32_imbe_encode(&v->codec, pcm, v->voice[v->voice_count])) return fail(v);
    if (++v->voice_count < 9) return 1;
    p25_voice_packet *p = next_packet(v);
    const uint8_t lsd[2] = {0, 0};
    if (!p || p25_encode_ldu(&v->call, v->next_duid, v->voice, lsd, p->data)) return fail(v);
    commit_packet(v, P25_LDU_BYTES);
    v->voice_count = 0;
    v->next_duid ^= P25_DUID_LDU1 ^ P25_DUID_LDU2;
    v->started = 1;
    return 1;
}

static void append_terminator(p25_voice *v)
{
    p25_voice_packet *p = next_packet(v);
    if (v->terminator_pending && p) {
        if (p25_encode_tdu(&v->call, p->data)) { fail(v); return; }
        commit_packet(v, P25_TDU_BYTES);
        v->terminator_pending = 0;
    }
}

int p25_voice_tx_end(p25_voice *v)
{
    if (!v->transmitting || v->stopping) return 0;
    if (!v->started && !v->voice_count) { p25_voice_tx_abort(v); return 1; }
    while (v->voice_count) {
        int16_t silence[160] = {0};
        if (!p25_voice_tx_pcm(v, silence)) return 0;
    }
    v->stopping = v->terminator_pending = 1;
    append_terminator(v);
    return !v->fault;
}

size_t p25_voice_tx_read(p25_voice *v, uint8_t *out, size_t capacity)
{
    if (!v->transmitting || !v->started || !out || !capacity) return 0;
    size_t n = 0;
    while (n < capacity) {
        append_terminator(v);
        if (!v->count) {
            if (v->stopping) { p25_voice_tx_abort(v); break; }
            v->tx_underruns++;
            fail(v);
            break;
        }
        p25_voice_packet *p = &v->queue[v->tail];
        unsigned bit = v->packet_symbol * 2u;
        out[n++] = (uint8_t)((p->data[bit / 8] >> (6 - bit % 8)) & 3u);
        if (++v->packet_symbol == p->bytes * 4u) {
            v->packet_symbol = 0;
            v->tail = (uint8_t)((v->tail + 1) % P25_VOICE_QUEUE);
            v->count--;
        }
    }
    /* A read ending exactly at the TDU boundary is done immediately. */
    if (v->transmitting && v->stopping && !v->count && !v->terminator_pending)
        p25_voice_tx_abort(v);
    return n;
}

int p25_voice_tx_done(const p25_voice *v) { return !v->transmitting; }
