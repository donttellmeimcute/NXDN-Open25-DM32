/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DM32_P25_H
#define DM32_P25_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    P25_DUID_HDU = 0x0, P25_DUID_TDU = 0x3, P25_DUID_LDU1 = 0x5,
    P25_DUID_LDU2 = 0xa, P25_DUID_TDULC = 0xf,
    P25_HDU_BYTES = 99, P25_LDU_BYTES = 216, P25_TDU_BYTES = 18,
    P25_TDULC_BYTES = 54, P25_IMBE_FRAMES = 9,
    P25_NAC_ANY = 0xffff
};

/* All frame buffers are MSB-first, packed on-air bits including status symbols.
 * This library has no allocation, device register access, or RF transmitter.
 * Callers provide complete buffers and serialize access to each RX context.
 * Encoders and p25_decode_frame return 0 on success, -1 on invalid arguments
 * or unsupported frame/NID. A decoded frame can still have metadata_valid=0;
 * callers must check that flag and clear_voice before using call information.
 * FEC decoders return a nonnegative correction count, or -1 on failure.
 */
typedef struct {
    uint16_t nac;
    uint16_t talkgroup;
    uint32_t source_id;       /* 24 bits */
    uint8_t manufacturer_id;  /* standard format 0 or 1 */
    uint8_t service_options;  /* emergency bit + priority only; other bits zero */
    uint8_t status_symbol;    /* 2-bit status: 0 for direct subscriber TX */
} p25_call;

typedef struct {
    uint16_t nac;
    uint8_t duid;
    uint8_t corrected_bits;
} p25_nid;

typedef struct {
    p25_nid nid;
    uint8_t metadata_valid;
    uint8_t clear_voice; /* meaningful only with metadata_valid; caller gates audio */
    uint8_t manufacturer_id;
    uint8_t service_options;
    uint8_t link_control_format;
    uint8_t algorithm_id;
    uint16_t key_id;
    uint16_t talkgroup;
    uint32_t source_id;
    uint8_t message_indicator[9];
    uint8_t low_speed_data[2];
    uint8_t low_speed_valid[2];
    uint16_t metadata_corrected_bits; /* inner FEC only; UINT16_MAX if unknown */
    uint8_t metadata_corrected_symbols;
    uint8_t voice_corrected_bits[9];
    uint8_t voice_u0_corrected_bits[9];
    uint16_t imbe[9][8];
} p25_decoded;

uint64_t p25_nid_encode(uint16_t nac, uint8_t duid);
/* Full (63,16,23) BCH correction plus the DUID-specific NID parity check.
 * Rejects codewords >11 bits from the reconstructed 64-bit codeword.
 * Only conventional voice DUIDs listed above are accepted by this API.
 */
int p25_nid_decode(uint64_t air_nid, p25_nid *result);
size_t p25_frame_bytes(uint8_t duid);

int p25_encode_hdu(const p25_call *call, uint8_t out[P25_HDU_BYTES]);
int p25_encode_ldu(const p25_call *call, uint8_t duid,
                   const uint16_t imbe[P25_IMBE_FRAMES][8],
                   const uint8_t low_speed_data[2], uint8_t out[P25_LDU_BYTES]);
int p25_encode_tdu(const p25_call *call, uint8_t out[P25_TDU_BYTES]);
int p25_encode_tdulc(const p25_call *call, uint8_t out[P25_TDULC_BYTES]);
int p25_decode_frame(const uint8_t *frame, size_t bytes, p25_decoded *result);

/* Yazev/nostar frame_vector convention: u0..3=12 bits, u4..6=11 bits,
 * u7=7 bits verbatim. Unlike OP25 imbe_header_* helpers, u7 is NOT shifted.
 * LDU encoder sets u7 bit0 to the required alternating IMBE sync pattern.
 * The channel decoder returns corrections and cannot detect every >t error;
 * audio concealment/gating remains a caller responsibility.
 */
int p25_imbe_encode(const uint16_t u[8], uint8_t cw[18]);
int p25_imbe_decode(const uint8_t cw[18], uint16_t u[8], uint8_t *u0_errors);

/* Public FEC kernels permit independent vector validation. Shortened RS over
 * GF(64), primitive polynomial 0x43, first root 1; parity 8,12,16 only.
 * RS decoder changes its buffer only on success and corrects <=parity/2.
 */
int p25_rs_encode(const uint8_t *data, size_t count, unsigned parity, uint8_t *out);
int p25_rs_decode(uint8_t *codeword, size_t count, unsigned parity);

typedef struct {
    uint64_t sync_shift;
    uint32_t symbols_seen;
    uint32_t sync_acquisitions;
    uint32_t complete_frames;
    uint32_t bad_nids;
    uint32_t truncated_frames;
    uint16_t expected_nac;
    uint16_t symbols;
    uint16_t target_symbols;
    uint8_t max_sync_errors;
    uint8_t inverted;
    uint8_t frame[P25_LDU_BYTES];
    p25_nid nid;
} p25_rx;

/* User NAC 0..fff, or P25_NAC_ANY. Sync tolerance 0..4, clamped to 4.
 * push returns 1 only after a complete HDU/LDU/TDU/TDULC with valid NID.
 * frame/nid remain available until next push. Inverted dibits are normalized
 * with XOR 2. Length, status positions, and partial-frame recovery are checked;
 * metadata and voice FEC checks are performed by p25_decode_frame separately.
 */
void p25_rx_init(p25_rx *rx, uint16_t nac, uint8_t max_sync_errors);
int p25_rx_push_dibit(p25_rx *rx, uint8_t dibit);

#ifdef __cplusplus
}
#endif
#endif
