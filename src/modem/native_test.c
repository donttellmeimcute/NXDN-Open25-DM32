/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "native_test.h"
#include <string.h>
#if defined(CONFIG_FIRSTBOOT) && CONFIG_FIRSTBOOT
#include "firstboot/firstboot.h"
#endif

#if defined(CONFIG_DEV_TX) && CONFIG_DEV_TX
/* A single radio owns the modem. Retain startup and the first 5.76 s of
 * accepted handoffs, with no serial work or allocation on an interrupt. */
typedef struct {
    uint32_t session, start_us, count, complete, reason, sent, pa_keys, rf_irqs;
    uint32_t first_rf_us, first_rf_state, first_rf_sent, request_us, first_pa_us, pa_callbacks;
    uint32_t first_boundary_us[3], boundary_mask, boundary_source, pic_mask, pic_pending;
    uint32_t pic_after_clear, pic_at_quiet, first_stage_us, staged;
    struct { uint32_t us, queued; uint8_t raw[NT_BYTES]; } blocks[NT_TRACE_BLOCKS];
} nt_trace_state;
static nt_trace_state tx_trace;

static int tracing(const nt_test *t)
{
    return nt_tx_live(&t->config) &&
           t->session==tx_trace.session && !tx_trace.complete;
}
void nt_trace_irq_setup(const nt_test *t, uint32_t mask, uint32_t before, uint32_t after)
{
    if (tracing(t)) {
        tx_trace.pic_mask=mask;tx_trace.pic_pending=before;tx_trace.pic_after_clear=after;
    }
}
void nt_trace_irq_quiet(const nt_test *t, uint32_t seen)
{
    if (tracing(t)) tx_trace.pic_at_quiet|=seen;
}
void nt_trace_stage(const nt_test *t, uint32_t now)
{
    if (tracing(t) && !tx_trace.staged++) tx_trace.first_stage_us=now;
}
void nt_trace_summary(uint32_t out[NT_TRACE_WORDS])
{
    const uint32_t values[]={2,tx_trace.session,tx_trace.complete,tx_trace.start_us,
        tx_trace.count,tx_trace.reason,tx_trace.sent,tx_trace.pa_keys,tx_trace.rf_irqs,
        tx_trace.first_rf_us,tx_trace.first_rf_state,tx_trace.first_rf_sent,tx_trace.request_us,
        tx_trace.first_pa_us,tx_trace.pa_callbacks,tx_trace.boundary_mask,
        tx_trace.first_boundary_us[0],tx_trace.first_boundary_us[1],tx_trace.first_boundary_us[2],
        tx_trace.boundary_source,tx_trace.pic_mask,tx_trace.pic_pending,
        tx_trace.pic_after_clear,tx_trace.pic_at_quiet,tx_trace.first_stage_us,tx_trace.staged};
    memcpy(out,values,sizeof values);
}
size_t nt_trace_read(uint32_t session, unsigned first, uint8_t *out, size_t cap)
{
    if (!tx_trace.complete || session!=tx_trace.session || first>=tx_trace.count) return 0;
    size_t n=0;
    for (;first<tx_trace.count && cap-n>=NT_TRACE_BLOCK_BYTES;first++) {
        uint32_t words[]={tx_trace.blocks[first].us,tx_trace.blocks[first].queued};
        for (unsigned w=0;w<2;w++)
            for (unsigned b=0;b<4;b++) out[n++]=(uint8_t)(words[w]>>(8*b));
        memcpy(out+n,tx_trace.blocks[first].raw,NT_BYTES);n+=NT_BYTES;
    }
    return n;
}
#endif

