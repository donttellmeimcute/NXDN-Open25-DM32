/* SPDX-License-Identifier: GPL-3.0-or-later */
/* radio_nxdn.c (NXDN channels in the radio) with RadioFoundry's real voice
 * bridge (core/dmr/ambe/ambeVoice.c, ambeFec.c) and a fake board: its radio,
 * flat audio tap, PCM stream and vocoder. The test plays the radio's tasks:
 * the radio tick every 10 ms, the audio block every 20 ms, the tap's timer
 * interrupt every 2 ms, and the FM RX RAM delivering 80 samples per 10 ms.
 * Built against the generated RadioFoundry tree by tools/test_nxdn.py. */
#include "radio_nxdn.h"
#include "board.h"
#include "dmr/ambe/ambeFec.h"
#include "dmr/ambe/ambeVoice.h"
#include "native_test.h"
#include "nxdn_link.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ---- fake board ---- */
static struct {
    int rx_calls, tx_calls, tx_off_calls, flat_on, flat_off, inject_on, inject_off, starts, stops, spk_on;
    uint32_t rx_hz, tx_hz;
    uint8_t power;
    bool refuse_tx, transmitting, flat;
    audio_tap_pull_t pull;
    audio_block_cb_t cb;
    void *cb_ctx;
} hw;

static bool f_rx(uint32_t hz, radio_bw_t bw, const subtone_t *tone)
{
    assert(bw == RADIO_BW_12K5 && tone == NULL && !hw.flat);
    hw.rx_calls++;
    hw.rx_hz = hz;
    hw.transmitting = false;
    return true;
}
static bool f_tx(uint32_t hz, radio_bw_t bw, uint8_t power, const subtone_t *tone)
{
    assert(bw == RADIO_BW_12K5 && tone == NULL && !hw.flat);
    hw.tx_calls++;
    if (hw.refuse_tx) return false;
    hw.tx_hz = hz;
    hw.power = power;
    hw.transmitting = true;
    return true;
}
static void f_tx_off(void)
{
    assert(hw.pull == NULL);   /* the samples stop before the carrier */
    hw.tx_off_calls++;
    hw.transmitting = false;
}
static const hal_radio_t radio = { .rx = f_rx, .tx = f_tx, .tx_off = f_tx_off };

/* the receive audio: what the test puts in, as the RX RAM halves would */
enum { AIR_MAX = 8000 * 20 };
static int16_t rx_air[AIR_MAX];
static size_t rx_have, rx_pos;
static bool f_flat(bool on)
{
    if (on) {
        assert(!hw.transmitting && !hw.flat);
        hw.flat_on++;
    } else {
        hw.flat_off++;
    }
    hw.flat = on;
    return true;
}
static uint16_t f_read(int16_t *buf, uint16_t max)
{
    assert(hw.flat);
    uint16_t n = 0;
    while (n < max && rx_pos < rx_have) buf[n++] = rx_air[rx_pos++];
    return n;
}
static bool f_inject(audio_tap_pull_t pull, audio_tap_mode_t mode)
{
    assert(mode == AUDIO_TAP_FLAT);
    if (pull) {
        assert(hw.transmitting && !hw.flat);
        hw.inject_on++;
    } else {
        hw.inject_off++;
    }
    hw.pull = pull;
    return true;
}
static const hal_audio_tap_t tap = {
    .caps = AUDIO_TAP_FM_TX_FLAT | AUDIO_TAP_FM_RX_FLAT,
    .fm_tx_inject = f_inject,
    .fm_tx_nominal_peak = 1500 * 8,   /* 8 LSB per Hz */
    .fm_rx_flat = f_flat,
    .fm_rx_read = f_read,
};

static bool a_start(audio_block_cb_t cb, void *ctx)
{
    hw.starts++;
    hw.cb = cb;
    hw.cb_ctx = ctx;
    return true;
}
static void a_stop(void) { hw.stops++; hw.cb = NULL; }
static void a_spk(bool on) { hw.spk_on = on; }
static const hal_audio_t audio = { .start = a_start, .stop = a_stop, .speaker_pcm = a_spk };

/* The vocoder: encode makes a known parameter sequence, decode records what it is given */
enum { MAXP = 600 };
static uint8_t enc[MAXP][49], dec[MAXP][49];
static unsigned n_enc, n_dec;
static uint32_t rnd = 0xBADC0DEu;
static uint32_t next_rnd(void) { rnd ^= rnd << 13; rnd ^= rnd >> 17; rnd ^= rnd << 5; return rnd; }
static bool v_present(void) { return true; }
static void v_reset(void) {}
static void v_encode(const int16_t pcm[VOCODER_FRAME_SAMPLES], uint8_t bits[VOCODER_BITS])
{
    (void)pcm;
    assert(n_enc < MAXP);
    for (unsigned i = 0; i < 49; i++) bits[i] = enc[n_enc][i] = next_rnd() & 1;
    n_enc++;
}
static void v_decode(const uint8_t bits[VOCODER_BITS], int16_t pcm[VOCODER_FRAME_SAMPLES])
{
    if (n_dec < MAXP) memcpy(dec[n_dec++], bits, 49);
    memset(pcm, 0, VOCODER_FRAME_SAMPLES * sizeof pcm[0]);
}
static const hal_vocoder_t vocoder = { .present = v_present, .reset = v_reset, .encode = v_encode, .decode = v_decode };

