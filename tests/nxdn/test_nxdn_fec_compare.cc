// SPDX-License-Identifier: GPL-3.0-or-later
// Correction rate of src/nxdn against MMDVMHost for SACCH and FACCH1 with random bit errors (docs/nxdn-modem.md)
#include "NXDNFACCH1.h"
#include "NXDNSACCH.h"
extern "C" {
#include "nxdn.h"
}
#include <cstdio>
#include <cstring>
static unsigned rnd=12345; static unsigned nx(){rnd^=rnd<<13;rnd^=rnd>>17;rnd^=rnd<<5;return rnd;}
int main(){
 for (unsigned errs=0; errs<=4; errs++){
  unsigned ours_ok=0, mm_ok=0, n=2000;
  for(unsigned it=0;it<n;it++){
   uint8_t l3[10]; for(int i=0;i<10;i++) l3[i]=nx();
   uint8_t f[48]={0}; nxdn_facch1_encode(l3,f,0);
   for(unsigned e=0;e<errs;e++){unsigned k=96+nx()%144; f[k>>3]^=0x80>>(k&7);}
   uint8_t b[10]; bool ok=nxdn_facch1_decode(f,0,b,NULL)&&!memcmp(b,l3,10);
   CNXDNFACCH1 m; bool mok=m.decode(f,96); uint8_t mb[10]; if(mok){m.getData(mb); mok=!memcmp(mb,l3,10);}
   ours_ok+=ok; mm_ok+=mok;
  }
  printf("FACCH1 %u errors: ours %u/%u mmdvm %u/%u\n",errs,ours_ok,n,mm_ok,n);
 }
 for (unsigned errs=0; errs<=4; errs++){
  unsigned ours_ok=0, mm_ok=0, n=2000;
  for(unsigned it=0;it<n;it++){
   nxdn_sacch s={ (uint8_t)(nx()&63),(uint8_t)(nx()&3), nx()&0x3FFFF};
   uint8_t f[48]={0}; nxdn_sacch_encode(&s,f);
   for(unsigned e=0;e<errs;e++){unsigned k=36+nx()%60; f[k>>3]^=0x80>>(k&7);}
   nxdn_sacch g; bool ok=nxdn_sacch_decode(f,&g,NULL)&&g.data==s.data&&g.ran==s.ran;
   CNXDNSACCH m; bool mok=m.decode(f)&&m.getRAN()==s.ran;
   ours_ok+=ok; mm_ok+=mok;
  }
  printf("SACCH %u errors: ours %u/%u mmdvm %u/%u\n",errs,ours_ok,n,mm_ok,n);
 }
}
