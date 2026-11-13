/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "phy_stream.h"
#include "p25_runtime.h"

static unsigned bit(const uint8_t *p, unsigned n) { return (p[n / 8] >> (7 - n % 8)) & 1u; }
static void segment(uint8_t *out, unsigned *at, const uint8_t *in, unsigned from, unsigned count)
{
    for (unsigned i = 0; i < count; i++, (*at)++)
        out[*at / 8] |= (uint8_t)(bit(in, from + i) << (7 - *at % 8));
}

int p25_phy_rx_air(const uint8_t ram[36], uint8_t kind, uint8_t air[36])
{
    if (!ram || !air || kind > 2) return -1;
    uint8_t out[36] = {0};
    unsigned n = 0;
    segment(out, &n, ram, 0, 24);
    if (kind < 2) {
        segment(out, &n, ram, 72, 108);
        segment(out, &n, ram, 24, 48);
        segment(out, &n, ram, 180, 108);
    } else {
        segment(out, &n, ram, 92, 98);
        segment(out, &n, ram, 24, 10);
        segment(out, &n, ram, 44, 48);
        segment(out, &n, ram, 34, 10);
        segment(out, &n, ram, 190, 98);
    }
    memcpy(air, out, sizeof out);
    return 0;
}

int p25_phy_rx_record(p25_phy_rx *phy, p25_voice *v, uint32_t sequence,
                      uint8_t kind, const uint8_t ram[36])
{
    uint8_t air[36];
    int gap = phy->sequenced && sequence != phy->next_sequence;
    phy->next_sequence = sequence + 1u;
    phy->sequenced = 1;
    if (gap) {
        phy->gaps++;
        p25_voice_rx_reset(v);
        if (phy->tap) phy->tap(phy->tap_ctx, P25_PHY_TAP_GAP);
    }
    if (p25_phy_rx_air(ram, kind, air)) {
        phy->unsupported++;
        p25_voice_rx_reset(v);
        if (phy->tap) phy->tap(phy->tap_ctx, P25_PHY_TAP_GAP);
        return -1;
    }
    phy->records++;
    for (unsigned i = 0; i < 144; i++) {
        uint8_t d = (uint8_t)((air[i / 4] >> (6 - (i % 4) * 2)) & 3);
        if (phy->tap) phy->tap(phy->tap_ctx, d);
        p25_voice_rx_dibit(v, d);
    }
    return gap ? -1 : 0;
}

size_t p25_phy_tx_block(p25_voice *v, uint8_t ram[36])
{
    uint8_t dibits[144];
    size_t n = p25_voice_tx_read(v, dibits, sizeof dibits);
    memset(ram, 0, 36);
    if (v->fault) return 0;
    for (size_t i = 0; i < n; i++) ram[i / 4] |= (uint8_t)(dibits[i] << (6 - (i % 4) * 2));
    return n;
}

size_t p25_phy_tx_pack(const uint8_t *dibits, size_t n, uint8_t ram[36])
{
    if (n > P25_PHY_SYMBOLS) n = P25_PHY_SYMBOLS;
    memset(ram, 0, 36);
    for (size_t i = 0; i < n; i++) ram[i / 4] |= (uint8_t)((dibits[i] & 3u) << (6 - (i % 4) * 2));
    return n;
}
