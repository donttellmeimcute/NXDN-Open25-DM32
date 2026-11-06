/* SPDX-License-Identifier: GPL-3.0-or-later */
/* HR_C7000 guide 9.4-9.6, printed 294-297, Tables 46-49/Figures 70-71;
 * RadioFoundry dfbaf20c hrc7000_bs.c raw-TX sequencing. Hardware-unverified.
 * RF_MODE, RF_MOD_BIAS, DEV_LIMITER and RX FIR are deliberately never written:
 * board routing/calibration is retained, not replaced by the guide's IF example.
 * Both TX halves get identical bytes, matching upstream's tested default; this
 * does not assert measured alternate-buffer ownership or P25 pulse shaping. */
#include "hrc_native_chip.h"
#include <string.h>
#define RX_DONE (1u<<11)
#define PHY_READY (1u<<12)
#define ABNORMAL (1u<<16)
#define OWNED_SYS (RX_DONE|PHY_READY|ABNORMAL)
_Static_assert(NT_SNAPSHOT_WORDS==8+2*HN_DIAG_REGS,"snapshot layout mismatch");
static const uint16_t regs[HN_REGS]={C7_WORK_MODE,C7_LAYER2_CONTROL,C7_LAYER2_SLOTON,
 C7_LAYER2_TXRX_CTRL,C7_SYS_INTERP_MASK,C7_RECV_MS_SYNC_H,C7_RECV_MS_SYNC_L,
 C7_RECV_BS_SYNC_H,C7_RECV_BS_SYNC_L,C7_RECV_TDMA1_SYNC_H,C7_RECV_TDMA1_SYNC_L,
 C7_RECV_TDMA2_SYNC_H,C7_RECV_TDMA2_SYNC_L,C7_RECV_RC_SYNC_H,C7_RECV_RC_SYNC_L};
static const uint16_t diag_regs[HN_DIAG_REGS]={C7_WORK_MODE,C7_LAYER2_CONTROL,C7_LAYER2_SLOTON,
 C7_LAYER2_TXRX_CTRL,C7_SYS_INTERP_MASK,C7_RECV_MS_SYNC_H,C7_RECV_MS_SYNC_L,
 C7_RECV_BS_SYNC_H,C7_RECV_BS_SYNC_L,C7_RECV_TDMA1_SYNC_H,C7_RECV_TDMA1_SYNC_L,
 C7_RECV_TDMA2_SYNC_H,C7_RECV_TDMA2_SYNC_L,C7_RECV_RC_SYNC_H,C7_RECV_RC_SYNC_L,
 C7_RF_MODE,C7_RF_CONTROL,C7_SYS_SOFT_RSTN,C7_RECV_INFO_COUNT,C7_RX_TYPE_INFO,
 C7_SYS_INTERP_LIST,C7_LAYER2_STATUS,C7_SIG_CENTER,C7_RX_IF_FREQ};
