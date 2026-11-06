/* SPDX-License-Identifier: GPL-3.0-or-later */
/* User-authorized development derivative; not an MIT-only upstream submission. */
#include <string.h>
#include "p25_lab.h"
#include "selftest.h"
#include "phy_stream.h"
#include "audio_fifo.h"
#include "cpu_profile.h"
#include "config.h"
#include "board.h"
#include "hal_time.h"
#include "debuglink/debuglink_proto.h"
#include "debuglink/dl_fmt.h"
#include "FreeRTOS.h"
#include "task.h"
#if CONFIG_DEV_TX
#include "input/input.h"
#endif
#if CONFIG_P25_NATIVE
#include "native_modem/native_lab.h"
#endif

static p25_voice voice;
#if CONFIG_P25_BENCH || CONFIG_DEV_TX
static p25_selftest_result result;
#endif
#if CONFIG_P25_BENCH
static uint8_t sample_checks=1;
#else
#define sample_checks 0
#endif
static TaskHandle_t worker;
static uint8_t state; /* 0 idle, 1 pending/running, 2 passed/finished, 3 failed, 4 no worker */
static uint8_t request_mode, play_requested, delay_seconds;
static volatile uint8_t stop_requested;
static uint32_t stack_free;
#if CONFIG_P25_BENCH
static struct { uint8_t kind,raw[36]; } replay[64];
#endif
static uint16_t expected,loaded,nac,group;
static uint32_t frequency,duration_ms;
static p25_audio_fifo fifo;
static uint32_t records,pcm_blocks,pcm_crc,nonzero,record_max_us,decode_us;
static uint32_t callbacks,played,played_nonzero,session,gaps,unsupported;
static uint8_t native_state,native_reason,audio_on;
static uint8_t pcm_paced=1;
static p25_phy_rx phy;
#if CONFIG_DEV_TX
static uint32_t live_sid;
static volatile uint8_t live_released,live_capturing;
static uint32_t live_mic_frames,live_peak,live_encode_max,live_encode_us,live_slow;
static uint32_t live_first_us,live_last_us,live_encoded;
static uint16_t live_fifo_high;
static uint32_t live_overflows;

int p25_lab_live_begin(uint32_t sid)
{
    taskENTER_CRITICAL();
    int ok=worker && state!=1 && board.native_mic_route && board.audio &&
           board.audio->start && board.audio->stop;
    if (ok) {
        live_sid=sid;
        live_released=live_capturing=stop_requested=0;
        request_mode=4;
        state=1;
    }
    taskEXIT_CRITICAL();
    if (ok)xTaskNotifyGive(worker);
    return ok;
}
void p25_lab_live_up(void) { live_released=1; }

int p25_lab_channel_rx(uint32_t hz)
{
    taskENTER_CRITICAL();
    int ok=worker && state!=1;
    if (ok) {
        frequency=hz;nac=0x293;group=1;duration_ms=0;
        request_mode=5;play_requested=1;delay_seconds=0;stop_requested=0;
        state=1;
    }
    taskEXIT_CRITICAL();
    if (ok)xTaskNotifyGive(worker);
    return ok;
}

static void mic_block(const int16_t *mic,int16_t *spk,void *ctx)
{
    (void)ctx;
    memset(spk,0,320);
    taskENTER_CRITICAL();
    if (live_capturing && !live_released) {
        if (!p25_audio_fifo_push(&fifo,mic)) stop_requested=1;
        else {
            live_last_us=hal_micros();
            if (!live_mic_frames++) live_first_us=live_last_us;
        }
    }
    taskEXIT_CRITICAL();
}

static unsigned queued_symbols(void)
{
    unsigned symbols=0;
    for (unsigned i=0;i<voice.count;i++)
        symbols+=voice.queue[(voice.tail+i)%P25_VOICE_QUEUE].bytes*4u;
    return symbols-voice.packet_symbol;
}

