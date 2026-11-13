/* SPDX-License-Identifier: GPL-3.0-or-later */
/* P25 Phase 2 (TDMA) above the physical layer: the channel coding of the
 * outbound timeslots (TIA-102.BBAC structure, as OP25 and dsd-fme decode it;
 * docs/p25-phase2.md, docs/protocol-sources.json).
 *
 * The air stream is 6000 dibits/s. A superframe is 12 bursts of 180 dibits
 * (30 ms): a 20-dibit ISCH, then a 160-dibit payload. Bursts 0-9 alternate
 * logical channel 0 and 1; bursts 10 and 11 carry the SACCH of channel 1 and
 * 0. The ISCH before bursts 2, 3, 6, 7, 10 and 11 is the S-ISCH sync word; the
 * others (I-ISCH) carry the burst's channel and location in the superframe.
 *
 * A payload: DUID dibits at 0, 37, 122 and 159 (an (8,4) code, never
 * scrambled) around the fields:
 *   4V     voice code words at 1, 38, 86, 123 (36 dibits each), ESS-B at 74 (12)
 *   2V     voice code words at 1, 38, ESS-A at 74-121 and 123-158 (28 hex bits)
 *   SACCH  1-36, 38-121, 123-158: 52 hex bits of RS(63,35) shortened by 5 and
 *          punctured by 6: a 168-bit MAC PDU and its CRC-12
 *   FACCH  1-36, 38-68, 90-121, 123-158: 45 hex bits, shortened by 9,
 *          punctured by 9: a 144-bit MAC PDU and its CRC-12
 * Voice code words are AMBE+2 3600x2450 frames in exactly the 72-bit air form
 * DMR uses (same Golay codes, whitening and interleave; tools/gen_p25p2_tables.py
 * checks the interleave against OP25's), so the radio's DMR vocoder path
 * takes them unchanged.
 *
 * Scrambling: the payload dibits except the DUID are XORed with a sequence
 * from a 44-bit LFSR seeded with WACN, System ID and NAC; burst b's payload
 * dibit p takes sequence dibit 180 b + 10 + p.
 *
 * No allocation, no RTOS, no hardware. */
#ifndef P25P2_P2_H
#define P25P2_P2_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    P2_SYMBOL_RATE = 6000,
    P2_BURST_DIBITS = 180,
    P2_ISCH_DIBITS = 20,
    P2_PAYLOAD_DIBITS = 160,
    P2_SUPERFRAME_BURSTS = 12,
    P2_SUPERFRAME_DIBITS = 2160,
    P2_MASK_BYTES = 540,            /* 4320 bits */
    P2_VCW_BYTES = 9,               /* one AMBE+2 frame, 72 bits */
    P2_FACCH_BYTES = 18,            /* MAC PDU sizes */
    P2_SACCH_BYTES = 21,
    P2_ESS_HEX = 44,                /* ESS-B (16) then ESS-A (28) */
    /* DUID values */
    P2_DUID_4V = 0,
    P2_DUID_SACCH_S = 3,            /* scrambled */
    P2_DUID_2V = 6,
    P2_DUID_FACCH_S = 9,
    P2_DUID_SACCH = 12,             /* not scrambled */
    P2_DUID_LCCH = 13,
    P2_DUID_FACCH = 15,
};

#define P2_S_ISCH UINT64_C(0x575D57F7FF)

/* The logical channel (0 or 1) of burst b of a superframe */
static inline uint8_t p2_burst_channel(unsigned b)
{
    return b >= 10 ? (uint8_t)(11 - b) : (uint8_t)(b & 1u);
}
/* The ISCH before burst b: S-ISCH, or I-ISCH with chn = b % 4 and loc = b / 4 */
static inline bool p2_burst_has_sync(unsigned b)
{
    return (b & 3u) >= 2;
}

/* I-ISCH: value = chn << 5 | loc << 3 | fr << 2 | cnt (7 bits) */
uint64_t p2_isch_encode(uint8_t value);
/* -2: the S-ISCH; -1: no code word within 7 bit errors; else the value */
int p2_isch_decode(uint64_t cw, uint8_t *errors);
static inline uint8_t p2_isch_value(uint8_t chn, uint8_t loc, bool fr, uint8_t cnt)
{
    return (uint8_t)((chn & 3u) << 5 | (loc & 3u) << 3 | (fr ? 4u : 0u) | (cnt & 3u));
}

