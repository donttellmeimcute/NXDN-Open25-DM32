/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "config.h"
#if CONFIG_P25_NATIVE
#include "hrc_native_port.h"
#include "platform.h"
/* Timer 6 is the independent 1 ms lease/duration guard; Timer 3 preserves the
 * board's native DMR PA delay. Refuse borrowed timers that are still running. */
static hn_chip *chip;
static nt_test *test;
static uint8_t installed;
static const uint8_t sources[]={IRQ_MODEM_SYS,IRQ_MODEM_TS_TX,IRQ_MODEM_TS_RX,IRQ_MODEM_RF_TX,IRQ_TIMER3,IRQ_TIMER6};
static hrc_irq_fn saved_fn[6];
static uint8_t saved_masked[6];
static void sys(void) { hn_sys(chip,hrc_micros()); }
static void ts_tx(void) { hn_slot(chip,NT_IRQ_TX_SLOT,hrc_micros()); }
static void ts_rx(void) { hn_slot(chip,NT_IRQ_RX_SLOT,hrc_micros()); }
static void rftx(void) { hn_rftx(chip,hrc_micros()); }
static void guard(void) { hrc_timer_ack(6); if (test) nt_tick(test,hrc_micros()); }
static void pa(void) { hrc_timer_ack(TIMER_PA); hrc_timer_stop(TIMER_PA); if (test) nt_pa_ready(test,hrc_micros()); }
int hn_port_on(hn_chip *d, nt_test *t)
{
    uint32_t s=hrc_irq_save();
    if (installed || (TMR_CTRL(6)&1u) || (TMR_CTRL(TIMER_PA)&1u)) { hrc_irq_restore(s); return 0; }
    hrc_irq_fn f[]={sys,ts_tx,ts_rx,rftx,pa,guard};
    for (unsigned i=0;i<6;i++) {
        uint8_t src=sources[i];
        saved_fn[i]=hrc_irq_table[src];
        saved_masked[i]=((src<32?PIC_MASK:PIC_MASK_1)>>(src&31u))&1u;
        hrc_irq_register(src,f[i]);
    }
    chip=d; test=t; installed=1;
#if CONFIG_DEV_TX
    uint32_t before=0,after=0;
    if (nt_tx_live(&t->config)) {
        before=PIC_INT_ST_1;
        /* Clear only the RF-start history bit, not the modem IRQ itself. */
        PIC_INT_ST_1=1u<<(IRQ_MODEM_RF_TX&31u);
        after=PIC_INT_ST_1;
    }
#endif
    for (unsigned i=0;i<6;i++) hrc_irq_enable(sources[i],true);
#if CONFIG_DEV_TX
    nt_trace_irq_setup(t,PIC_MASK_1,before,after);
#endif
    hrc_timer_periodic(6,1000);
    hrc_irq_restore(s); return 1;
}
void hn_port_quiet(void)
{
    if (!installed) return;
#if CONFIG_DEV_TX
    nt_trace_irq_quiet(test,PIC_INT_ST_1);
#endif
    for (unsigned i=0;i<5;i++) hrc_irq_enable(sources[i],false);
    hrc_timer_stop(TIMER_PA);
}
void hn_port_restore(void)
{
    uint32_t s=hrc_irq_save();
    if (installed) {
        hrc_timer_stop(6); hrc_timer_stop(TIMER_PA);
        for (unsigned i=0;i<6;i++) { hrc_irq_register(sources[i],saved_fn[i]); hrc_irq_enable(sources[i],!saved_masked[i]); }
        installed=0; test=NULL; chip=NULL;
    }
    hrc_irq_restore(s);
}
#endif