static int live_run(void)
{
    const p25_call call={0x293,1,1,0,0,0};
    int16_t pcm[160];
    uint8_t raw[36],queued=0;
    unsigned lead=3;
    bool kicked=false,sealed=false,closed=false;
    bool ok=p25_voice_tx_begin(&voice,&call)!=0;
    uint32_t sid=live_sid;
    live_mic_frames=live_peak=live_encode_max=live_encode_us=live_slow=live_encoded=0;
    live_first_us=live_last_us=live_fifo_high=live_overflows=0;
    native_state=NT_ARMED;
    native_reason=NT_OK;
    p25_audio_fifo_reset(&fifo);
    if (ok) {
        board.native_mic_route(true);
        live_capturing=1;
        ok=board.audio->start(mic_block,NULL);
    }
    uint32_t last_input=hal_millis();
    while (ok && !stop_requested) {
        if (!native_lab_live_poll(sid,&native_state,&native_reason,&queued) || native_state==NT_FAULT) { ok=false;break; }
        if (native_state==NT_STOPPED) { ok=sealed && native_reason==NT_TX_COMPLETE;break; }
        bool closing=live_released || !input_physical_ptt();
        if (closing && live_capturing) {
            live_capturing=0;
            board.audio->stop();
        }
        while (!sealed && queued<NT_TX_CAP && (voice.started || closed)) {
            if (lead) {
                memset(raw,0x77,sizeof raw);
                lead--;
            } else if (closed && p25_voice_tx_done(&voice)) {
                /* Keep the PA through the final CAI unit and hardware pipeline. */
                memset(raw,0x77,sizeof raw);
                sealed=true;
            } else if (closed || queued_symbols()>=P25_PHY_SYMBOLS) {
                if (!p25_phy_tx_block(&voice,raw)) {
                    if (voice.fault) ok=false;
                    break;
                }
            } else break;
            if (!native_lab_live_block(sid,raw,sealed)) { ok=false;break; }
            queued++;
        }
        if (!ok) break;
        if (!kicked && (queued>=7 || sealed)) {
            kicked=native_lab_live_go(sid)!=0;
            if (!kicked) { ok=false;break; }
        }
        if (!closed && (voice.count<P25_VOICE_QUEUE || voice.voice_count<8)) {
            taskENTER_CRITICAL();
            int have=p25_audio_fifo_take(&fifo,pcm);
            taskEXIT_CRITICAL();
            if (have) {
                last_input=hal_millis();
                for (unsigned i=0;i<160;i++) {
                    int sample=pcm[i];
                    unsigned magnitude=(unsigned)(sample<0?-sample:sample);
                    if (magnitude>live_peak) live_peak=magnitude;
                }
            } else if (closing && voice.voice_count) {
                memset(pcm,0,sizeof pcm);
                have=1;
            } else if (closing) {
                ok=p25_voice_tx_end(&voice)!=0;
                if (p25_voice_tx_done(&voice)) break; /* an empty PTT */
                closed=true;
                continue;
            }
            if (have) {
                uint32_t at=hal_micros();
                ok=p25_voice_tx_pcm(&voice,pcm)!=0;
                uint32_t elapsed=hal_micros()-at;
                live_encode_us+=elapsed;
                live_encoded++;
                if (elapsed>live_encode_max) live_encode_max=elapsed;
                if (elapsed>20000u) live_slow++;
                /* Give the idle watchdog time to run even while catching up. */
                vTaskDelay(1);
                continue;
            }
        }
        if (!closing && hal_millis()-last_input>300u) { ok=false;break; }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    live_capturing=0;
    native_lab_voice_stop(sid);
    board.audio->stop();
    board.native_mic_route(false);
    uint32_t end=hal_millis()+500;
    int released;
    while (!(released=native_lab_voice_release(sid)) && (int32_t)(hal_millis()-end)<0)
        vTaskDelay(pdMS_TO_TICKS(2));
    live_fifo_high=fifo.high_water;live_overflows=fifo.overflows;
    p25_voice_tx_abort(&voice);
    return ok && !stop_requested && !live_overflows && released;
}
#endif
int p25_lab_busy(void) { return state==1; }
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0]|p[1]<<8); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
/* Empty RX polling may wait 5 ms: records arrive every 30 ms and the
 * independent 500 ms lease/fault timer still runs. Bench timing stays at 1 ms. */
static void relax(void) { vTaskDelay(pdMS_TO_TICKS(CONFIG_P25_BENCH?1:5)); }