const board_t board = { .radio = &radio, .audio = &audio, .vocoder = &vocoder, .audio_tap = &tap };

/* ---- the radio's clock ---- */
static uint32_t now_ms;
static size_t call_end;   /* the end of the call the test put into rx_air: silence after it */
static int16_t tx_air[AIR_MAX];
static size_t tx_have;
static void step_ms(unsigned ms)
{
    for (unsigned k = 0; k < ms; k++) {
        now_ms++;
        if (now_ms % 2 == 0 && hw.pull) {
            assert(tx_have + 16 <= AIR_MAX);
            hw.pull(tx_air + tx_have, 16);   /* the tap's timer interrupt */
            tx_have += 16;
        }
        if (now_ms % 10 == 0) {
            /* the FM RX RAM's samples of the last 10 ms reach the board's ring */
            if (rx_have + 80 <= AIR_MAX) {
                if (rx_have >= call_end) memset(rx_air + rx_have, 0, 80 * sizeof rx_air[0]);
                rx_have += 80;
            }
            radio_nxdn_service(now_ms);       /* the radio tick */
        }
        if (now_ms % 20 == 0 && hw.cb) {
            static int16_t mic[160], spk[160];
            hw.cb(mic, spk, hw.cb_ctx);       /* the audio task's block */
        }
    }
}

/* Another radio's transmission: our own link with known AMBE+2 parameters */
static uint8_t sent[MAXP][49];
static unsigned n_sent;
static bool src_next(void *ctx, uint8_t slot[9])
{
    unsigned *left = ctx;
    if (!*left) return false;
    (*left)--;
    for (unsigned i = 0; i < 49; i++) sent[n_sent][i] = next_rnd() & 1;
    ambeFecEncode(sent[n_sent++], slot);
    return true;
}

static void make_call(size_t at, unsigned slots, uint8_t ran, uint16_t src, uint16_t group)
{
    static nxdn_link far;
    static const nxdn_audio_path flat = { 0, 0, 0, 2048 };
    nxdn_link_init(&far, &flat, 0, 0, 0, NULL, NULL);
    nxdn_tx_cfg c = { ran, src, group, false, false, NXDN_DIR_INBOUND };
    nxdn_link_tx_begin(&far, &c);
    unsigned left = slots;
    size_t n = at;
    for (;;) {
        nxdn_link_tx_fill(&far, src_next, &left);
        if (!left) nxdn_link_tx_end(&far);
        int16_t s[80];
        uint16_t got = nxdn_link_pull(&far, s, 80);
        for (uint16_t i = 0; i < got && n < AIR_MAX; i++) rx_air[n++] = s[i];
        if (nxdn_link_tx_done(&far)) break;
    }
    call_end = n;
}

static bool silence_or(const uint8_t *bits)
{
    uint8_t sil[49];
    ambe_fec_result_t r;
    ambeFecDecode(nxdn_silence_slot, sil, &r);
    return memcmp(bits, sil, 49) == 0;
}

/* the slots another radio hears from our transmission */
static uint8_t heard_slots[MAXP][9];
static unsigned n_heard_slots;
static void collect(void *ctx, const uint8_t slot[9])
{
    (void)ctx;
    if (n_heard_slots < MAXP) memcpy(heard_slots[n_heard_slots++], slot, 9);
}

static radio_dv_cfg_t channel(void)
{
    radio_dv_cfg_t c = { .mode = RADIO_MODE_NXDN, .rx_hz = 451125000, .tx_hz = 451625000, .power = 1, .nac_rx = 12,
                         .nac_tx = 12, .talkgroup = 200, .unit_id = 4001 };
    return c;
}

