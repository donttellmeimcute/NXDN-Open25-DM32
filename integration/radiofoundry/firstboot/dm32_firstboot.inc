/* SPDX-License-Identifier: GPL-3.0-or-later */
#if CONFIG_FIRSTBOOT
#include "firstboot/firstboot.h"
bool board_firstboot_pin_level(uint8_t pin,bool requested)
{
    if (requested && (pin==PIN_UHF_PA || pin==PIN_VHF_PA))
        return firstboot_tx_allowed(FB_PA_GPIO) ? true : firstboot_deny(FB_PA_GPIO);
    return requested;
}
#endif