static void event(hn_chip *d,unsigned which,uint32_t now)
{
    d->last_event=which;d->last_us=now;
    for (unsigned i=0;i<HN_DIAG_REGS;i++)d->event_regs[i]=hrc7000_rd(d->modem,diag_regs[i]);
}
static size_t snapshot(void *ctx,uint32_t *out,size_t cap)
{
    hn_chip *d=ctx;
    if (!d->modem || cap<NT_SNAPSHOT_WORDS)return 0;
    out[0]=1;out[1]=d->prepared;out[2]=d->saved_valid;out[3]=d->last_event;
    out[4]=d->last_us;out[5]=d->last_sys;out[6]=d->last_type;out[7]=d->last_sys_us;
    for (unsigned i=0;i<HN_DIAG_REGS;i++) {
        out[8+i]=hrc7000_rd(d->modem,diag_regs[i]);out[8+HN_DIAG_REGS+i]=d->event_regs[i];
    }
    return NT_SNAPSHOT_WORDS;
}
static void w(hn_chip *d, unsigned r, uint32_t v) { hrc7000_wr(d->modem,r,v); }
static void quiet(void *ctx)
{
    hn_chip *d=ctx;
    d->hooks->fast_off(d->hooks->ctx); /* PA first, before any other processing */
    d->hooks->irq_quiet(d->hooks->ctx);
    w(d,C7_LAYER2_TXRX_CTRL,0); w(d,C7_LAYER2_CONTROL,0); w(d,C7_SYS_INTERP_MASK,0);
}
static int start(void *ctx, nt_test *t)
{
    hn_chip *d=ctx;
    d->test=t; d->prepared=1; d->saved_valid=0;
#if CONFIG_DEV_TX
    d->staged_index=0; /* index zero is the bootstrapping write on TX_SLOT */
#endif
    if (!d->hooks->prepare(d->hooks->ctx,&t->config)) return 0;
    for (unsigned i=0;i<HN_REGS;i++) d->saved[i]=hrc7000_rd(d->modem,regs[i]);
    d->saved_valid=1;
    w(d,C7_LAYER2_TXRX_CTRL,0); w(d,C7_LAYER2_SLOTON,0);
    w(d,C7_WORK_MODE,C7_WORK_MODE_FM); w(d,C7_WORK_MODE,0x02);
    if (t->config.mode==NT_RECEIVE && t->config.p25_sync) {
        /* Sync comparators contain 24 bits per register, with enable in H31.
         * Splitting 16/32 silently loses L31:24 on the actual chip. */
        const uint64_t sync=UINT64_C(0x5575f5ff77ff);
        uint32_t hi=0x80000000u|(uint32_t)(sync>>24),lo=(uint32_t)(sync&0xffffffu);
        w(d,C7_RECV_MS_SYNC_H,hi); w(d,C7_RECV_MS_SYNC_L,lo);
        w(d,C7_RECV_BS_SYNC_H,hi); w(d,C7_RECV_BS_SYNC_L,lo);
        for (unsigned i=9;i<HN_REGS;i+=2) w(d,regs[i],d->saved[i]&~0x80000000u);
    }
    w(d,C7_SYS_INTERP_CLEAR,OWNED_SYS); w(d,C7_INTERP_CLEAR,0x0fu);
    w(d,C7_SYS_INTERP_MASK,t->config.mode==NT_RECEIVE?OWNED_SYS:PHY_READY|ABNORMAL);
    w(d,C7_LAYER2_CONTROL,t->config.mode==NT_RECEIVE?0x40u:0x82u);
#if defined(CONFIG_DEV_TX) && CONFIG_DEV_TX
    if (t->config.mode==NT_TRANSMIT && t->config.audio)
        hrc7000_audio_bits(d->modem,C7_AUDIO_PCM_MIC,0);
#endif
    if (!d->hooks->irq_on(d->hooks->ctx,t)) return 0;
    if (t->config.mode==NT_RECEIVE) w(d,C7_LAYER2_TXRX_CTRL,0x41);
    d->last_sys=d->last_type=d->last_sys_us=0;event(d,1,t->started_us);
    return 1;
}
static void copy_block(hn_chip *d, const uint8_t raw[36])
{
    for (unsigned i=0;i<36;i++) { d->modem->buf[i]=raw[i]; d->modem->buf[36+i]=raw[i]; }
}
#if CONFIG_DEV_TX
static void stage_live(hn_chip *d, uint32_t now)
{
    nt_test *t=d->test;
    if (!nt_tx_live(&t->config) || t->state!=NT_TX || !t->stats.tx_sent || !t->tx_count ||
        d->staged_index==t->stats.tx_sent) return;
    /* PHY_RDY for the next slot is about 26.3 ms after the preceding strobe.
     * A same-slot duplicate must never overwrite the block still on air. */
    uint32_t age=now-t->boundary_us;
    if (age<23000u || age>28500u) return;
    const nt_block *b=&t->tx[t->tx_tail];
    if (b->index!=t->stats.tx_sent) return;
    copy_block(d,b->raw);
    d->staged_index=b->index;
    nt_trace_stage(t,now);
}
#endif
static int write_block(void *ctx, const uint8_t raw[36])
{
    hn_chip *d=ctx;
#if CONFIG_DEV_TX
    if (nt_tx_live(&d->test->config) && d->test->stats.tx_sent) {
        /* This slot has already begun fetching its head. If the early
         * handoff was missed, stop rather than transmit a mixed block. */
        if (d->staged_index!=d->test->stats.tx_sent) return 0;
    } else
#endif
        copy_block(d,raw);
    w(d,C7_LAYER2_TXRX_CTRL,0x80); return 1; /* one send, no autotest/repeat */
}
static void cancel(void *ctx) { w(ctx,C7_LAYER2_TXRX_CTRL,0); }
static void key(void *ctx) { hn_chip *d=ctx; d->hooks->key_request(d->hooks->ctx); }
static void pa(void *ctx) { hn_chip *d=ctx; d->hooks->pa_on(d->hooks->ctx); }
static uint32_t lock(void *ctx) { hn_chip *d=ctx; return d->hooks->lock ? d->hooks->lock(d->hooks->ctx) : 0; }
static void unlock(void *ctx, uint32_t irq) { hn_chip *d=ctx; if (d->hooks->unlock) d->hooks->unlock(d->hooks->ctx,irq); }
static int restore(void *ctx)
{
    hn_chip *d=ctx;
    if (d->saved_valid) {
        w(d,C7_WORK_MODE,C7_WORK_MODE_FM);
        for (unsigned i=1;i<HN_REGS;i++)
            w(d,regs[i],i==3 ? d->saved[i]&~0x80u : d->saved[i]);
        w(d,C7_WORK_MODE,d->saved[0]);
        w(d,C7_SYS_INTERP_CLEAR,OWNED_SYS); w(d,C7_INTERP_CLEAR,0x0fu);
    }
    d->hooks->irq_restore(d->hooks->ctx);
    d->test=NULL;
    int ok=!d->prepared || d->hooks->restore(d->hooks->ctx);
    d->prepared=d->saved_valid=0;
    return ok;
}
void hn_init(hn_chip *d, hrc7000_t *m, const hn_hooks *hooks, nt_ops *out)
{
    memset(d,0,sizeof *d); d->modem=m; d->hooks=hooks;
    *out=(nt_ops){d,start,write_block,cancel,key,pa,quiet,restore,lock,unlock,snapshot};
}
void hn_sys(hn_chip *d, uint32_t now)
{
    uint32_t flags=hrc7000_rd(d->modem,C7_SYS_INTERP_LIST);
    d->last_sys=flags;d->last_sys_us=now;event(d,2,now);
    uint32_t owned_flags=flags&OWNED_SYS;
    if (!d->test || !owned_flags) return;
    uint32_t info=0; uint8_t raw[36];
    if (flags&RX_DONE) {
        info=hrc7000_rd(d->modem,C7_RX_TYPE_INFO);
        d->last_type=info;
        for (unsigned i=0;i<36;i++) raw[i]=d->modem->buf[C7_RX_RAM+0x30+i];
    }
    w(d,C7_SYS_INTERP_CLEAR,owned_flags); /* retain full snapshot, acknowledge only owned bits */
    if (flags&ABNORMAL) {
        nt_abnormal(d->test);
        if (d->test->state!=NT_RX) return;
    }
    if (flags&RX_DONE) nt_rx(d->test,now,flags,info,raw);
    if (flags&PHY_READY) {
        nt_boundary(d->test,NT_IRQ_PHY,now);
#if CONFIG_DEV_TX
        stage_live(d,now);
#endif
    }
}
void hn_slot(hn_chip *d, uint8_t source, uint32_t now)
{
    event(d,source==NT_IRQ_TX_SLOT?3:4,now);
    w(d,C7_INTERP_CLEAR,source==NT_IRQ_TX_SLOT?C7_INT_TS_TX:C7_INT_TS_RX);
    if (d->test) nt_boundary(d->test,source,now);
}
void hn_rftx(hn_chip *d, uint32_t now)
{
    event(d,5,now);
    w(d,C7_INTERP_CLEAR,C7_INT_RF_TX);
    if (d->test) nt_rf_tx(d->test,now);
}
