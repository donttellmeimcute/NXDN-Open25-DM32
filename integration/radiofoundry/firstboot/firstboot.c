/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "firstboot.h"
#include "debuglink/debuglink_proto.h"
#include <limits.h>
static uint32_t denied[FB_EVENT_COUNT];
bool firstboot_deny(enum firstboot_event event)
{
    uint32_t irq=firstboot_irq_save();
    if ((unsigned)event<FB_EVENT_COUNT && denied[event]!=UINT32_MAX) denied[event]++;
    firstboot_irq_restore(irq);
    return false;
}
bool firstboot_debug_allowed(uint8_t command)
{
    switch (command) {
#if defined(CONFIG_DEV_UPDATE) && CONFIG_DEV_UPDATE
    case DL_CMD_STUB: return true;
#endif
    case DL_CMD_PING: case DL_CMD_VERSION: case DL_CMD_GET_DIAG:
    case DL_CMD_SCREENSHOT: case DL_CMD_KEY: case DL_CMD_GET_LOG:
    case DL_CMD_GET_VIEW: case DL_CMD_RADIO: case DL_CMD_INFO_CHIP: case DL_CMD_REBOOT:
    case 0x20: case 0x21: case 0x22: return true;
    default: return firstboot_deny(FB_DEBUG_COMMAND);
    }
}
bool firstboot_reboot_allowed(const uint8_t *req,uint16_t len)
{
    /* Normal reset only. The existing chip-UID token gate still applies. */
    return req && len==5 && req[0]==0 ? true : firstboot_deny(FB_DEBUG_COMMAND);
}
size_t firstboot_command(const uint8_t *req,uint16_t len,uint8_t *out,size_t cap)
{
    if (!cap) return 0;
    out[0]=DL_ST_BAD_ARG;
    if (!req || len!=1 || req[0]!=0 || cap<1+4*FIRSTBOOT_WORDS) return 1;
    uint32_t words[FIRSTBOOT_WORDS]={FIRSTBOOT_VERSION,FIRSTBOOT_FLAGS};
    uint32_t irq=firstboot_irq_save();
    for (unsigned i=0;i<FB_EVENT_COUNT;i++) words[2+i]=denied[i];
    firstboot_irq_restore(irq);
    for (unsigned i=0;i<FIRSTBOOT_WORDS;i++)
        for (unsigned b=0;b<4;b++) out[1+4*i+b]=(uint8_t)(words[i]>>(8*b));
    out[0]=DL_ST_OK;
    return 1+4*FIRSTBOOT_WORDS;
}
