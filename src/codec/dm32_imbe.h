/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DM32_IMBE_H
#define DM32_IMBE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { DM32_IMBE_SAMPLES = 160, DM32_IMBE_BYTES = 11,
       DM32_IMBE_CONTEXT_BYTES = 6144 };

/* One static context may be shared by half-duplex RX/TX. No heap is used. All calls
 * must be serialized in a single task: upstream basic_op flags are global.
 * PCM is signed 16-bit at 8000 samples/second (20 ms per call).
 */
typedef union dm32_imbe_context {
    uint64_t alignment;
    uint8_t storage[DM32_IMBE_CONTEXT_BYTES];
} dm32_imbe_context;

void dm32_imbe_init(dm32_imbe_context *context);
size_t dm32_imbe_state_bytes(void);

/* u[0..3] hold 12 bits, u[4..6] 11 bits, and u[7] holds 7 bits.
 * This is the Yazev frame_vector convention, including sync in u[7] bit0.
 * Channel coding adapters must explicitly handle their u7 convention.
 */
/* Return 0 on a detected arithmetic fault; output is cleared. The radio
 * integration must cancel TX on any failed codec operation.
 */
int dm32_imbe_encode(dm32_imbe_context *context,
                      int16_t pcm[DM32_IMBE_SAMPLES], uint16_t u[8]);
int dm32_imbe_decode(dm32_imbe_context *context, const uint16_t u[8],
                     int16_t pcm[DM32_IMBE_SAMPLES]);

/* Pack exactly the upstream nostar encode_4400 88-bit representation,
 * MSB first, concatenating widths {12,12,12,12,11,11,11,7}. This is a
 * codec payload, not a complete or channel-coded P25 air frame.
 */
int dm32_imbe_pack(const uint16_t u[8], uint8_t bytes[DM32_IMBE_BYTES]);
void dm32_imbe_unpack(const uint8_t bytes[DM32_IMBE_BYTES], uint16_t u[8]);

#ifdef __cplusplus
}
#endif
#endif
