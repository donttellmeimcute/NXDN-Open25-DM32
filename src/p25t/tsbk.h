/* SPDX-License-Identifier: GPL-3.0-or-later */
/* P25 Phase 1 trunking signalling blocks (TIA-102.BAAA framing): the 12-octet
 * TSBK, its CRC, the rate 1/2 trellis code and block interleaver, the TSDU
 * frame that carries one to three of them, and an incremental receiver that
 * finds TSDUs in a dibit stream and reports the outbound status symbols (the
 * inbound channel's idle / busy state for random access).
 *
 * TSBK octets: 0 = LB (last block, bit 7), P (protected, bit 6), opcode
 * (bits 5-0); 1 = manufacturer ID (0 standard); 2-9 = arguments; 10-11 =
 * CRC-CCITT over octets 0-9, inverted. Bits are numbered MSB first from octet 0
 * bit 7 (bit 0) to octet 11 bit 0 (bit 95), as sdrtrunk and OP25 number them.
 *
 * The trellis tables follow OP25's p25p1_fdma.cc (after Ossmann's Wireshark
 * dissector); the decoder here is a full Viterbi decoder instead of their
 * greedy one. No allocation, no RTOS. */
#ifndef P25T_TSBK_H
#define P25T_TSBK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "p25.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    P25T_TSBK_BYTES = 12,
    P25T_BLOCK_BITS = 196,          /* one trellis-coded, interleaved TSBK */
    P25T_DUID_TSDU = 0x7,
    P25T_TSDU_MAX_BLOCKS = 3,
    P25T_TSDU_MAX_BYTES = 90,       /* 48 + 64 + 3 x 196 bits plus status symbols: 720 bits */
};

/* Outbound status symbols (TIA-102.BAAA): the inbound channel's state */
enum { P25T_STATUS_TALKAROUND = 0, P25T_STATUS_BUSY = 1, P25T_STATUS_UNKNOWN = 2, P25T_STATUS_IDLE = 3 };

uint16_t p25t_crc16(const uint8_t *data, unsigned bits);
/* CRC into octets 10-11 */
void p25t_tsbk_seal(uint8_t tsbk[P25T_TSBK_BYTES]);
bool p25t_tsbk_crc_ok(const uint8_t tsbk[P25T_TSBK_BYTES]);

/* 96 bits -> 196 interleaved bits (25 octets, MSB first, last 4 bits zero) */
void p25t_trellis_encode(const uint8_t tsbk[P25T_TSBK_BYTES], uint8_t block[25]);
/* Viterbi; returns the path's bit distance from the received block, -1 on bad input */
int p25t_trellis_decode(const uint8_t block[25], uint8_t tsbk[P25T_TSBK_BYTES]);

/* A whole TSDU: FS, NID (NAC, DUID 7), n blocks (LB set on the last), status symbols
 * (the given value) after every 35 dibits. Returns the frame's length in bits. */
unsigned p25t_tsdu_encode(uint16_t nac, const uint8_t tsbk[][P25T_TSBK_BYTES], unsigned n, uint8_t status,
                          uint8_t out[P25T_TSDU_MAX_BYTES]);

/* Receiver */
typedef void (*p25t_tsbk_cb)(void *ctx, uint16_t nac, const uint8_t tsbk[P25T_TSBK_BYTES]);
/* Another DUID's frame started (voice on a voice channel: HDU, LDU, TDU) */
typedef void (*p25t_frame_cb)(void *ctx, uint16_t nac, uint8_t duid);
/* A status symbol inside a frame, at stream dibit `at` */
typedef void (*p25t_status_cb)(void *ctx, uint8_t status, uint32_t at);

typedef struct {
    uint16_t nac;                   /* P25_NAC_ANY hears every NAC */
    p25t_tsbk_cb on_tsbk;
    p25t_frame_cb on_frame;
    p25t_status_cb on_status;
    void *ctx;
    /* state */
    uint64_t shift;
    uint32_t dibits;                /* stream position */
    uint16_t pos;                   /* dibits since the frame's first */
    uint16_t data_bits;             /* frame data bits collected (status symbols excluded) */
    uint8_t state, inverted, blocks;
    uint64_t nid_bits;
    uint16_t frame_nac;
    uint8_t block[25];
    /* counters */
    uint32_t frames, tsdus, tsbks, bad_crc, bad_nid, other_frames;
} p25t_rx;

void p25t_rx_init(p25t_rx *rx, uint16_t nac, p25t_tsbk_cb on_tsbk, p25t_frame_cb on_frame, p25t_status_cb on_status,
                  void *ctx);
void p25t_rx_dibit(p25t_rx *rx, uint8_t dibit);
/* Retuned: hunt for sync again */
void p25t_rx_reset(p25t_rx *rx);

#ifdef __cplusplus
}
#endif
#endif
