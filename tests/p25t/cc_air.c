/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A control channel's outbound stream as dibits (one per byte) for tools/p25t_validate.py: TSDUs built by
 * src/p25t back to back, one, two and three blocks, idle status symbols.
 *   cc_air OUT.dibits NAC */
#include "tmsg.h"
#include <stdio.h>
#include <stdlib.h>

static FILE *out;
static void frame(uint16_t nac, const p25t_osp *m, unsigned n)
{
    uint8_t t[3][P25T_TSBK_BYTES], f[P25T_TSDU_MAX_BYTES];
    for (unsigned i = 0; i < n; i++) p25t_osp_build(&m[i], t[i]);
    unsigned bits = p25t_tsdu_encode(nac, t, n, P25T_STATUS_IDLE, f);
    for (unsigned i = 0; i < bits; i += 2) fputc(((f[i >> 3] >> (7 - (i & 7))) & 1) << 1 | ((f[(i + 1) >> 3] >> (6 - (i & 7))) & 1), out);
    /* null dibits up to the next status symbol boundary, as a frame ends */
    for (unsigned d = bits / 2; d % 36; d++) fputc(d % 36 == 35 ? 3 : 0, out);
}

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    out = fopen(argv[1], "wb");
    uint16_t nac = (uint16_t)strtoul(argv[2], NULL, 16);
    p25t_osp iden = { .opcode = P25T_OSP_IDEN_UP_VU, .u.iden = { 1, { true, false, 1, 0, 12500, 12500, 851006250, 45000000, true } } };
    p25t_osp net = { .opcode = P25T_OSP_NET_STS_BCST, .u.net = { 1, 0xBEE00, 0x3A5, 0x1001, 0x70 } };
    p25t_osp rfss = { .opcode = P25T_OSP_RFSS_STS_BCST, .u.rfss = { 1, true, 0x3A5, 2, 3, 0x1001, 0x70 } };
    p25t_osp grant = { .opcode = P25T_OSP_GRP_V_CH_GRANT, .u.grant = { 0x02, 0x1010, 10101, 1234567 } };
    p25t_osp upd = { .opcode = P25T_OSP_GRP_V_CH_GRANT_UPDT, .u.update = { { 0x1010, 0x1012 }, { 10101, 300 } } };
    p25t_osp reg = { .opcode = P25T_OSP_U_REG_RSP, .u.reg = { 0, 0x3A5, 1234567, 1234567 } };
    for (int k = 0; k < 6; k++) {
        p25t_osp three[3] = { iden, net, rfss };
        frame(nac, three, 3);
        frame(nac, &grant, 1);
        p25t_osp two[2] = { upd, reg };
        frame(nac, two, 2);
    }
    fclose(out);
    return 0;
}
