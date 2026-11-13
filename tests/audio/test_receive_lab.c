/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "p25_lab.h"
#include "native_modem/native_lab.h"
#include "board.h"
#include "phy_stream.h"
#include "selftest.h"
#include "config.h"
#include "debuglink/debuglink_proto.h"
#include "task.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
static void (*task_fn)(void *);
static audio_block_cb_t callback;
static void *callback_ctx;
static jmp_buf done;
static unsigned depth,notifies,turn,clock_ms,audio_at,outputs_nonzero,route_seen,stops,native_owned;
static unsigned native_end,send_live,send_index,send_due,native_limit;
static unsigned route_calls,pace_calls,paced=1,pending_until;
static uint32_t output_crc;
void audio_test_enter(void) { depth++; }
void audio_test_exit(void) { assert(depth);depth--; }
BaseType_t xTaskCreate(void (*fn)(void *),const char *name,unsigned n,void *ctx,unsigned prio,TaskHandle_t *h)
{ (void)name;(void)ctx;assert(n==(CONFIG_P25_BENCH || CONFIG_P25_NATIVE?2048:1024) && prio==3);task_fn=fn;*h=(void *)1;return pdPASS; }
void xTaskNotifyGive(TaskHandle_t h) { assert(h && !depth);notifies++; }
uint32_t ulTaskNotifyTake(BaseType_t clear,TickType_t wait)
{ assert(clear && wait==portMAX_DELAY);if(turn++)longjmp(done,1);return 1; }
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t h) { (void)h;return 512; }
uint32_t hal_micros(void) { return clock_ms*1000u; }
uint32_t hal_millis(void) { return clock_ms; }
void vTaskDelay(TickType_t ms)
{
    clock_ms+=ms;
    while(callback && clock_ms-audio_at>=20) {
        int16_t mic[160]={0},spk[160];audio_at+=20;
        callback(mic,spk,callback_ctx);
        for(unsigned i=0;i<160;i++) {
            if(spk[i]) { outputs_nonzero++;pending_until=clock_ms+40; }
            for(unsigned j=0;j<2;j++) {
                output_crc^=((uint16_t)spk[i]>>(8*j))&255u;
                for(unsigned b=0;b<8;b++)output_crc=(output_crc>>1)^(0xedb88320u&(0u-(output_crc&1u)));
            }
        }
    }
}
static bool audio_start(audio_block_cb_t cb,void *ctx) { callback=cb;callback_ctx=ctx;audio_at=clock_ms;output_crc=0xffffffffu;return true; }
static void audio_stop(void) { assert(clock_ms>=pending_until);callback=NULL;stops++; }
static void route(bool on) { route_seen+=on;route_calls++; }
static bool pending(void) { return clock_ms<pending_until; }
static void pace(bool on) { pace_calls++;assert(on==paced); }
static const hal_audio_t audio={.start=audio_start,.stop=audio_stop,.speaker_pcm=route,.spk_pending=pending,.playback_pace=pace};
const board_t board={.audio=&audio};
int native_lab_reserved(void) { return native_owned; }
uint32_t native_lab_voice_start(uint32_t hz,uint32_t ms) { assert(hz==145500000);native_end=clock_ms+ms;native_owned=1;send_index=0;send_due=clock_ms;return 42; }
int native_lab_voice_poll(uint32_t sid,uint8_t *state,uint8_t *reason) { assert(sid==42);*state=clock_ms>=native_end?NT_STOPPED:NT_RX;*reason=*state==NT_RX?0:native_limit?NT_RX_LIMIT:NT_RX_DURATION;return 1; }
static uint8_t raw[16][36];
int native_lab_voice_read(uint32_t sid,nt_record *r) {
    assert(sid==42);
    if(!send_live || send_index>=16 || clock_ms<send_due)return 0;
    memset(r,0,sizeof *r);r->sequence=send_index;r->time_us=send_due*1000u;
    memcpy(r->raw,raw[send_index++],36);send_due+=30;return 1;
}
void native_lab_voice_stop(uint32_t sid) { assert(sid==42); }
int native_lab_voice_release(uint32_t sid) { assert(sid==42);native_owned=0;return 1; }
/* The live (channel TX) side is not exercised by these receive tests. */
int native_lab_live_poll(uint32_t sid,uint8_t *state,uint8_t *reason,uint8_t *queued) { (void)sid;(void)state;(void)reason;(void)queued;return 0; }
int native_lab_live_block(uint32_t sid,const uint8_t raw[36],int last) { (void)sid;(void)raw;(void)last;return 0; }
int native_lab_live_go(uint32_t sid) { (void)sid;return 0; }
void native_lab_live_summary(uint32_t out[5]) { for (unsigned i=0;i<5;i++) out[i]=0; }
static uint8_t reply[1024];
static unsigned call(const uint8_t *p,unsigned len,unsigned cap) { unsigned n=p25_lab_command(p,len,reply,cap);assert(!depth);return n; }
static uint32_t word(unsigned n) { uint32_t x;memcpy(&x,reply+1+4*n,4);return x; }
static void fixture(void)
{
    static p25_voice v;p25_voice_init(&v,0x293,42,NULL,NULL);
    const p25_call c={0x293,42,123456,0,0,0};assert(p25_voice_tx_begin(&v,&c));
    uint8_t air[576]={0};unsigned at=0;
    for(unsigned f=0;f<18;f++) {
        int16_t pcm[160];
        for(unsigned i=0;i<160;i++) { unsigned phase=(f*160+i)%40;pcm[i]=(int16_t)(phase<20?(int)phase*900-9000:27000-(int)phase*900); }
        assert(p25_voice_tx_pcm(&v,pcm));
        if(f==8 || f==17)for(unsigned n=0;n<(f==8?8u:6u);n++){assert(p25_phy_tx_block(&v,air+at)==144);at+=36;}
    }
    assert(p25_voice_tx_end(&v));assert(p25_phy_tx_block(&v,air+at)==144);at+=36;
    assert(p25_phy_tx_block(&v,air+at)==36);at+=36;assert(at==576);
    for(unsigned i=0;i<16;i++)for(unsigned b=0;b<288;b++) {
        unsigned from=b<24?b:b<72?b+108:b<180?b-48:b;
        raw[i][b/8]|=(uint8_t)(((air[36*i+from/8]>>(7-from%8))&1u)<<(7-b%8));
    }
}
#if CONFIG_P25_BENCH
static void load(void)
{
    uint8_t begin[]={2,16,0};assert(call(begin,3,1024)==1 && !reply[0]);
    for(unsigned i=0;i<16;i++) {
        uint8_t p[40]={3,(uint8_t)i,0,0};memcpy(p+4,raw[i],36);
        if(i==0) {p[3]=3;assert(call(p,40,1024)==1 && reply[0]==DL_ST_BAD_ARG);p[3]=0;}
        assert(call(p,40,1024)==1 && !reply[0]);
    }
}
#endif
static void execute(void) { turn=0;if(!setjmp(done))task_fn(NULL);assert(!p25_lab_busy() && !depth); }
int main(void)
{
    fixture();p25_lab_start();uint8_t status=7;
#if CONFIG_P25_BENCH
    load();
    uint8_t pacing[]={9,2};assert(call(pacing,2,1024)==1 && reply[0]==DL_ST_BAD_ARG);
    pacing[1]=0;assert(call(pacing,2,1024)==1 && !reply[0]);paced=0;
    assert(call(&status,1,128)==1 && reply[0]==DL_ST_BAD_ARG);
    uint8_t run[]={4,0,0};assert(call(run,3,1024)==1 && !reply[0] && p25_lab_busy());
    assert(call(pacing,2,1024)==1 && reply[0]==DL_ST_BUSY);
    assert(call(run,3,1024)==1 && reply[0]==DL_ST_BUSY);execute();
    assert(call(&status,1,1024)==129 && !reply[0]);
    assert(word(2)==2 && word(6)==16 && word(7)==4 && word(8)==0 && word(9)==0);
    assert(word(10)==18 && word(11)==0x4e158373 && word(12)==2715);
    assert(!word(17) && !native_owned);
    load();run[1]=1;run[2]=1;assert(call(run,3,1024)==1 && !reply[0]);execute();
    assert(call(&status,1,1024)==129 && word(2)==2 && word(18)==18 && word(11)==0x4e158373);
    assert(!word(21) && !word(22) && !word(20) && !native_owned);
    assert(outputs_nonzero==2715 && route_seen && stops==1 && !callback && notifies==2);
    assert(route_calls==2 && pace_calls==1); /* one route on/off, no per-block toggles */
    pacing[1]=1;assert(call(pacing,2,1024)==1 && !reply[0]);paced=1;
    send_live=1;
    uint8_t live[]={5,0,0,0,0,0xe8,3,0x93,2,0,0};uint32_t hz=145500000;memcpy(live+1,&hz,4);
    assert(call(live,sizeof live,1024)==1 && !reply[0]);execute();
    assert(call(&status,1,1024)==129 && word(2)==2 && word(6)==16 && word(18)==18);
    assert(word(11)==0x4e158373 && word(25)==NT_STOPPED && word(26)==NT_RX_DURATION);
    assert(!word(21) && !word(22) && !native_owned && stops==2 && outputs_nonzero==5430);
    uint32_t checked_crc=output_crc;printf("PCM_CALLBACK_CRC=%08x\n",checked_crc);
    uint8_t checks[]={12,0};assert(call(checks,2,1024)==1 && !reply[0]);
    assert(call(live,sizeof live,1024)==1 && !reply[0]);execute();
    assert(call(&status,1,1024)==129 && word(2)==2 && word(18)==18 && !word(11) && !word(12));
    assert(!word(19) && outputs_nonzero==8145 && stops==3 && output_crc==checked_crc);
    send_live=0;native_limit=1;live[0]=8;live[5]=0x2c;live[6]=1; /* 300 seconds, no u16 millisecond truncation */
    assert(call(live,sizeof live,1024)==1 && !reply[0]);execute();
    assert(call(&status,1,1024)==129 && word(2)==2 && word(25)==NT_STOPPED && word(26)==NT_RX_LIMIT);
    assert(!word(6) && word(17)>=14999 && !native_owned && stops==4);
    puts("P25 receive lab: bounded RAM replay, real IMBE/CAI golden CRC, paced PCM callbacks, ownership release PASS");
#else
    for(unsigned op=1;op<=4;op++) { uint8_t p=(uint8_t)op;assert(call(&p,1,1024)==1 && reply[0]==DL_ST_UNAVAILABLE); }
    uint8_t p[]={12,1};assert(call(p,2,1024)==1 && reply[0]==DL_ST_UNAVAILABLE);
    uint8_t resource=11;assert(call(&resource,1,1024)>1 && !reply[0]);
    assert(strstr((char *)reply+1,"p25.bench=0\n") && strstr((char *)reply+1,"p25.sample_checks=0\n"));
    send_live=1;
    uint8_t live[]={5,0,0,0,0,0xe8,3,0x93,2,0,0};uint32_t hz=145500000;memcpy(live+1,&hz,4);
    assert(call(live,sizeof live,1024)==1 && !reply[0]);execute();
    assert(call(&status,1,1024)==129 && word(2)==2 && word(18)==18 && !word(11) && !word(12));
    assert(!word(19) && outputs_nonzero==2715 && !word(21) && !word(22) && !native_owned && stops==1);
    printf("PCM_CALLBACK_CRC=%08x\n",output_crc);
    puts("Lean RX: bench commands refused, real live CAI/IMBE audio preserved, no per-sample checks PASS");
#endif
}