int main(void)
{
    ambeVoiceInit(&audio, &vocoder);
    radio_dv_cfg_t c = channel();

    /* Selecting the channel tunes FM receive with the flat path on */
    assert(radio_nxdn_select(&c));
    assert(hw.rx_calls == 1 && hw.rx_hz == 451125000 && hw.flat && hw.flat_on == 1);
    radio_dv_status_t st;
    radio_nxdn_status(&st, now_ms);
    assert(st.active && !st.sync && !st.playing);

    /* Receive: 300 ms of nothing, then a call of 60 slots for our group; every slot reaches the vocoder in order */
    n_sent = 0;
    make_call(2400, 60, 12, 3210, 200);
    bool seen_playing = false;
    while (rx_have < call_end + 8000) {
        step_ms(10);
        radio_nxdn_status(&st, now_ms);
        if (st.playing) {
            seen_playing = true;
            assert(st.source == 3210 && st.talkgroup == 200 && st.nac == 12 && st.sync_ours && !st.encrypted);
        }
    }
    radio_nxdn_status(&st, now_ms);
    fprintf(stderr, "rx: decoded %u of %u slots, playing seen %d\n", n_dec, n_sent, seen_playing);
    assert(seen_playing && !st.playing && !st.sync);
    unsigned k = 0;
    while (k < n_dec && silence_or(dec[k])) k++;
    assert(n_dec - k >= 60 && !memcmp(dec[k], sent[0], 49));
    for (unsigned i = 0; i < 60; i++) assert(!memcmp(dec[k + i], sent[i], 49));

    /* Transmit: the board keys FM on the TX frequency at the channel's power, the receive path is off meanwhile, the
     * mic's frames go out whole; after PTT the release frame and tail go, then the carrier drops and receive resumes */
    n_enc = 0;
    tx_have = 0;
    int rx_before = hw.rx_calls;
    assert(radio_nxdn_ptt(true));
    assert(hw.tx_calls == 1 && hw.tx_hz == 451625000 && hw.power == 1 && hw.pull && !hw.flat);
    radio_nxdn_status(&st, now_ms);
    assert(st.transmitting && st.keyed);
    step_ms(1000);
    assert(radio_nxdn_ptt(false));
    unsigned spoken = n_enc;
    for (unsigned guard = 0; hw.tx_off_calls == 0 && guard < 1000; guard++) step_ms(10);
    assert(hw.tx_off_calls == 1 && hw.inject_off >= 1 && hw.pull == NULL);
    assert(hw.rx_calls == rx_before + 1 && hw.flat && hw.rx_hz == 451125000);
    radio_nxdn_status(&st, now_ms);
    assert(!st.transmitting && st.tx_calls == 1 && st.tx_reason == NT_TX_COMPLETE && st.tx_blocks >= 12);
    /* what went on air, received by another radio */
    static nxdn_link rx;
    static const nxdn_audio_path flat = { 0, 0, 0, 2048 };
    n_heard_slots = 0;
    nxdn_link_init(&rx, &flat, 12, 200, 0, collect, NULL);
    nxdn_link_rx_audio(&rx, tx_air, tx_have);
    fprintf(stderr, "tx: %zu samples, %u slots heard, %u encoded (%u while PTT), calls %u rel %u src %u\n", tx_have,
            n_heard_slots, n_enc, spoken, rx.rx.calls, rx.rx.releases, rx.rx.msg.source);
    assert(rx.rx.calls == 1 && rx.rx.releases == 1 && rx.rx.msg.source == 4001 && rx.rx.msg.destination == 200);
    /* the voice heard: the encoder's frames in order, from the first, maybe after leading silence */
    unsigned h = n_heard_slots, j = 0;
    uint8_t p[49];
    ambe_fec_result_t r;
    for (; j < h; j++) {
        ambeFecDecode(heard_slots[j], p, &r);
        if (!silence_or(p)) break;
    }
    unsigned voice = 0;
    for (; j < h; j++) {
        ambeFecDecode(heard_slots[j], p, &r);
        if (silence_or(p)) break;
        assert(!memcmp(p, enc[voice], 49));
        voice++;
    }
    fprintf(stderr, "tx: %u voice slots on air in order\n", voice);
    assert(voice + 5 >= spoken && voice >= 45);

    /* The board refuses to key (band, calibration, battery): reported, receive goes on */
    hw.refuse_tx = true;
    rx_before = hw.rx_calls;
    assert(!radio_nxdn_ptt(true));
    radio_nxdn_status(&st, now_ms);
    assert(st.tx_calls == 2 && st.tx_reason == NT_DENIED && !st.transmitting && hw.flat && hw.rx_calls == rx_before + 1);
    hw.refuse_tx = false;

    /* Leaving the channel while transmitting: samples stop, carrier off, receive path off, voice stream stopped */
    assert(radio_nxdn_ptt(true));
    step_ms(200);
    int stops = hw.stops;
    assert(radio_nxdn_select(NULL));
    assert(hw.pull == NULL && !hw.transmitting && !hw.flat && hw.stops == stops + 1);

    printf("PASS NXDN radio channel: receive to the vocoder in order, PTT keys FM with the flat path, the mic's frames "
           "on air in order, release then carrier off then receive, board refusal, leaving while on air\n");
    return 0;
}

