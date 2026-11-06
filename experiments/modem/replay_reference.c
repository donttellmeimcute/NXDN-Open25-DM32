/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Offline oracle for the exact on-device admission/codec/PCM path. No radio. */
#include "phy_stream.h"
#include <stdio.h>
#include <stdint.h>
static FILE *output;
static unsigned blocks,failed;
static void sink(void *ctx,const int16_t pcm[160])
{
    (void)ctx;uint8_t bytes[320];blocks++;
    for(unsigned i=0;i<160;i++) { bytes[2*i]=(uint8_t)pcm[i];bytes[2*i+1]=(uint8_t)((uint16_t)pcm[i]>>8); }
    if(fwrite(bytes,1,sizeof bytes,output)!=sizeof bytes)failed=1;
}
int main(int argc,char **argv)
{
    if(argc!=3)return 2;
    FILE *input=fopen(argv[1],"rb");if(!input)return 2;
    output=fopen(argv[2],"wbx");if(!output) { fclose(input);return 2; }
    static p25_voice voice;p25_phy_rx phy={0};p25_voice_init(&voice,0x293,0,sink,NULL);
    uint8_t record[37];unsigned count=0;size_t n;
    while((n=fread(record,1,sizeof record,input))!=0) {
        if(n!=sizeof record || count>=64 || record[0]>2 || p25_phy_rx_record(&phy,&voice,count,record[0],record+1)) { failed=1;break; }
        count++;
    }
    if(ferror(input))failed=1;
    if(fclose(output))failed=1;
    fclose(input);
    printf("{\"records\":%u,\"cai_frames\":%u,\"muted\":%u,\"erasures\":%u,\"pcm_blocks\":%u}\n",
        count,voice.rx_frames,voice.rx_muted,voice.rx_erasures,blocks);
    return failed || !count;
}
