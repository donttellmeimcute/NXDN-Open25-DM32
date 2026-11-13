/* SPDX-License-Identifier: GPL-3.0-or-later */
/* NXDN 4800 bit/s (NXDN48, 6.25 kHz) conventional voice: the frame layer.
 *
 * A frame is 80 ms, 384 bits = 192 4FSK symbols at 2400 symbols/s:
 *   FSW 20 | LICH 16 | SACCH 60 | 4 x 72 voice (or FACCH1 2 x 144)
 * Everything after the frame sync word is scrambled with the PN9 sequence
 * (symbol polarity). Bits are MSB first in byte arrays, in air order; a dibit
 * is two consecutive bits (01 +3, 00 +1, 10 -1, 11 -3).
 *
 * The channel coding follows NXDN TS 1-A as MMDVMHost implements it
 * (G4KLX, GPL-2.0-or-later; research/protocol/MMDVMHost NXDN*.cpp, pinned in
 * docs/protocol-sources.json): the convolutional code, puncturing,
 * interleaving and CRCs below are ported from there. The layer 3 field
 * positions were checked against dsd-fme's VCALL decoder. No allocation, no
 * RTOS, no hardware: host tests run it unchanged.
 */
#ifndef NXDN_H
#define NXDN_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    NXDN_FRAME_BITS = 384, NXDN_FRAME_BYTES = 48, NXDN_FRAME_SYMBOLS = 192,
    NXDN_FSW_BITS = 20, NXDN_LICH_BITS = 16, NXDN_SACCH_BITS = 60,
    NXDN_FACCH1_BITS = 144, NXDN_VOICE_BITS = 72,
    NXDN_SACCH_OFFSET = 36, NXDN_PAYLOAD_OFFSET = 96,   /* bit offsets in the frame */
    NXDN_SACCH_PAYLOAD_BITS = 18,                      /* layer 3 bits one SACCH carries */
    NXDN_LAYER3_BYTES = 10,                            /* FACCH1 data (80 bits) */
    NXDN_AMBE_BITS = 49
};
#define NXDN_FSW 0xCDF59u            /* 20 bits: -3 +1 -3 +3 -3 -3 +3 +3 -1 +3 */

/* LICH: RF channel type, functional channel type, option, direction (TS 1-A) */
enum { NXDN_RFCT_RCCH = 0, NXDN_RFCT_RTCH = 1, NXDN_RFCT_RDCH = 2, NXDN_RFCT_RTCH_C = 3 };
enum { NXDN_USC_SACCH_NS = 0, NXDN_USC_UDCH = 1, NXDN_USC_SACCH_SS = 2, NXDN_USC_SACCH_SS_IDLE = 3 };
enum { NXDN_STEAL_FACCH = 0, NXDN_STEAL_FACCH1_1 = 1, NXDN_STEAL_FACCH1_2 = 2, NXDN_STEAL_NONE = 3 };
enum { NXDN_DIR_INBOUND = 0, NXDN_DIR_OUTBOUND = 1 };
/* SACCH structure: the superframe position of an 18-bit layer 3 chunk */
enum { NXDN_SF_SINGLE = 0, NXDN_SF_4_4 = 0, NXDN_SF_3_4 = 1, NXDN_SF_2_4 = 2, NXDN_SF_1_4 = 3 };
/* Layer 3 message types used by conventional voice */
enum { NXDN_MSG_VCALL = 0x01, NXDN_MSG_TX_REL = 0x08, NXDN_MSG_IDLE = 0x10 };
/* VCALL call types (3 bits) */
enum { NXDN_CALL_BROADCAST = 0, NXDN_CALL_GROUP = 1, NXDN_CALL_INDIVIDUAL = 4 };
#define NXDN_GROUP_ALL 0xFFFFu        /* destination that every unit hears */

typedef struct {
    uint8_t rfct, fct, option, direction;
} nxdn_lich;

/* 8-bit LICH with even parity of RFCT and FCT in bit 0 */
uint8_t nxdn_lich_pack(const nxdn_lich *l);
/* Returns false on a parity error */
bool nxdn_lich_unpack(uint8_t raw, nxdn_lich *l);
/* LICH in a frame: each bit as a dibit (b, 1) */
void nxdn_lich_put(uint8_t frame[NXDN_FRAME_BYTES], uint8_t raw);
/* Majority of the dibits' first bits; *off_bits counts second bits that are 0 (0 for a clean LICH) */
uint8_t nxdn_lich_get(const uint8_t frame[NXDN_FRAME_BYTES], unsigned *off_bits);

