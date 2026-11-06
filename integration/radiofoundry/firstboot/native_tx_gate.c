/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "config.h"
#if CONFIG_DEV_TX
#include "firstboot.h"
#include "native_test.h"
#include "hal_time.h"
/* No persistent enable switch: the grant refers to this native ownership and
 * generation only. Fast-off revokes it before touching RF registers. */
static nt_test *owner;
static uint32_t session;
bool firstboot_native_tx_config(const nt_config *c)
{
    if (!c || c->mode!=NT_TRANSMIT || c->frequency_hz!=145500000u || c->p25_sync!=1) return false;
    if (c->audio) return c->audio==1 && !c->duration_ms && c->lease_ms==250;
    return
        c->blocks && c->blocks<=NT_MAX_BLOCKS && c->duration_ms && c->duration_ms<=NT_MAX_MS &&
        c->duration_ms>=c->blocks*30u+150u &&
        ((c->lease_ms>=NT_MIN_LEASE_MS && c->lease_ms<=250u) || nt_tx_preloaded(c));
}
void firstboot_native_tx_close(void)
{
    uint32_t irq=firstboot_irq_save();owner=NULL;session=0;firstboot_irq_restore(irq);
}
bool firstboot_native_tx_open(nt_test *t)
{
    uint32_t irq=firstboot_irq_save();owner=NULL;session=0;
    bool ok=t && t->owned && t->state==NT_ARMED && t->session && firstboot_native_tx_config(&t->config);
    if(ok) { owner=t;session=t->session; }
    firstboot_irq_restore(irq);return ok;
}
bool firstboot_tx_allowed(enum firstboot_event event)
{
    uint32_t irq=firstboot_irq_save();nt_test *t=owner;bool ok=false;
    if(t && t->session==session && t->owned && firstboot_native_tx_config(&t->config) &&
       (t->state==NT_ARMED || t->state==NT_TX || t->state==NT_DRAIN)) {
        uint32_t now=hal_micros();
        if((uint32_t)(now-t->ping_us)<t->config.lease_ms*1000u &&
           (!t->config.duration_ms || (uint32_t)(now-t->started_us)<t->config.duration_ms*1000u)) {
            if(event==FB_BOARD_TX || event==FB_FD_TX)ok=true;
            else if(t->state!=NT_ARMED && t->stats.tx_sent && t->key_requested) {
                if(event==FB_RF_SWITCH)ok=true;
                else if(event==FB_PA_KEY || event==FB_PA_GPIO)ok=t->keyed!=0;
            }
        }
    }
    firstboot_irq_restore(irq);return ok;
}
#endif
