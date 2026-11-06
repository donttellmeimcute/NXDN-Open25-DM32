/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "selftest.h"
#include "phy_stream.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "reference_frames.h"

static p25_voice v;
static unsigned blocks, loud;
static const p25_call call = {0x293, 42, 123456, 0, 0, 0};
static void sink(void *ctx, const int16_t pcm[160])
{
    (void)ctx;
    blocks++;
    for (unsigned i = 0; i < 160; i++) loud += pcm[i] != 0;
}
static void reset(uint16_t nac, uint16_t group)
{
    blocks = loud = 0;
    p25_voice_init(&v, nac, group, sink, NULL);
}
static void feed(const uint8_t *data, size_t n)
{
    for (size_t i = 0; i < n; i++)
        for (unsigned j = 0; j < 4; j++) p25_voice_rx_dibit(&v, (data[i] >> (6 - j * 2)) & 3u);
}
static void pcm(unsigned frame)
{
    int16_t a[160];
    for (unsigned i = 0; i < 160; i++) a[i] = (int16_t)(((frame * 160 + i) % 40) * 400 - 8000);
    assert(p25_voice_tx_pcm(&v, a));
}
static size_t drain(uint8_t *air, size_t capacity)
{
    uint8_t d[144];
    size_t at = 0;
    while (!p25_voice_tx_done(&v)) {
        size_t n = p25_voice_tx_read(&v, d, sizeof d);
        assert(!v.fault && n && n % 4 == 0 && at + n / 4 <= capacity);
        for (size_t i = 0; i < n; i += 4) air[at++] = (d[i] << 6) | (d[i+1] << 4) | (d[i+2] << 2) | d[i+3];
    }
    return at;
}
static void check_frame(const uint8_t *data, size_t n, unsigned duid)
{
    p25_decoded d;
    assert(!p25_decode_frame(data, n, &d));
    assert(d.nid.nac == call.nac && d.nid.duid == duid && d.metadata_valid);
}
static void short_call(void)
{
    uint8_t air[333], d[1];
    reset(0x293, 42);
    assert(p25_voice_tx_begin(&v, &call));
    assert(!p25_voice_tx_read(&v, d, sizeof d)); /* no RF before prefill */
    assert(!p25_voice_tx_begin(&v, &call));
    pcm(0);
    assert(p25_voice_tx_end(&v));
    assert(drain(air, sizeof air) == sizeof air);
    check_frame(air, 99, P25_DUID_HDU);
    check_frame(air + 99, 216, P25_DUID_LDU1);
    check_frame(air + 315, 18, P25_DUID_TDU);
    feed(air, sizeof air);
    assert(blocks == 9 && loud && !v.rx_muted && !v.rx_erasures);
    assert(!v.crypto_clear && !v.have_group); /* terminator */
    reset(0x294, 42);
    feed(air, sizeof air);
    assert(blocks == 0); /* wrong NAC */
    reset(0x293, 43);
    feed(air, sizeof air);
    assert(blocks == 9 && loud == 0 && v.rx_muted == 9); /* wrong group */
    reset(0x293, 42);
    feed(air + 99, 216);
    assert(blocks == 9 && loud == 0); /* late entry waits for ESYNC */
    reset(0x293, 42);
    feed(air, 99);
    p25_voice_rx_reset(&v);
    feed(air + 99, 216);
    assert(loud == 0); /* retune/gap must clear old admission */
    reset(0xf7e, 42);
    feed(air, sizeof air);
    assert(blocks == 9 && loud); /* standard wildcard NAC */
    reset(0x293, 42);
    feed(encrypted_hdu, sizeof encrypted_hdu);
    feed(air + 99, 216);
    assert(blocks == 9 && !loud && v.rx_muted == 9);
    reset(0x293, 42);
    feed(foreign_hdu, sizeof foreign_hdu);
    feed(air + 99, 216);
    assert(blocks == 9 && !loud);
    reset(0x293, 42);
    feed(air, 99);
    feed(encrypted_ldu2, sizeof encrypted_ldu2);
    feed(air + 99, 216); /* unprotected LC cannot override encrypted ESYNC */
    assert(blocks == 18 && !loud && v.rx_muted == 18);
    uint8_t damaged[99];
    memcpy(damaged, air, 99);
    memset(damaged + 14, 0xa5, 80);
    p25_decoded bad;
    assert(!p25_decode_frame(damaged, sizeof damaged, &bad) && !bad.metadata_valid);
    reset(0x293, 42);
    feed(air, 99);
    feed(damaged, sizeof damaged);
    feed(air + 99, 216);
    assert(blocks == 9 && !loud);
    /* All Call is admitted even with a different selected group. */
    reset(0x293, 42);
    p25_call all = call; all.talkgroup = 0xffff;
    assert(p25_voice_tx_begin(&v, &all));
    pcm(0); assert(p25_voice_tx_end(&v));
    assert(drain(air, sizeof air) == sizeof air);
    feed(air, sizeof air);
    assert(blocks == 9 && loud);
}
static void paced_call(void)
{
    uint8_t air[4096], d[144];
    size_t at = 0;
    reset(0x293, 42);
    assert(p25_voice_tx_begin(&v, &call));
    unsigned mic = 0, ended = 0;
    for (unsigned ms = 0; ms < 3000 && !p25_voice_tx_done(&v); ms += 10) {
        if (ms % 20 == 0 && mic < 47) pcm(mic++);
        if (mic == 47 && !ended) { assert(p25_voice_tx_end(&v)); ended = 1; }
        if (ms % 30 == 0) {
            size_t n = p25_voice_tx_read(&v, d, sizeof d);
            assert(!v.fault && at + n / 4 <= sizeof air);
            for (size_t i = 0; i < n; i += 4) air[at++] = (d[i] << 6) | (d[i+1] << 4) | (d[i+2] << 2) | d[i+3];
        }
    }
    assert(p25_voice_tx_done(&v) && at == 99 + 6 * 216 + 18 && !v.tx_underruns);
    check_frame(air, 99, P25_DUID_HDU);
    for (unsigned i = 0; i < 6; i++) check_frame(air + 99 + i * 216, 216, i % 2 ? P25_DUID_LDU2 : P25_DUID_LDU1);
    feed(air, at);
    assert(blocks == 54 && loud && !v.rx_muted && !v.rx_erasures);
    /* An LDU2 by itself cannot bypass talkgroup admission. */
    reset(0x293, 42);
    feed(air + 99 + 216, 216);
    assert(blocks == 9 && loud == 0);
    /* Clear late entry starts only once LC and ESYNC are both available. */
    reset(0x293, 42);
    feed(air + 99, 432);
    assert(blocks == 18 && loud && v.rx_muted == 9);
    /* NAC wildcard cannot inherit group/crypto state across networks. */
    reset(P25_NAC_ANY, 42);
    feed(air, 99);
    p25_call other = call;
    other.nac = 0x294;
    uint16_t imbe[9][8] = {{0}};
    uint8_t ldu[216], lsd[2] = {0};
    assert(!p25_encode_ldu(&other, P25_DUID_LDU1, imbe, lsd, ldu));
    feed(ldu, sizeof ldu);
    assert(blocks == 9 && loud == 0);
}
static void faults(void)
{
    uint8_t d[144];
    reset(0x293, 42);
    assert(p25_voice_tx_begin(&v, &call));
    pcm(0);
    dm32_imbe_context saved = v.codec;
    p25_voice_rx_reset(&v);
    uint8_t stale[36] = {0};
    p25_phy_rx phy = {0};
    assert(p25_phy_rx_record(&phy, &v, 0, 3, stale) == -1);
    assert(!memcmp(&saved, &v.codec, sizeof saved));
    p25_voice_tx_abort(&v);
    assert(p25_voice_tx_begin(&v, &call));
    assert(p25_voice_tx_end(&v) && p25_voice_tx_done(&v)); /* empty PTT */
    assert(p25_voice_tx_begin(&v, &call));
    for (unsigned i = 0; i < 9; i++) pcm(i);
    for (unsigned i = 0; i < 9; i++) (void)p25_voice_tx_read(&v, d, 144);
    assert(v.fault && v.tx_underruns == 1 && p25_voice_tx_done(&v));
    assert(p25_voice_tx_begin(&v, &call));
    for (unsigned i = 0; i < 26; i++) pcm(i);
    int16_t a[160] = {0};
    assert(!p25_voice_tx_pcm(&v, a)); /* third LDU cannot overrun HDU + two LDUs */
    assert(v.fault && p25_voice_tx_done(&v));
    p25_call invalid = call;
    invalid.nac = P25_NAC_ANY;
    assert(!p25_voice_tx_begin(&v, &invalid));
    invalid = call; invalid.source_id = 10000000;
    assert(!p25_voice_tx_begin(&v, &invalid));
    invalid = call; invalid.source_id = 0;
    assert(!p25_voice_tx_begin(&v, &invalid));
    invalid = call; invalid.talkgroup = 0;
    assert(!p25_voice_tx_begin(&v, &invalid));
}
static void layout(void)
{
    /* Independently label every bit by the published field lengths, then
     * check all 288 one-hot inputs in each layout, including in-place use. */
    for (unsigned kind = 0; kind < 3; kind++) {
        unsigned from[288], at = 0;
        const unsigned spans0[][2] = {{0,24},{72,108},{24,48},{180,108}};
        const unsigned spans2[][2] = {{0,24},{92,98},{24,10},{44,48},{34,10},{190,98}};
        for (unsigned seg = 0; seg < (kind == 2 ? 6u : 4u); seg++) {
            unsigned first = kind == 2 ? spans2[seg][0] : spans0[seg][0];
            unsigned count = kind == 2 ? spans2[seg][1] : spans0[seg][1];
            for (unsigned b = 0; b < count; b++) from[at++] = first + b;
        }
        for (unsigned b = 0; b < 288; b++) {
            uint8_t input[36] = {0}, out[36];
            input[b / 8] = (uint8_t)(1u << (7 - b % 8));
            assert(!p25_phy_rx_air(input, kind, out));
            for (unsigned x = 0; x < 288; x++) assert(((out[x/8] >> (7-x%8)) & 1u) == (from[x] == b));
            assert(!p25_phy_rx_air(input, kind, input));
            assert(!memcmp(input, out, 36));
        }
    }
    uint8_t ram[36] = {0}, out[36];
    memset(out, 0xa5, 36);
    assert(p25_phy_rx_air(ram, 3, out) == -1 && out[0] == 0xa5);
    reset(0x293, 42);
    v.crypto_clear = v.have_group = 1;
    p25_phy_rx phy = {0};
    assert(!p25_phy_rx_record(&phy, &v, 0xfffffffeu, 0, ram));
    assert(!p25_phy_rx_record(&phy, &v, 0xffffffffu, 0, ram));
    assert(!p25_phy_rx_record(&phy, &v, 0, 0, ram));
    assert(p25_phy_rx_record(&phy, &v, 2, 0, ram) == -1);
    assert(phy.gaps == 1 && !v.crypto_clear && !v.have_group);
    assert(p25_phy_rx_record(&phy, &v, 3, 3, ram) == -1 && phy.unsupported == 1);
}
int main(void)
{
    p25_selftest_result r;
    p25_selftest_run(&v, &r, NULL, NULL);
    printf("selftest ok=%u air=%u blocks=%u pcm_crc=%08x nonzero=%u session_bytes=%u\n",
            r.ok, r.air_bytes, r.pcm_blocks, r.pcm_crc, r.nonzero_samples, r.session_bytes);
    assert(r.ok);
    short_call(); paced_call(); faults(); layout();
    puts("PASS conventional voice lifecycle, pacing, admission, underrun, overflow, PHY layouts and loss");
    return 0;
}
