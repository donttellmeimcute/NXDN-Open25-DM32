/* SPDX-License-Identifier: GPL-3.0-or-later
 * Deterministic sanitizer exercise for packet lengths, corrupt data, APIs.
 */
#include "p25.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>

static uint32_t random_state = 0x2532;
static uint32_t next(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

int main(void)
{
    p25_rx rx;
    p25_decoded decoded;
    p25_call call = {0x293, 123, 123456, 0, 0, 0};
    uint8_t packet[216], copy[216], lsd[2] = {0x12, 0x34};
    uint16_t imbe[9][8] = {{0}};
    unsigned i, j, k;
    p25_rx_init(&rx, P25_NAC_ANY, 2);
    for (i = 0; i < 100000; ++i) {
        uint64_t nid = ((uint64_t)next() << 32) | next();
        p25_nid result;
        (void)p25_nid_decode(nid, &result);
        (void)p25_rx_push_dibit(&rx, (uint8_t)(next() & 3));
        if (!(i & 255)) {
            size_t length = next() % 217;
            for (j = 0; j < sizeof packet; ++j) packet[j] = (uint8_t)next();
            (void)p25_decode_frame(packet, length, &decoded);
        }
    }
    for (i = 0; i < 3000; ++i) {
        for (j = 0; j < 9; ++j) for (k = 0; k < 8; ++k)
            imbe[j][k] = (uint16_t)(next() & ((1u << (k < 4 ? 12 : k < 7 ? 11 : 7)) - 1));
        assert(!p25_encode_ldu(&call, 5, (const uint16_t (*)[8])imbe, lsd, packet));
        memcpy(copy, packet, sizeof packet);
        for (j = 0; j < 20; ++j) copy[next() % 216] ^= (uint8_t)next();
        (void)p25_decode_frame(copy, sizeof copy, &decoded);
    }
    assert(p25_rs_encode(packet, SIZE_MAX, 8, copy) < 0);
    assert(p25_rs_encode(packet, 20, UINT32_MAX, copy) < 0);
    assert(p25_rs_decode(packet, SIZE_MAX, 8) < 0);
    assert(p25_decode_frame(0, 0, &decoded) < 0);
    assert(p25_rx_push_dibit(0, 0) < 0);
    assert(p25_imbe_encode(0, packet) < 0);
    assert(p25_imbe_decode(0, imbe[0], packet) < 0);
    call.nac = 0x1000;
    assert(p25_encode_hdu(&call, packet) < 0);
    return 0;
}
