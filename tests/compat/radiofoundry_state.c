/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Run actual upstream calibration/storage readers on a private saved image.
 * Every write/erase callback refuses and counts; the input file stays read-only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "board.h"
#include "storage/storage.h"
#include "baofeng_cal.h"
static unsigned char *data;
static unsigned reads,writes,erases;
static bool rd(uint32_t a,void *out,uint32_t n) {
    if(a>0x1000000 || n>0x1000000-a)return false;
    memcpy(out,data+a,n);reads++;return true;
}
static bool wr(uint32_t a,const void *p,uint32_t n) { (void)a;(void)p;(void)n;writes++;return false; }
static bool er(uint32_t a) { (void)a;erases++;return false; }
static const hal_flash_map_t map={.settings_addr=0x711000,.settings_size=0x8000,.journal_addr=0x719000,
    .openfw_eeprom=0x700000,.openfw_flash=0x720000,.openfw_end=0xf00000,.native_addr=0xf00000,.native_size=0x100000};
static const hal_flash_t flash={.size=0x1000000,.erase_size=4096,.read=rd,.write=wr,.erase=er,.map=&map};
const board_t board={.name="saved-image compatibility audit",.flash=&flash};
void storage_lock(void) {} void storage_unlock(void) {}
void storage_quick_lock(void) {} void storage_quick_unlock(void) {}
int main(int argc,char **argv) {
    if(argc!=2)return 2;
    FILE *f=fopen(argv[1],"rb");if(!f)return 2;
    data=malloc(0x1000000);if(!data)return 2;
    if(fread(data,1,0x1000000,f)!=0x1000000 || fgetc(f)!=EOF)return 2;
    fclose(f);
    bool calibration=bf_cal_load(&flash,0x710000);
    unsigned cal_writes=writes,cal_erases=erases;
    bool storage=storage_init();storage_stats_t st;storage_get_stats(&st);
    bf_cal_power_points_t power={0};bool power_ok=bf_cal_power(446050000,&power);
    printf("{\"factory_calibration_loaded\":%s,\"calibration_source\":\"%s\","
        "\"calibration_write_attempts\":%u,\"calibration_erase_attempts\":%u,"
        "\"storage_ready\":%s,\"storage_region\":%u,\"foreign_slots\":%u,"
        "\"storage_init_write_attempts\":%u,\"storage_init_erase_attempts\":%u,"
        "\"uhf_power_points_present\":%s,\"lowest_drive_within_DAC_range\":%s,\"input_unchanged\":true}\n",
        calibration?"true":"false",bf_cal_source(),cal_writes,cal_erases,storage?"true":"false",
        (unsigned)storage_region(),st.foreign_slots,writes-cal_writes,erases-cal_erases,
        power_ok?"true":"false",power_ok && bf_cal_power_level(446050000,0)<=4095?"true":"false");
    bool ok=calibration && power_ok && !strcmp(bf_cal_source(),"factory") &&
        !storage && storage_region()==STORAGE_REGION_FOREIGN &&
        st.foreign_slots>0 && writes==0 && erases==0 &&
        bf_cal_power_level(446050000,0)<=4095;
    free(data);return ok?0:1;
}