uint8_t p2_duid_encode(uint8_t duid);
/* the DUID within one bit error, else -1 */
int p2_duid_decode(uint8_t cw);

/* The scrambling sequence of a superframe, 4320 bits MSB first */
void p2_mask(uint32_t wacn, uint16_t sysid, uint16_t nac, uint8_t mask[P2_MASK_BYTES]);
static inline uint8_t p2_mask_dibit(const uint8_t mask[P2_MASK_BYTES], unsigned d)
{
    d %= P2_SUPERFRAME_DIBITS;
    return (uint8_t)((mask[d >> 2] >> (6 - 2 * (d & 3u))) & 3u);
}

/* Reed-Solomon (63,35) over GF(64), x^6 + x + 1, roots alpha^1..alpha^28; code word symbol 0 is the highest
 * degree (data 0-34, parity 35-62), as ezpwd's RS<63,35> that OP25 and dsd-fme decode with. */
void p2_rs_encode(uint8_t cw[63]);
/* Errors and erasures; returns the symbols corrected (erasures included), -1 when it cannot */
int p2_rs_decode(uint8_t cw[63], const uint8_t *erasures, unsigned n_erasures);

/* CRC-12 of a MAC PDU (x^12 + x^11 + x^7 + x^4 + x^2 + x + 1, inverted), over bits MSB first */
uint16_t p2_crc12(const uint8_t *bytes, unsigned bits);

/* Fields of a payload (160 dibits, descrambled). The DUID positions are left alone. */
void p2_xcch_encode(bool facch, const uint8_t *pdu, uint8_t payload[P2_PAYLOAD_DIBITS]);
/* The PDU (18 or 21 bytes) when RS and the CRC check; returns RS corrections, -1 otherwise */
int p2_xcch_decode(bool facch, const uint8_t payload[P2_PAYLOAD_DIBITS], uint8_t *pdu);

/* Encryption sync (algorithm, key, message indicator) <-> ESS-B and ESS-A hex bits (RS(44,16,29)) */
void p2_ess_encode(uint8_t algid, uint16_t keyid, const uint8_t mi[9], uint8_t hex[P2_ESS_HEX]);
int p2_ess_decode(uint8_t hex[P2_ESS_HEX], uint8_t *algid, uint16_t *keyid, uint8_t mi[9]);

/* 4V: four code words and ESS-B part k (hex[4k..4k+3]); 2V: two code words and ESS-A */
void p2_4v_encode(const uint8_t vcw[4][P2_VCW_BYTES], const uint8_t essb[4], uint8_t payload[P2_PAYLOAD_DIBITS]);
void p2_4v_decode(const uint8_t payload[P2_PAYLOAD_DIBITS], uint8_t vcw[4][P2_VCW_BYTES], uint8_t essb[4]);
void p2_2v_encode(const uint8_t vcw[2][P2_VCW_BYTES], const uint8_t essa[28], uint8_t payload[P2_PAYLOAD_DIBITS]);
void p2_2v_decode(const uint8_t payload[P2_PAYLOAD_DIBITS], uint8_t vcw[2][P2_VCW_BYTES], uint8_t essa[28]);

/* Whether a DUID's payload is scrambled */
static inline bool p2_duid_scrambled(uint8_t duid)
{
    return duid == P2_DUID_4V || duid == P2_DUID_2V || duid == P2_DUID_SACCH_S || duid == P2_DUID_FACCH_S;
}
/* A whole burst: ISCH (S-ISCH or I-ISCH code word), DUID, payload scrambled with mask (NULL: never) */
void p2_burst_build(uint64_t isch, uint8_t duid, const uint8_t payload[P2_PAYLOAD_DIBITS], const uint8_t *mask,
                    unsigned burst, uint8_t out[P2_BURST_DIBITS]);
/* Back: the DUID (-1 unknown) and the descrambled payload */
int p2_burst_parse(const uint8_t in[P2_BURST_DIBITS], const uint8_t *mask, unsigned burst,
                   uint8_t payload[P2_PAYLOAD_DIBITS]);

#ifdef __cplusplus
}
#endif
#endif