/* Only the worker decodes; only the audio task consumes the fixed PCM FIFO. */
#if CONFIG_P25_NATIVE
static void audio_block(const int16_t *mic,int16_t *spk,void *ctx)
{
    (void)mic;(void)ctx;
    taskENTER_CRITICAL();
    callbacks++;
    int ok=p25_audio_fifo_pop(&fifo,spk);
    if(ok) {
        played++;
        if(sample_checks)for(unsigned i=0;i<160;i++)played_nonzero+=spk[i]!=0;
    }
    taskEXIT_CRITICAL();
    /* Keep the digital route through silence and buffering. Toggling it inside
     * this callback empties the driver's queue and discards the current block. */
}
#endif
static void sink(void *ctx,const int16_t pcm[160])
{
    (void)ctx;
    pcm_blocks++;
    if(sample_checks)for(unsigned i=0;i<160;i++) {
        nonzero+=pcm[i]!=0;
        for(unsigned j=0;j<2;j++) {
            pcm_crc^=((uint16_t)pcm[i]>>(8*j))&255u;
            for(unsigned b=0;b<8;b++)pcm_crc=(pcm_crc>>1)^(0xedb88320u&(0u-(pcm_crc&1u)));
        }
    }
    if(audio_on) {
        taskENTER_CRITICAL();
        if(!p25_audio_fifo_push(&fifo,pcm))stop_requested=1;
        taskEXIT_CRITICAL();
    }
}
#if CONFIG_P25_NATIVE
static void reset_stream(void)
{
    p25_voice_rx_reset(&voice);
    taskENTER_CRITICAL();fifo.head=fifo.tail=fifo.count=fifo.playing=0;taskEXIT_CRITICAL();
    memset(&phy,0,sizeof phy);
}
#endif
static void consume(uint32_t seq,uint8_t kind,const uint8_t raw[36])
{
    uint32_t at=hal_micros();
    uint32_t prior_gaps=phy.gaps;
    uint32_t prior_frames=voice.rx_frames;
    if(p25_phy_rx_record(&phy,&voice,seq,kind,raw)) {
        gaps+=phy.gaps-prior_gaps;unsupported=phy.unsupported;
        if(unsupported)stop_requested=1;
        taskENTER_CRITICAL();fifo.head=fifo.tail=fifo.count=fifo.playing=0;taskEXIT_CRITICAL();
    }
    if(voice.rx_frames!=prior_frames) {
        taskENTER_CRITICAL();
        fifo.draining=voice.framer.nid.duid==P25_DUID_TDU || voice.framer.nid.duid==P25_DUID_TDULC;
        taskEXIT_CRITICAL();
    }
    uint32_t dt=hal_micros()-at;
    decode_us+=dt;if(dt>record_max_us)record_max_us=dt;
    records++;
}
#if CONFIG_P25_NATIVE
static int start_audio(void)
{
    if(!board.audio || !board.audio->start || !board.audio->stop || !board.audio->speaker_pcm)return 0;
    audio_on=1;
    if(board.audio->playback_pace)board.audio->playback_pace(pcm_paced!=0);
    board.audio->speaker_pcm(true);
    if(board.audio->start(audio_block,NULL))return 1;
    audio_on=0;board.audio->stop();board.audio->speaker_pcm(false);return 0;
}
#endif
static void stop_audio(void)
{
    if(!audio_on)return;
    board.audio->stop();board.audio->speaker_pcm(false);audio_on=0;
}
#if CONFIG_P25_NATIVE
static int wait_native(void)
{
    uint32_t end=hal_millis()+1000;
    do {
        if(!native_lab_voice_poll(session,&native_state,&native_reason))return 0;
        if(native_state==NT_RX)return 1;
        if(native_state==NT_STOPPED || native_state==NT_FAULT)return 0;
        relax();
    } while((int32_t)(hal_millis()-end)<0 && !stop_requested);
    return 0;
}
#endif
static int receive_run(void)
{
    records=pcm_blocks=nonzero=record_max_us=decode_us=callbacks=played=played_nonzero=0;
    session=gaps=unsupported=0;native_state=native_reason=0;pcm_crc=sample_checks?0xffffffffu:0;
    memset(&phy,0,sizeof phy);p25_audio_fifo_reset(&fifo);
    p25_voice_init(&voice,nac,group,sink,NULL);voice.micros=sample_checks?hal_micros:NULL;
    int ok=1;
#if CONFIG_P25_NATIVE
    /* Playback also borrows native RX ownership to exclude normal beeps/PTT and
     * radio retunes. Replayed bytes remain in RAM and never enter a TX path. */
    if(request_mode==3 || request_mode==5 || play_requested) {
        session=native_lab_voice_start(frequency,duration_ms);
        if(!session || !wait_native())ok=0;
        if(ok && play_requested && !start_audio())ok=0;
    }
#else
    if(request_mode==3 || play_requested)ok=0;
#endif
    uint32_t due=hal_millis()+delay_seconds*1000u;
#if CONFIG_P25_BENCH
    uint16_t index=0;
#endif
#if CONFIG_P25_NATIVE
    uint32_t last_tick=0,last_record_at=hal_millis();
    int silent_reset=0;
#endif
    while(ok && !stop_requested) {
#if CONFIG_P25_NATIVE
        if(session) {
            if(!native_lab_voice_poll(session,&native_state,&native_reason)) { ok=0;break; }
            if(native_state!=NT_RX) {
                ok=native_state==NT_STOPPED && (native_reason==NT_RX_DURATION || native_reason==NT_RX_LIMIT ||
                    (request_mode==5 && native_reason==NT_USER_STOP));
                break;
            }
            if(request_mode==2) { nt_record discard;while(native_lab_voice_read(session,&discard)) {} }
        }
#endif
        if((int32_t)(hal_millis()-due)<0) { relax();continue; }
        if(request_mode==2) {
#if CONFIG_P25_BENCH
            if(index==loaded)break;
            consume(index,replay[index].kind,replay[index].raw);index++;
            if(play_requested)due+=30; /* a hardware block's real arrival cadence */
#else
            ok=0;break;
#endif
        } else {
#if CONFIG_P25_NATIVE
            nt_record r;
            if(native_lab_voice_read(session,&r)) {
                uint32_t delta=r.time_us-last_tick;
                if(last_tick && (delta>45000u || delta<15000u)) { gaps++;reset_stream(); }
                last_tick=r.time_us;last_record_at=hal_millis();silent_reset=0;
                consume(r.sequence,(uint8_t)(r.rx_info&3u),r.raw);
                if (request_mode==5) vTaskDelay(1);
            } else {
                if(!silent_reset && hal_millis()-last_record_at>500) {
                    reset_stream();silent_reset=1;
                }
                relax();
            }
#else
            ok=0;
#endif
        }
    }
    if(request_mode==2 && audio_on && ok && !stop_requested) {
        taskENTER_CRITICAL();fifo.draining=1;taskEXIT_CRITICAL();
        uint32_t end=hal_millis()+2000;
        while((fifo.count || (board.audio->spk_pending && board.audio->spk_pending())) &&
              (int32_t)(hal_millis()-end)<0 && !stop_requested) {
#if CONFIG_P25_NATIVE
            if(session && (!native_lab_voice_poll(session,&native_state,&native_reason) || native_state!=NT_RX)) { ok=0;break; }
#endif
            relax();
        }
        if(fifo.count || (board.audio->spk_pending && board.audio->spk_pending()))ok=0;
    }
    stop_audio();
#if CONFIG_P25_NATIVE
    if(session) {
        native_lab_voice_stop(session);
        uint32_t end=hal_millis()+500;
        int released;
        while(!(released=native_lab_voice_release(session)) && (int32_t)(hal_millis()-end)<0)relax();
        if(!released)ok=0;
    }
#endif
    if(sample_checks)pcm_crc^=0xffffffffu;
    return ok && !fifo.overflows && !unsupported;
}
static void run(void *ctx)
{
    (void)ctx;
    for(;;) {
        ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        int ok;
#if CONFIG_P25_BENCH || CONFIG_DEV_TX
        if(request_mode==1) {
            p25_selftest_result r;
            p25_selftest_run(&voice,&r,hal_micros,relax);
            taskENTER_CRITICAL();result=r;taskEXIT_CRITICAL();ok=r.ok;
        } else
#endif
#if CONFIG_DEV_TX
        if(request_mode==4) ok=live_run(); else
#endif
        ok=receive_run();
        uint32_t free_bytes=(uint32_t)uxTaskGetStackHighWaterMark(NULL)*sizeof(StackType_t);
        taskENTER_CRITICAL();stack_free=free_bytes;state=ok?2:3;taskEXIT_CRITICAL();
    }
}
#if CONFIG_P25_BENCH || CONFIG_DEV_TX
#define P25_WORKER_WORDS 2048 /* synthetic encoder is the deepest path */
#else
#define P25_WORKER_WORDS 1024 /* RX: static bound 2368 B; >1.5 KiB margin */
#endif
void p25_lab_start(void)
{
    if(xTaskCreate(run,"p25-lab",P25_WORKER_WORDS,NULL,3,&worker)!=pdPASS) { worker=NULL;state=4; }
}
/* Private 0x20: diagnostics, bounded RAM capture replay and RX audio.
 * Live TX can only start from the local PTT path. */
