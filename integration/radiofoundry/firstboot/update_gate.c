/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "config.h"
#if CONFIG_DEV_UPDATE
#include "firstboot.h"
#include "hal_time.h"
/* The same SHA-pinned universal RAM helper already used at power-on.
 * CRC here is an image identity/error check; host also checks SHA-256. */
enum { UPDATE_BYTES=7784, UPDATE_CRC=0xc57b2bcau, UPDATE_BASE=0x44000u };
static uint32_t offset,last_ms;
static bool updating;
extern uint32_t storage_crc32(uint32_t,const void *,uint32_t);
static uint32_t get32(const uint8_t *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
bool firstboot_update_busy(void)
{
    uint32_t irq=firstboot_irq_save();
    if(updating && hal_millis()-last_ms>=5000u)updating=false;
    bool busy=updating;firstboot_irq_restore(irq);return busy;
}
void firstboot_update_cancel(void)
{ uint32_t irq=firstboot_irq_save();updating=false;firstboot_irq_restore(irq); }
bool firstboot_update_request(const uint8_t *p,uint16_t len)
{
    if(!p || !len)return false;
    if(p[0]==0)return len==1;
    uint32_t irq=firstboot_irq_save();bool ok=false;
    if(p[0]==1 && len==17 && get32(p+1)==0x42533743u &&
       get32(p+5)==UPDATE_BYTES && get32(p+9)==UPDATE_CRC && get32(p+13)==UPDATE_BASE) {
        updating=true;offset=0;ok=true;
    } else if(firstboot_update_busy()) {
        if(p[0]==2 && len>=6 && len<=64 && get32(p+1)==offset &&
           (uint32_t)(len-5)<=UPDATE_BYTES-offset) { offset+=len-5;ok=true; }
        else if(p[0]==3 && len==5 && get32(p+1)==0x42533743u && offset==UPDATE_BYTES)ok=true;
    }
    if(ok)last_ms=hal_millis();
    firstboot_irq_restore(irq);return ok;
}
bool firstboot_stub_image_ok(const uint8_t *image,uint32_t len)
{ return firstboot_update_busy() && image && len==UPDATE_BYTES && storage_crc32(0,image,len)==UPDATE_CRC; }
#endif