static int active(const nt_test *t) { return t->state==NT_RX || t->state==NT_TX || t->state==NT_DRAIN; }
static int reject(nt_test *t) { t->stats.rejected++; return 0; }
static void finish(nt_test *t, uint8_t why, int fault)
{
    if (t->state==NT_STOPPED || t->state==NT_FAULT || t->state==NT_IDLE) return;
    /* Latch state before hardware shutdown: even a late PA callback is denied. */
    t->state=fault ? NT_FAULT : NT_STOPPED;
    t->reason=why;
    t->stats.faults += fault!=0;
    if (t->owned) { t->ops->quiet(t->ops->ctx); t->stats.quiets++; }
    t->keyed=t->key_requested=0;
    t->tx_count=0; /* canceled queued blocks must never leak into the next run */
}
void nt_init(nt_test *t, const nt_ops *ops) { memset(t,0,sizeof *t); t->ops=ops; }
static int arm(nt_test *t, const nt_config *c, int allow_rf, int audio)
{
#if defined(CONFIG_FIRSTBOOT) && CONFIG_FIRSTBOOT
    if (c && c->mode==NT_TRANSMIT) {
#if defined(CONFIG_DEV_TX) && CONFIG_DEV_TX
        if (!firstboot_native_tx_config(c))
#endif
        { firstboot_deny(FB_NATIVE_TX); return reject(t); }
    }
#endif
    unsigned max_blocks=audio?10000u:NT_MAX_BLOCKS;
    unsigned max_ms=audio?300000u:NT_MAX_MS;
    /* A zero/zero limit is reserved for the internally owned channel. */
    int channel=c && audio && !c->blocks && !c->duration_ms;
    if (!c || t->owned || t->rx_count || (t->state!=NT_IDLE && t->state!=NT_STOPPED) ||
        !c->frequency_hz || (!channel && (!c->blocks || !c->duration_ms)) || c->blocks>max_blocks ||
        c->duration_ms>max_ms || c->lease_ms<NT_MIN_LEASE_MS ||
        (c->lease_ms>NT_MAX_LEASE_MS && !nt_tx_preloaded(c)) ||
        c->audio!=audio ||
        c->p25_sync>1 || (c->mode!=NT_RECEIVE && c->mode!=NT_TRANSMIT) ||
        (c->mode==NT_TRANSMIT && (!allow_rf || (!channel && c->duration_ms < c->blocks*30u+150u)))) return reject(t);
    const nt_ops *ops=t->ops;
    uint32_t generation=t->generation+1u;
    if (!generation) generation=1;
    memset(t,0,sizeof *t);
    t->ops=ops; t->generation=t->session=generation; t->config=*c; t->state=NT_ARMED;
    return 1;
}
int nt_arm(nt_test *t, const nt_config *c, int allow_rf) { return arm(t,c,allow_rf,0); }
int nt_arm_voice(nt_test *t,uint32_t hz,uint32_t ms)
{
    if (!ms) return reject(t);
    nt_config c={.frequency_hz=hz,.blocks=10000,.duration_ms=ms,.lease_ms=500,
                 .mode=NT_RECEIVE,.p25_sync=1,.audio=1};
    return arm(t,&c,0,1);
}
#if defined(CONFIG_DEV_TX) && CONFIG_DEV_TX
int nt_arm_live(nt_test *t,uint32_t hz)
{
    nt_config c={.frequency_hz=hz,.lease_ms=250,.mode=NT_TRANSMIT,.p25_sync=1,.audio=1};
    return arm(t,&c,1,1);
}
int nt_arm_live_rx(nt_test *t,uint32_t hz)
{
    nt_config c={.frequency_hz=hz,.lease_ms=500,.mode=NT_RECEIVE,.p25_sync=1,.audio=1};
    return arm(t,&c,0,1);
}
int nt_live_seal(nt_test *t,uint32_t sid)
{
    if (t->session!=sid || !t->config.audio || t->config.mode!=NT_TRANSMIT ||
        (t->state!=NT_ARMED && t->state!=NT_TX) || t->config.blocks || !t->tx_count) return reject(t);
    t->config.blocks=t->stats.tx_loaded;
    return 1;
}
#endif
int nt_load(nt_test *t, uint32_t sid, uint32_t index, const uint8_t raw[36])
{
    unsigned capacity=nt_tx_preloaded(&t->config) ? NT_TX_STORAGE_CAP : NT_TX_CAP;
    if (!raw || sid!=t->session || (t->state!=NT_ARMED && t->state!=NT_TX) ||
        t->config.mode!=NT_TRANSMIT || index!=t->stats.tx_loaded ||
        (t->config.blocks && index>=t->config.blocks) || t->tx_count>=capacity) return reject(t);
    nt_block *b=&t->tx[t->tx_head]; b->index=index; memcpy(b->raw,raw,36);
    t->tx_head=(uint8_t)((t->tx_head+1)%NT_TX_STORAGE_CAP); t->tx_count++; t->stats.tx_loaded++;
    return 1;
}
int nt_begin(nt_test *t, uint32_t sid, uint32_t now)
{
    const nt_ops *o=t->ops;
    uint32_t irq=o && o->lock ? o->lock(o->ctx) : 0;
    if (sid!=t->session || t->state!=NT_ARMED || !o || !o->start || !o->quiet || !o->restore ||
        (t->config.mode==NT_TRANSMIT && (!o->write || !o->cancel_send || !o->request_key || !o->pa_on ||
                                        t->tx_count<nt_tx_required(&t->config)))) {
        int r=reject(t);
        if (o && o->unlock) o->unlock(o->ctx,irq);
        return r;
    }
    t->owned=1; t->started_us=t->ping_us=now;
#if defined(CONFIG_DEV_TX) && CONFIG_DEV_TX
    if (t->config.mode==NT_TRANSMIT && t->config.audio) {
        memset(&tx_trace,0,offsetof(nt_trace_state,blocks));
        tx_trace.session=sid;tx_trace.start_us=now;
    }
#endif
    if (o->unlock) o->unlock(o->ctx,irq);
    int ok=o->start(o->ctx,t);
    irq=o->lock ? o->lock(o->ctx) : 0;
    /* STOP may have preempted slow board setup. Never resurrect that run;
     * quiet again because setup may have enabled peripherals after STOP. */
    if (t->state!=NT_ARMED) { o->quiet(o->ctx); t->stats.quiets++; ok=0; }
    else if (!ok) finish(t,NT_IO,1);
    else t->state=t->config.mode==NT_RECEIVE ? NT_RX : NT_TX;
    if (o->unlock) o->unlock(o->ctx,irq);
    return ok;
}
int nt_ping(nt_test *t, uint32_t sid, uint32_t now)
{
    if (sid!=t->session || !active(t)) return reject(t);
    /* A late heartbeat cannot resurrect an expired lease. */
    nt_tick(t,now);
    if (!active(t)) return reject(t);
    if (!nt_tx_preloaded(&t->config)) t->ping_us=now;
    return 1;
}
int nt_stop(nt_test *t, uint32_t sid, uint8_t why)
{
    if (!sid || sid!=t->session) return reject(t);
    finish(t,why,0); return 1;
}
void nt_deny(nt_test *t) { if (t->state==NT_ARMED) finish(t,NT_DENIED,1); }
void nt_fault(nt_test *t, uint8_t why) { finish(t,why,1); }
int nt_cleanup(nt_test *t)
{
    if (!t->owned || active(t) || t->state==NT_ARMED) return 0;
    int ok=t->ops->restore(t->ops->ctx);
    if (!ok) {
        t->ops->quiet(t->ops->ctx); t->stats.quiets++;
        t->stats.restore_failures++;
        if (t->state!=NT_FAULT) { t->reason=NT_RESTORE; t->stats.faults++; }
        t->state=NT_FAULT;
    }
    t->owned=0;
#if defined(CONFIG_DEV_TX) && CONFIG_DEV_TX
    if (tracing(t)) {
        uint32_t irq=t->ops->lock?t->ops->lock(t->ops->ctx):0;
        tx_trace.reason=t->reason;tx_trace.sent=t->stats.tx_sent;
        tx_trace.pa_keys=t->stats.pa_keys;tx_trace.rf_irqs=t->stats.rf_irqs;
        tx_trace.boundary_source=t->boundary_source;tx_trace.complete=1;
        if (t->ops->unlock) t->ops->unlock(t->ops->ctx,irq);
    }
#endif
    return 1;
}
int nt_reset(nt_test *t)
{
    if (t->owned || active(t) || t->state==NT_ARMED || t->rx_count) return reject(t);
    t->state=NT_IDLE; t->reason=NT_OK; return 1;
}
int nt_pop(nt_test *t, uint32_t sid, nt_record *r)
{
    if (sid!=t->session || !r || !t->rx_count) return 0;
    *r=t->rx[t->rx_tail]; t->rx_tail=(uint8_t)((t->rx_tail+1)%NT_RX_CAP);
    t->rx_count--; t->stats.rx_read++; return 1;
}
void nt_tick(nt_test *t, uint32_t now)
{
    if (!active(t)) return;
    if (!nt_tx_preloaded(&t->config) && (uint32_t)(now-t->ping_us)>=t->config.lease_ms*1000u) {
        finish(t,NT_LEASE,1); return;
    }
    if (t->config.duration_ms && (uint32_t)(now-t->started_us)>=t->config.duration_ms*1000u) {
        finish(t,t->state==NT_RX ? NT_RX_DURATION : NT_DEADLINE,t->state!=NT_RX); return;
    }
    if (t->state==NT_DRAIN && (uint32_t)(now-t->drain_us)>=NT_DRAIN_US) { finish(t,NT_TX_COMPLETE,0); return; }
    if (t->state==NT_TX && (uint32_t)(now-(t->boundary_seen?t->boundary_us:t->started_us))>=
        (t->boundary_seen?NT_STALL_US:NT_START_US)) finish(t,NT_STALLED,1);
}
void nt_boundary(nt_test *t, uint8_t source, uint32_t now)
{
#if defined(CONFIG_DEV_TX) && CONFIG_DEV_TX
    if (tracing(t) && source<=NT_IRQ_RX_SLOT) {
        unsigned i=source-NT_IRQ_PHY;
        if (!(tx_trace.boundary_mask&(1u<<i))) {
            tx_trace.boundary_mask|=1u<<i;
            tx_trace.first_boundary_us[i]=now;
        }
    }
#endif
    if (source==NT_IRQ_PHY) t->stats.phy_irqs++;
    else if (source==NT_IRQ_TX_SLOT) t->stats.tx_slot_irqs++;
    else if (source==NT_IRQ_RX_SLOT) t->stats.rx_slot_irqs++;
    if (!active(t)) { t->stats.ignored_irqs++; return; }
    nt_tick(t,now); if (!active(t)) return;
    if (t->state==NT_RX) return;
    /* TX hands blocks over only in PHY_RDY. PHY_RDY is the first interrupt
     * of every slot; the modem fetches the head of TX RAM for the coming slot
     * within about 0.4 ms of it. The 2026-11-06 traces wrote on the TX-slot
     * vector 0.43-0.45 ms after PHY_RDY and the first three bytes of every
     * block went out from the previous block. Slot vectors only supervise:
     * one that arrives more than half a slot after the last PHY_RDY means
     * this slot's PHY_RDY was lost, and a late write would send a mixed
     * block, so the run stops instead. */
    if (source!=NT_IRQ_PHY) {
        t->stats.duplicate_irqs++;
        if (source==NT_IRQ_TX_SLOT && t->state==NT_TX && t->boundary_seen &&
            (uint32_t)(now-t->boundary_us)>NT_TX_SLOT_LAG_US)
            finish(t,NT_LATE,1);
        return;
    }
    if (t->boundary_seen) {
        uint32_t gap=now-t->boundary_us;
        if (gap<NT_SLOT_US/2u) { t->stats.duplicate_irqs++; return; }
        t->stats.last_gap_us=gap;
        if (gap>t->stats.max_gap_us) t->stats.max_gap_us=gap;
        if (t->state==NT_TX && (gap<27000u || gap>33000u)) { finish(t,gap<27000u?NT_EARLY:NT_LATE,1); return; }
    }
    t->boundary_seen=1; t->boundary_source=source; t->boundary_us=now;
    if (t->state==NT_DRAIN) {
        if (!t->canceled) { t->ops->cancel_send(t->ops->ctx); t->canceled=1; }
        return;
    }
    if (!t->tx_count) { finish(t,NT_UNDERRUN,1); return; }
    nt_block *b=&t->tx[t->tx_tail];
    if (b->index!=t->stats.tx_sent || !t->ops->write(t->ops->ctx,b->raw)) { finish(t,NT_IO,1); return; }
#if defined(CONFIG_DEV_TX) && CONFIG_DEV_TX
    nt_trace_stage(t,now); /* trace v2: first/total PHY_RDY handoffs */
    if (tracing(t) && tx_trace.count<NT_TRACE_BLOCKS) {
        unsigned i=tx_trace.count++;
        tx_trace.blocks[i].us=now;tx_trace.blocks[i].queued=t->tx_count;
        memcpy(tx_trace.blocks[i].raw,b->raw,NT_BYTES);
    }
#endif
    t->tx_tail=(uint8_t)((t->tx_tail+1)%NT_TX_STORAGE_CAP); t->tx_count--; t->stats.tx_sent++;
    if (t->config.blocks && t->stats.tx_sent==t->config.blocks) { t->state=NT_DRAIN; t->drain_us=now; }
}
void nt_rx(nt_test *t, uint32_t now, uint32_t flags, uint32_t info, const uint8_t raw[36])
{
    if (t->state!=NT_RX) { t->stats.ignored_irqs++; return; }
    nt_tick(t,now); if (t->state!=NT_RX) return;
    uint32_t sequence=t->stats.rx_seen++;
    if (!raw) { finish(t,NT_IO,1); return; }
    if (t->rx_count==NT_RX_CAP) { t->stats.rx_dropped++; finish(t,NT_RX_OVERFLOW,1); return; }
    nt_record *r=&t->rx[t->rx_head];
    r->sequence=sequence; r->time_us=now; r->sys_flags=flags; r->rx_info=info; memcpy(r->raw,raw,36);
    t->rx_head=(uint8_t)((t->rx_head+1)%NT_RX_CAP); t->rx_count++; t->stats.rx_saved++;
    if (t->config.blocks && t->stats.rx_saved==t->config.blocks) finish(t,NT_RX_LIMIT,0);
}
void nt_abnormal(nt_test *t)
{
    t->stats.abnormal_irqs++;
#if defined(CONFIG_FIRSTBOOT) && CONFIG_FIRSTBOOT
    /* A DMR slot-format warning was observed after three valid P25 RX blocks.
     * Keep it visible while testing stream continuity with TX compiled out. */
    if (t->state==NT_RX) { t->stats.ignored_irqs++; return; }
#endif
    if (active(t)) finish(t,NT_ABNORMAL,1); else t->stats.ignored_irqs++;
}
void nt_rf_tx(nt_test *t, uint32_t now)
{
#if defined(CONFIG_DEV_TX) && CONFIG_DEV_TX
    if (tracing(t) && !t->stats.rf_irqs) {
        tx_trace.first_rf_us=now;tx_trace.first_rf_state=t->state;
        tx_trace.first_rf_sent=t->stats.tx_sent;
    }
#endif
    t->stats.rf_irqs++;
    nt_tick(t,now);
    if (t->state==NT_RX) {
#if defined(CONFIG_FIRSTBOOT) && CONFIG_FIRSTBOOT
        /* This notification was observed during actual RX. It is not a PA
         * measurement. In the immutable TX-inhibited image, count it without
         * invoking the RF-switch/key callback, so RX can deliver its data. */
        t->stats.ignored_irqs++; return;
#else
        finish(t,NT_UNEXPECTED_RF,1); return;
#endif
    }
    if ((t->state!=NT_TX && t->state!=NT_DRAIN) || !t->stats.tx_sent) { t->stats.ignored_irqs++; return; }
    if (!t->key_requested) {
        t->key_requested=1;
#if defined(CONFIG_DEV_TX) && CONFIG_DEV_TX
        if (tracing(t)) tx_trace.request_us=now;
#endif
        t->ops->request_key(t->ops->ctx);
    }
}
void nt_pa_ready(nt_test *t, uint32_t now)
{
#if defined(CONFIG_DEV_TX) && CONFIG_DEV_TX
    if (tracing(t) && !tx_trace.pa_callbacks++) tx_trace.first_pa_us=now;
#endif
    nt_tick(t,now);
    if ((t->state==NT_TX || t->state==NT_DRAIN) && t->key_requested && !t->keyed) {
        t->keyed=1; t->ops->pa_on(t->ops->ctx); t->stats.pa_keys++;
    } else t->stats.ignored_irqs++;
}
void nt_status(const nt_test *t, uint32_t o[NT_STATUS_WORDS])
{
    const nt_stats *s=&t->stats;
    const uint32_t v[NT_STATUS_WORDS]={1,t->state,t->reason,t->session,t->owned,t->config.mode,
      t->config.frequency_hz,t->config.blocks,t->config.duration_ms,t->config.lease_ms,
      t->rx_count,t->tx_count,s->rx_seen,s->rx_saved,s->rx_dropped,s->rx_read,s->tx_loaded,s->tx_sent,
      s->phy_irqs,s->tx_slot_irqs,s->rx_slot_irqs,s->rf_irqs,s->abnormal_irqs,s->duplicate_irqs,
      s->ignored_irqs,s->pa_keys,s->quiets,s->faults,s->rejected,s->max_gap_us,s->last_gap_us,
      s->restore_failures,t->started_us,t->boundary_us,t->config.p25_sync,t->key_requested,t->keyed};
    memcpy(o,v,sizeof v);
}