/* PN9 symbol scrambler over the 364 bits after the FSW (involution). */
void nxdn_scramble(uint8_t frame[NXDN_FRAME_BYTES]);
/* The 48-byte XOR mask (first 20 bits zero) */
const uint8_t *nxdn_scrambler_mask(void);

/* CRCs over the first `bits` bits of buf, MSB first */
uint8_t nxdn_crc6(const uint8_t *buf, unsigned bits);
uint16_t nxdn_crc12(const uint8_t *buf, unsigned bits);
uint16_t nxdn_crc15(const uint8_t *buf, unsigned bits);

/* Rate-1/2 K=5 convolutional code (G1 = 1 + D3 + D4, G2 = 1 + D + D2 + D4) */
void nxdn_conv_encode(const uint8_t *in, unsigned in_bits, uint8_t *out);
/* Hard-decision Viterbi. symbols: one byte per coded bit: 0, 1, or 2 = erased
 * (punctured). Returns the path metric (estimated corrected bit count). */
unsigned nxdn_viterbi(const uint8_t *symbols, unsigned in_bits, uint8_t *out);

/* SACCH: 26 bits (RAN 6, structure 2, 18 data bits) */
typedef struct {
    uint8_t ran, structure;
    uint32_t data;   /* 18 bits */
} nxdn_sacch;
void nxdn_sacch_encode(const nxdn_sacch *s, uint8_t frame[NXDN_FRAME_BYTES]);
bool nxdn_sacch_decode(const uint8_t frame[NXDN_FRAME_BYTES], nxdn_sacch *s, unsigned *corrected);

/* FACCH1: 80 data bits; which = 0 (first) or 1 (second half of the payload) */
void nxdn_facch1_encode(const uint8_t data[NXDN_LAYER3_BYTES], uint8_t frame[NXDN_FRAME_BYTES], unsigned which);
bool nxdn_facch1_decode(const uint8_t frame[NXDN_FRAME_BYTES], unsigned which, uint8_t data[NXDN_LAYER3_BYTES], unsigned *corrected);

/* Layer 3 VCALL / TX_REL (TS 1-C): the fields conventional voice uses */
typedef struct {
    uint8_t type;            /* NXDN_MSG_VCALL / NXDN_MSG_TX_REL */
    uint8_t cc_option;       /* bit 7 emergency */
    uint8_t call_type;       /* NXDN_CALL_* */
    uint8_t voice_option;    /* bit 4 duplex, bits 2-0 transmission mode: 0 = 4800 bit/s EHR */
    uint16_t source, destination;
    uint8_t cipher_type, key_id;
} nxdn_call_msg;
void nxdn_layer3_encode(const nxdn_call_msg *m, uint8_t data[NXDN_LAYER3_BYTES]);
bool nxdn_layer3_decode(const uint8_t data[NXDN_LAYER3_BYTES], nxdn_call_msg *m);

/* 72-bit voice slot k (0..3) of a voice frame */
void nxdn_voice_put(uint8_t frame[NXDN_FRAME_BYTES], unsigned k, const uint8_t slot[9]);
void nxdn_voice_get(const uint8_t frame[NXDN_FRAME_BYTES], unsigned k, uint8_t slot[9]);

/* Bit helpers (MSB first) */
static inline unsigned nxdn_bit(const uint8_t *b, unsigned i) { return (b[i >> 3] >> (7 - (i & 7))) & 1u; }
static inline void nxdn_set_bit(uint8_t *b, unsigned i, unsigned v)
{
    uint8_t m = (uint8_t)(0x80u >> (i & 7));
    b[i >> 3] = (uint8_t)(v ? b[i >> 3] | m : b[i >> 3] & ~m);
}
void nxdn_put_fsw(uint8_t frame[NXDN_FRAME_BYTES]);

/* What a transmission sends before its first frame, symbol i of n (n >= 12):
 * +3 / -3 dotting, then the 24-bit preamble 0x5775FD that MMDVM's NXDN
 * transmitter sends before the first frame (NXDNTX.cpp NXDN_PREAMBLE). MMDVM
 * sends 0x5F bytes (+3 +3 -3 -3) ahead of it; that tone at a quarter of the
 * symbol rate carries no timing for a Gardner detector, half the symbol rate
 * does (docs/nxdn-modem.md). */
#define NXDN_PREAMBLE_SYMBOLS 48u
uint8_t nxdn_preamble_dibit(unsigned i, unsigned n);

#ifdef __cplusplus
}
#endif
#endif