size_t p25_lab_command(const uint8_t *req,uint16_t len,uint8_t *out,size_t cap)
{
    if(!cap)return 0;
    out[0]=DL_ST_BAD_ARG;
    if(!req || !len)return 1;
#if CONFIG_DEV_TX
    if (req[0]==13 && len==1) {
        out[0]=DL_ST_OK;
        dl_text_t t;dl_text_init(&t,(char *)out+1,cap-1);
        uint32_t tx[5];native_lab_live_summary(tx);
        taskENTER_CRITICAL();
        dl_text(&t,"live.calls=%u\nlive.reason=%u\nlive.tx_blocks=%u\nlive.pa_keys=%u\nlive.faults=%u\n",
            (unsigned)tx[0],(unsigned)tx[1],(unsigned)tx[2],(unsigned)tx[3],(unsigned)tx[4]);
        dl_text(&t,"live.mic_frames=%u\nlive.mic_span_us=%u\nlive.peak=%u\nlive.encode_max_us=%u\n"
            "live.encoded=%u\nlive.encode_us=%u\nlive.encode_slow=%u\nlive.fifo_high=%u\nlive.overflows=%u\n",
            (unsigned)live_mic_frames,(unsigned)(live_last_us-live_first_us),(unsigned)live_peak,
            (unsigned)live_encode_max,(unsigned)live_encoded,(unsigned)live_encode_us,(unsigned)live_slow,
            (unsigned)live_fifo_high,(unsigned)live_overflows);
        taskEXIT_CRITICAL();
        return t.len+1;
    }
#endif
    if(req[0]==11 && len==1) {
        out[0]=DL_ST_OK;dl_text_t t;dl_text_init(&t,(char *)out+1,cap-1);
        dl_text(&t,"resource.version=1\np25.bench=%u\np25.sample_checks=%u\np25.context_bytes=%u\np25.context_used_bytes=%u\n",
            (unsigned)CONFIG_P25_BENCH,(unsigned)sample_checks,(unsigned)sizeof voice.codec,(unsigned)dm32_imbe_state_bytes());
        dl_text(&t,"p25.empty_poll_ms=%u\n",CONFIG_P25_BENCH?1u:5u);
        p25_cpu_report(&t);return t.len+1;
    }
    if(req[0]==10 && len==1) {
        if(cap<65)return 1;
        if(!board.audio || !board.audio->playback_stats) { out[0]=DL_ST_UNAVAILABLE;return 1; }
        uint32_t w[16];board.audio->playback_stats(w);
        for(unsigned i=0;i<16;i++)for(unsigned j=0;j<4;j++)out[1+4*i+j]=(uint8_t)(w[i]>>(8*j));
        out[0]=DL_ST_OK;return 65;
    }
    if(req[0]==7 && len==1) {
        if(cap<129)return 1;
        uint32_t w[32];
        taskENTER_CRITICAL();
        uint32_t values[32]={1,request_mode,state,loaded,expected,play_requested,
            records,voice.rx_frames,voice.rx_muted,voice.rx_erasures,pcm_blocks,pcm_crc,nonzero,
            voice.fec_max_us,voice.imbe_max_us,record_max_us,decode_us,callbacks,played,played_nonzero,
            fifo.count,fifo.underruns,fifo.overflows,fifo.high_water,session,native_state,native_reason,
            gaps,unsupported,stack_free,voice.framer.bad_nids,voice.framer.truncated_frames};
        memcpy(w,values,sizeof w);taskEXIT_CRITICAL();
        for(unsigned i=0;i<32;i++)for(unsigned j=0;j<4;j++)out[1+4*i+j]=(uint8_t)(w[i]>>(8*j));
        out[0]=DL_ST_OK;return 129;
    }
    if(req[0]==6 && len==1) { stop_requested=1;out[0]=DL_ST_OK;return 1; }
    if(req[0]==0 && len==1) {
        uint8_t s;uint32_t free;
        taskENTER_CRITICAL();s=state;free=stack_free;taskEXIT_CRITICAL();
        out[0]=DL_ST_OK;dl_text_t t;dl_text_init(&t,(char *)out+1,cap-1);
        dl_text(&t,"p25.lab=%u\np25.state=%u\np25.hardware_verified=0\np25.rf_mode_available=0\n",(unsigned)CONFIG_P25_BENCH,s);
#if CONFIG_P25_BENCH || CONFIG_DEV_TX
        p25_selftest_result r;
        taskENTER_CRITICAL();r=result;taskEXIT_CRITICAL();
        dl_text(&t,"p25.ok=%u\np25.air_bytes=%u\np25.pcm_blocks=%u\np25.pcm_crc=%08x\n",
            (unsigned)r.ok,(unsigned)r.air_bytes,(unsigned)r.pcm_blocks,(unsigned)r.pcm_crc);
        dl_text(&t,"p25.nonzero=%u\np25.tx_block_max_us=%u\np25.rx_unit_max_us=%u\n",
            (unsigned)r.nonzero_samples,(unsigned)r.tx_block_max_us,(unsigned)r.rx_unit_max_us);
        dl_text(&t,"p25.tx_first_us=%u\np25.tx_total_us=%u\np25.tx_over_20ms=%u\n",
            (unsigned)r.tx_first_us,(unsigned)r.tx_total_us,(unsigned)r.tx_over_20ms);
#endif
        dl_text(&t,"p25.session_bytes=%u\np25.stack_free=%u\n",(unsigned)sizeof voice,(unsigned)free);
        return t.len+1;
    }
#if !CONFIG_P25_BENCH
    if((req[0]>=(CONFIG_DEV_TX?2:1) && req[0]<=4) || req[0]==9 || req[0]==12) { out[0]=DL_ST_UNAVAILABLE;return 1; }
#endif
    taskENTER_CRITICAL();
    int busy=state==1;
#if CONFIG_P25_NATIVE
    busy=busy || native_lab_reserved();
#endif
    if(busy || !worker) {
        out[0]=worker?DL_ST_BUSY:DL_ST_UNAVAILABLE;taskEXIT_CRITICAL();return 1;
    }
    int start=0;
    if(0) { }
#if CONFIG_P25_BENCH || CONFIG_DEV_TX
    else if(req[0]==1 && len==1) { request_mode=1;start=1; }
#endif
#if CONFIG_P25_BENCH
    else if(req[0]==12 && len==2 && req[1]<=1) { sample_checks=req[1];out[0]=DL_ST_OK; }
    else if(req[0]==9 && len==2 && req[1]<=1) { pcm_paced=req[1];out[0]=DL_ST_OK; }
    else if(req[0]==2 && len==3 && u16(req+1)>=1 && u16(req+1)<=64) {
        expected=u16(req+1);loaded=0;out[0]=DL_ST_OK;
    } else if(req[0]==3 && len==40 && loaded<expected && u16(req+1)==loaded && req[3]<=2) {
        replay[loaded].kind=req[3];memcpy(replay[loaded].raw,req+4,36);loaded++;out[0]=DL_ST_OK;
    }
    else if(req[0]==4 && len==3 && loaded && loaded==expected && req[1]<=1 && req[2]<=15) {
        request_mode=2;play_requested=req[1];delay_seconds=req[2];frequency=145500000;
        duration_ms=60000;nac=0x293;group=0;start=1;
    }
#endif
    else if((req[0]==5 || req[0]==8) && len==11 && u32(req+1) &&
              (req[0]==5?(u16(req+5)>=1000 && u16(req+5)<=60000):(u16(req+5)>=1 && u16(req+5)<=300)) && u16(req+7)<=0xfff) {
#if CONFIG_P25_NATIVE
        request_mode=3;play_requested=1;delay_seconds=0;frequency=u32(req+1);
        duration_ms=(uint32_t)u16(req+5)*(req[0]==8?1000u:1u);nac=u16(req+7);group=u16(req+9);start=1;
#else
        out[0]=DL_ST_UNAVAILABLE;
#endif
    }
    if(start) { state=1;stop_requested=0;out[0]=DL_ST_OK; }
    taskEXIT_CRITICAL();
    if(start)xTaskNotifyGive(worker);
    return 1;
}
