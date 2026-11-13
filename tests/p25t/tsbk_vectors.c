/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Outbound TSBKs built by src/p25t/tmsg.c, as JSON lines (the 12 octets in hex and the values
 * put in), for tools/p25t_validate.py to parse with OP25's decoder. */
#include "tmsg.h"
#include <stdio.h>

static void emit(const p25t_osp *m, const char *fields)
{
    uint8_t t[P25T_TSBK_BYTES];
    if (!p25t_osp_build(m, t)) return;
    printf("{\"opcode\": %u, \"hex\": \"", m->opcode);
    for (int i = 0; i < 12; i++) printf("%02x", t[i]);
    printf("\", %s}\n", fields);
}

int main(void)
{
    char f[256];
    p25t_osp m = { 0 };
    m.opcode = P25T_OSP_GRP_V_CH_GRANT;
    m.u.grant.svc = 0x04; m.u.grant.channel = 0x1123; m.u.grant.group = 10101; m.u.grant.source = 1234567;
    snprintf(f, sizeof f, "\"svc\": 4, \"channel\": %u, \"group\": 10101, \"source\": 1234567", 0x1123);
    emit(&m, f);
    m = (p25t_osp){ 0 };
    m.opcode = P25T_OSP_GRP_V_CH_GRANT_UPDT;
    m.u.update.channel[0] = 0x1010; m.u.update.group[0] = 200; m.u.update.channel[1] = 0x1012; m.u.update.group[1] = 300;
    snprintf(f, sizeof f, "\"channel1\": %u, \"group1\": 200, \"channel2\": %u, \"group2\": 300", 0x1010, 0x1012);
    emit(&m, f);
    m = (p25t_osp){ 0 };
    m.opcode = P25T_OSP_U_REG_RSP;
    m.u.reg.response = 0; m.u.reg.sysid = 0x3a5; m.u.reg.source_id = 1234567; m.u.reg.source_address = 1234567;
    emit(&m, "\"response\": 0, \"sysid\": 933, \"source_id\": 1234567, \"source_address\": 1234567");
    m = (p25t_osp){ 0 };
    m.opcode = P25T_OSP_GRP_AFF_RSP;
    m.u.aff.global = true; m.u.aff.response = 0; m.u.aff.announce_group = 65535; m.u.aff.group = 10101; m.u.aff.target = 1234567;
    emit(&m, "\"global\": 1, \"response\": 0, \"announce_group\": 65535, \"group\": 10101, \"target\": 1234567");
    m = (p25t_osp){ 0 };
    m.opcode = P25T_OSP_RFSS_STS_BCST;
    m.u.rfss.lra = 1; m.u.rfss.active = true; m.u.rfss.sysid = 0x3a5; m.u.rfss.rfss = 2; m.u.rfss.site = 3; m.u.rfss.channel = 0x1001; m.u.rfss.ssc = 0x70;
    snprintf(f, sizeof f, "\"lra\": 1, \"active\": 1, \"sysid\": 933, \"rfss\": 2, \"site\": 3, \"channel\": %u, \"ssc\": 112", 0x1001);
    emit(&m, f);
    m = (p25t_osp){ 0 };
    m.opcode = P25T_OSP_NET_STS_BCST;
    m.u.net.lra = 1; m.u.net.wacn = 0xbee00; m.u.net.sysid = 0x3a5; m.u.net.channel = 0x1001; m.u.net.ssc = 0x70;
    snprintf(f, sizeof f, "\"lra\": 1, \"wacn\": %u, \"sysid\": 933, \"channel\": %u, \"ssc\": 112", 0xbee00, 0x1001);
    emit(&m, f);
    m = (p25t_osp){ 0 };
    m.opcode = P25T_OSP_IDEN_UP_VU;
    m.u.iden.id = 1; m.u.iden.band = (p25t_iden){ true, false, 1, 0, 12500, 12500, 851006250, 5000000, true };
    emit(&m, "\"id\": 1, \"base_hz\": 851006250, \"spacing_hz\": 12500, \"tx_offset_hz\": 5000000, \"bandwidth_hz\": 12500");
    m = (p25t_osp){ 0 };
    m.opcode = P25T_OSP_IDEN_UP;
    m.u.iden.id = 2; m.u.iden.band = (p25t_iden){ true, false, 1, 0, 12500, 6250, 851012500, -45000000, true };
    emit(&m, "\"id\": 2, \"base_hz\": 851012500, \"spacing_hz\": 6250, \"tx_offset_hz\": -45000000, \"bandwidth_hz\": 12500");
    m = (p25t_osp){ 0 };
    m.opcode = P25T_OSP_IDEN_UP_TDMA;
    m.u.iden.id = 3; m.u.iden.band = (p25t_iden){ true, true, 2, 3, 12500, 12500, 769006250, 30000000, true };
    emit(&m, "\"id\": 3, \"base_hz\": 769006250, \"spacing_hz\": 12500, \"tx_offset_hz\": 30000000, \"channel_type\": 3, \"slots\": 2");
    m = (p25t_osp){ 0 };
    m.opcode = P25T_OSP_U_DE_REG_ACK;
    m.u.dereg.wacn = 0xbee00; m.u.dereg.sysid = 0x3a5; m.u.dereg.target = 1234567;
    snprintf(f, sizeof f, "\"wacn\": %u, \"sysid\": 933, \"target\": 1234567", 0xbee00);
    emit(&m, f);
    return 0;
}
