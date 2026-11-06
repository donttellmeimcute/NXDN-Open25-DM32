/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Reuse the real native-lab fixtures; only the updater lease is a double. */
#define main native_fixture_main
#include "test_lab.c"
#undef main
static bool update_active;
bool firstboot_update_busy(void) { return update_active; }
void firstboot_update_cancel(void) { update_active=false; }
int main(void)
{
    fresh();update_active=true;
    assert(native_lab_reserved() && native_lab_owns());
    assert(!native_lab_voice_start(145500000,60000));
    uint8_t p[17]={1,1,0,3,0,0xd0,7,250,0};put(p+9,145500000);
    assert(call(p,17,512)==1 && reply[0]==DL_ST_BUSY);
    radio_req_t req={.type=RREQ_PTT,.arg=1};assert(native_lab_request(&sm,&req));
    native_lab_service(&sm,0);assert(!starts && !offs);
    req.type=RREQ_POWER_OFF;assert(!native_lab_request(&sm,&req));
    assert(!update_active && !native_lab_reserved() && !native_lab_owns());
    puts("Updater ownership: native/RX/PTT excluded, physical power-off cancels, no hardware starts PASS");
}
