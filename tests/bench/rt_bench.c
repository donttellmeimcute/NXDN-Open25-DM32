/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Real-time workloads of every mode, one per bench_* function, for tools/rt_budget.py to count with callgrind
 * (instructions inside the function only). Each runs the work one deadline unit needs, `n` times:
 *
 *   imbe_enc     one 20 ms IMBE encode (P25 TX), the self-test's waveform
 *   imbe_dec     one 20 ms IMBE decode (P25 RX)
 *   p25_tx_ldu   one LDU's CAI coding, 180 ms (P25 TX)
 *   p25_rx_ldu   one LDU through the framer and the CAI decoder, 180 ms (P25 RX)
 *   nxdn_tx      one 80 ms frame: framing, modulation, filters (NXDN TX, radio task + timer interrupt pulls)
 *   nxdn_rx      one 80 ms frame of received audio: demodulator, compensation filters, call layer (NXDN RX)
 *   tsbk_cc      one TSDU of three TSBKs, 75 ms, through the trunking receiver (control channel)
 *   tap_voice    one LDU's 864 dibits through the trunking receiver on a voice channel (no TSDU in it)
 *   su_75ms      the trunking logic over 75 ms: 75 ticks, 3 TSBKs, 10 status symbols
 *   p2_burst     one Phase 2 burst (30 ms) through the stream receiver (no PHY on the radio; for reference)
 *
 *   rt_bench WORKLOAD N */
#include "dm32_imbe.h"
#include "nxdn_call.h"
#include "nxdn_link.h"
#include "p25.h"
#include "p2_call.h"
#include "su.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile uint32_t sink_u;

static void selftest_pcm(unsigned f, int16_t pcm[160])
{
    for (unsigned i = 0; i < 160; i++) {
        unsigned phase = (f * 160 + i) % 40;
        pcm[i] = (int16_t)(phase < 20 ? (int)phase * 900 - 9000 : 27000 - (int)phase * 900);
    }
}

static dm32_imbe_context codec;
static int16_t pcm_in[64][160];
static uint16_t u_frames[64][8];

__attribute__((noinline)) void bench_imbe_enc(unsigned n)
{
    for (unsigned k = 0; k < n; k++) dm32_imbe_encode(&codec, pcm_in[k % 64], u_frames[k % 64]);
}

__attribute__((noinline)) void bench_imbe_dec(unsigned n)
{
    int16_t out[160];
    for (unsigned k = 0; k < n; k++) {
        dm32_imbe_decode(&codec, u_frames[k % 64], out);
        sink_u += (uint16_t)out[7];
    }
}

static p25_call call = { 0x293, 100, 1001, 0, 0, 2 };
static uint16_t imbe9[P25_IMBE_FRAMES][8];
static uint8_t ldu[P25_LDU_BYTES];

__attribute__((noinline)) void bench_p25_tx_ldu(unsigned n)
{
    static const uint8_t lsd[2];
    for (unsigned k = 0; k < n; k++) p25_encode_ldu(&call, (k & 1) ? P25_DUID_LDU2 : P25_DUID_LDU1, imbe9, lsd, ldu);
}

static uint8_t ldu_dibits[2][P25_LDU_BYTES * 4];
static p25_rx framer;

__attribute__((noinline)) void bench_p25_rx_ldu(unsigned n)
{
    p25_decoded d;
    for (unsigned k = 0; k < n; k++)
        for (unsigned i = 0; i < P25_LDU_BYTES * 4; i++)
            if (p25_rx_push_dibit(&framer, ldu_dibits[k & 1][i]) == 1) {
                p25_decode_frame(framer.frame, p25_frame_bytes(framer.nid.duid), &d);
                sink_u += d.imbe[0][0];
            }
}

/* NXDN: the radio's path with every compensation filter on (the worst case) */
static const nxdn_audio_path path = { 300, 750, 300, 512 };
static nxdn_link tx_link, rx_link;
static int16_t nxdn_air[200 * 640];
static unsigned nxdn_air_n;

static bool slot_src(void *ctx, uint8_t slot[9])
{
    (void)ctx;
    memcpy(slot, nxdn_silence_slot, 9);
    return true;
}

