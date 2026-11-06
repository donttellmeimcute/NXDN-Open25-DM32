/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "firstboot/firstboot.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint32_t now,depth;
uint32_t hal_millis(void) { return now; }
uint32_t firstboot_irq_save(void) { return depth++; }
void firstboot_irq_restore(uint32_t s) { assert(depth==s+1);depth=s; }
uint32_t storage_crc32(uint32_t crc,const void *buf,uint32_t n)
{ const uint8_t *p=buf;crc=~crc;while(n--){crc^=*p++;for(unsigned j=0;j<8;j++)crc=(crc>>1)^(0xedb88320u&(0u-(crc&1u)));}return ~crc; }
static void put(uint8_t *p,uint32_t x) { for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(x>>(8*i)); }
int main(int argc,char **argv)
{
    uint8_t begin[17]={1},data[64]={2},enter[5]={3},info=0;
    put(begin+1,0x42533743);put(begin+5,7784);put(begin+9,0xc57b2bca);put(begin+13,0x44000);put(enter+1,0x42533743);
    assert(!firstboot_update_busy() && firstboot_update_request(&info,1));
    assert(!firstboot_update_request(data,6) && !firstboot_update_request(enter,5));
    begin[5]^=1;assert(!firstboot_update_request(begin,17));begin[5]^=1;
    assert(firstboot_update_request(begin,17) && firstboot_update_busy());
    assert(!firstboot_update_request(enter,5));
    put(data+1,1);assert(!firstboot_update_request(data,64));put(data+1,0);
    assert(firstboot_update_request(data,64));now=4999;assert(firstboot_update_busy());
    now=5000;assert(!firstboot_update_busy() && !firstboot_update_request(data,64));
    assert(firstboot_update_request(begin,17));
    for(unsigned off=0;off<7784;) {
        unsigned n=7784-off<59?7784-off:59;put(data+1,off);
        assert(firstboot_update_request(data,(uint16_t)(n+5)));off+=n;now+=10;
    }
    assert(firstboot_update_request(enter,5));
    assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);
    uint8_t approved[7784];assert(fread(approved,1,sizeof approved,f)==sizeof approved);fclose(f);
    assert(firstboot_stub_image_ok(approved,sizeof approved));
    approved[100]^=1;assert(!firstboot_stub_image_ok(approved,sizeof approved));
    firstboot_update_cancel();assert(!firstboot_update_busy() && !firstboot_update_request(enter,5));
    uint8_t bad[7784]={0};assert(!firstboot_stub_image_ok(bad,sizeof bad));
    now=UINT32_MAX-100;assert(firstboot_update_request(begin,17));now=4899;assert(!firstboot_update_busy());
    assert(!depth);puts("Update gate: exact helper header, ordered bounded chunks, expiry/wrap, wrong-image refusal PASS");
}