static void slot_sink(void *ctx, const uint8_t slot[9])
{
    (void)ctx;
    sink_u += slot[0];
}

__attribute__((noinline)) void bench_nxdn_tx(unsigned n)
{
    int16_t buf[640];
    for (unsigned k = 0; k < n; k++) {
        nxdn_link_tx_fill(&tx_link, slot_src, NULL);
        unsigned got = 0;
        /* the timer interrupt's pulls: 16 samples every 2 ms */
        while (got < 640) {
            uint16_t m = nxdn_link_pull(&tx_link, buf + got, 16);
            if (!m) break;
            got += m;
            nxdn_link_tx_fill(&tx_link, slot_src, NULL);
        }
        sink_u += got;
    }
}

__attribute__((noinline)) void bench_nxdn_rx(unsigned n)
{
    for (unsigned k = 0; k < n; k++) {
        unsigned at = (k * 640) % (nxdn_air_n - 640);
        nxdn_link_rx_audio(&rx_link, nxdn_air + at, 640);
    }
}

/* Trunking */
static uint8_t cc_stream[360];
static p25t_rx trx;
static unsigned tsbks;
static void on_tsbk(void *c, uint16_t nac, const uint8_t t[12]) { (void)c; (void)nac; tsbks += t[0]; }

__attribute__((noinline)) void bench_tsbk_cc(unsigned n)
{
    for (unsigned k = 0; k < n; k++)
        for (unsigned i = 0; i < 360; i++) p25t_rx_dibit(&trx, cc_stream[i]);
}

__attribute__((noinline)) void bench_tap_voice(unsigned n)
{
    for (unsigned k = 0; k < n; k++)
        for (unsigned i = 0; i < P25_LDU_BYTES * 4; i++) p25t_rx_dibit(&trx, ldu_dibits[k & 1][i]);
}

static p25t_su su;
static uint8_t osp[3][12];

__attribute__((noinline)) void bench_su_75ms(unsigned n)
{
    p25t_su_out o;
    static uint32_t ms = 1000;
    for (unsigned k = 0; k < n; k++) {
        for (unsigned t = 0; t < 75; t++) p25t_su_tick(&su, ms++, &o);
        for (unsigned b = 0; b < 3; b++) p25t_su_tsbk(&su, 0x293, osp[b], &o);
        for (unsigned s = 0; s < 10; s++) p25t_su_status(&su, P25T_STATUS_IDLE, &o);
    }
}

static uint8_t p2_stream[12][180];
static p2_rx p2r;
static void on_mac(void *c, uint8_t ch, const p2_mac *m) { (void)c; (void)ch; sink_u += m->opcode; }
static void on_vcw(void *c, uint8_t ch, const uint8_t v[9], bool e) { (void)c; (void)ch; (void)e; sink_u += v[0]; }

__attribute__((noinline)) void bench_p2_burst(unsigned n)
{
    for (unsigned k = 0; k < n; k++)
        for (unsigned i = 0; i < 180; i++) p2_rx_dibit(&p2r, p2_stream[k % 12][i]);
}

static void setup(void)
{
    dm32_imbe_init(&codec);
    for (unsigned f = 0; f < 64; f++) selftest_pcm(f, pcm_in[f]);
    for (unsigned f = 0; f < 64; f++) dm32_imbe_encode(&codec, pcm_in[f], u_frames[f]);
    dm32_imbe_init(&codec);
    for (unsigned f = 0; f < 9; f++) memcpy(imbe9[f], u_frames[f], sizeof imbe9[f]);
    static const uint8_t lsd[2];
    for (unsigned j = 0; j < 2; j++) {
        p25_encode_ldu(&call, j ? P25_DUID_LDU2 : P25_DUID_LDU1, imbe9, lsd, ldu);
        for (unsigned i = 0; i < P25_LDU_BYTES * 4; i++) ldu_dibits[j][i] = (uint8_t)((ldu[i / 4] >> (6 - 2 * (i % 4))) & 3);
    }
    p25_rx_init(&framer, 0x293, 2);
    /* NXDN: a transmission from our own transmitter, received with every filter on */
    nxdn_link_init(&tx_link, &path, 12, 200, 4001, slot_sink, NULL);
    nxdn_link_init(&rx_link, &path, 12, 200, 4001, slot_sink, NULL);
    nxdn_tx_cfg cfg = { .ran = 12, .source = 4001, .destination = 200 };
    nxdn_link_tx_begin(&tx_link, &cfg);
    int16_t buf[640];
    while (nxdn_air_n + 640 <= sizeof nxdn_air / sizeof nxdn_air[0]) {
        nxdn_link_tx_fill(&tx_link, slot_src, NULL);
        uint16_t m = nxdn_link_pull(&tx_link, buf, 640);
        memcpy(nxdn_air + nxdn_air_n, buf, m * sizeof buf[0]);
        nxdn_air_n += m;
        if (!m) break;
    }
    /* a control channel TSDU of three TSBKs */
    p25t_osp m[3] = {
        { .opcode = P25T_OSP_NET_STS_BCST, .u.net = { 1, 0xBEE00, 0x3A5, 0x1001, 0x70 } },
        { .opcode = P25T_OSP_RFSS_STS_BCST, .u.rfss = { 1, true, 0x3A5, 2, 3, 0x1001, 0x70 } },
        { .opcode = P25T_OSP_GRP_V_CH_GRANT, .u.grant = { 4, 0x1010, 300, 5555 } },
    };
    uint8_t frame[P25T_TSDU_MAX_BYTES];
    for (unsigned b = 0; b < 3; b++) p25t_osp_build(&m[b], osp[b]);
    unsigned bits = p25t_tsdu_encode(0x293, (const uint8_t(*)[12])osp, 3, P25T_STATUS_IDLE, frame);
    for (unsigned i = 0; i < bits / 2; i++) cc_stream[i] = (uint8_t)((frame[i / 4] >> (6 - 2 * (i % 4))) & 3);
    p25t_rx_init(&trx, 0x293, on_tsbk, NULL, NULL, NULL);
    p25t_su_cfg sc = { .wacn = 0xBEE00, .sysid = 0x3A5, .unit_id = 1001, .nac = 0x293, .cc_hz = { 851012500 },
                       .cc_count = 1, .talkgroup = 100, .tx_enabled = true };
    p25t_su_out o;
    p25t_su_init(&su, &sc, 0, 1, &o);
    /* Phase 2: a superframe of a call on channel 0 */
    p2_tx t0, t1;
    p2_tx *ch[2] = { &t0, &t1 };
    uint8_t mask[P2_MASK_BYTES];
    p2_mask(0xBEE00, 0x3A5, 0x293, mask);
    p2_tx_init(&t0, 0x293);
    p2_tx_init(&t1, 0x293);
    p2_tx_begin(&t0, 100, 1001);
    for (unsigned n = 0; n < 3; n++)
        for (unsigned b = 0; b < 12; b++) {
            for (int k = 0; k < 2; k++) p2_tx_voice(&t0, nxdn_silence_slot);
            p2_outbound_burst(ch, b, n, mask, p2_stream[b]);
        }
    p2_rx_init(&p2r, 0, 0xBEE00, 0x3A5, 0x293, on_mac, on_vcw, NULL);
    for (unsigned k = 0; k < 24; k++)
        for (unsigned i = 0; i < 180; i++) p2_rx_dibit(&p2r, p2_stream[k % 12][i]);
}

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    unsigned n = (unsigned)atoi(argv[2]);
    setup();
    static const struct { const char *name; void (*fn)(unsigned); } w[] = {
        { "imbe_enc", bench_imbe_enc }, { "imbe_dec", bench_imbe_dec }, { "p25_tx_ldu", bench_p25_tx_ldu },
        { "p25_rx_ldu", bench_p25_rx_ldu }, { "nxdn_tx", bench_nxdn_tx }, { "nxdn_rx", bench_nxdn_rx },
        { "tsbk_cc", bench_tsbk_cc }, { "tap_voice", bench_tap_voice }, { "su_75ms", bench_su_75ms },
        { "p2_burst", bench_p2_burst },
    };
    for (unsigned i = 0; i < sizeof w / sizeof w[0]; i++)
        if (!strcmp(argv[1], w[i].name)) {
            w[i].fn(n);
            printf("%s %u %u\n", w[i].name, n, sink_u + tsbks);
            return 0;
        }
    return 2;
}
