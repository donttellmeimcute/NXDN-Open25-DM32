/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <climits>
#include <cstdint>
#include <cstring>
#include <new>
#include "dm32_imbe.h"
#include "imbe_vocoder_impl.h"

static_assert(CHAR_BIT == 8 && sizeof(Word16) == 2 && sizeof(Word32) == 4,
              "IMBE requires 8/16/32-bit integer types");
static_assert(sizeof(imbe_vocoder_impl) <= DM32_IMBE_CONTEXT_BYTES,
              "increase context storage after auditing RAM budget");
static_assert(alignof(imbe_vocoder_impl) <= alignof(dm32_imbe_context),
              "IMBE context alignment is insufficient");
static_assert((-1 >> 1) == -1, "IMBE requires arithmetic signed right shift");

namespace {
const unsigned widths[8] = {12, 12, 12, 12, 11, 11, 11, 7};
unsigned arithmetic_fault;
imbe_vocoder_impl *state(dm32_imbe_context *context) {
    return reinterpret_cast<imbe_vocoder_impl *>(context->storage);
}
}

extern "C" void dm32_imbe_note_fault(void) {
    arithmetic_fault = 1;
}

extern "C" void dm32_imbe_init(dm32_imbe_context *context) {
    /* This also resets the per-instance synthesis RNG in the audited port. */
    new (context->storage) imbe_vocoder_impl();
}

extern "C" size_t dm32_imbe_state_bytes(void) {
    return sizeof(imbe_vocoder_impl);
}

extern "C" int dm32_imbe_encode(dm32_imbe_context *context,
                                  int16_t pcm[160], uint16_t u[8]) {
    int16_t frame[8];
    arithmetic_fault = 0;
    state(context)->imbe_encode(frame, pcm);
    if (arithmetic_fault) {
        std::memset(u, 0, 8 * sizeof(*u));
        return 0;
    }
    for (unsigned i = 0; i < 8; ++i)
        u[i] = static_cast<uint16_t>(frame[i]);
    return 1;
}

extern "C" int dm32_imbe_decode(dm32_imbe_context *context,
                                 const uint16_t u[8], int16_t pcm[160]) {
    int16_t frame[8];
    for (unsigned i = 0; i < 8; ++i) {
        if (u[i] >= (1u << widths[i])) {
            std::memset(pcm, 0, 160 * sizeof(*pcm));
            return 0;
        }
        frame[i] = static_cast<int16_t>(u[i]);
    }
    const unsigned pitch = ((u[0] >> 4) & 0xfcu) | ((u[7] >> 1) & 3u);
    if (pitch > 207) {
        /* The radio layer gates damaged/incomplete voice. Reserved pitch
         * frames are muted here; full P25 erasure/repeat policy is separate.
         */
        std::memset(pcm, 0, 160 * sizeof(*pcm));
        return 0;
    }
    arithmetic_fault = 0;
    state(context)->imbe_decode(frame, pcm);
    if (arithmetic_fault) {
        std::memset(pcm, 0, 160 * sizeof(*pcm));
        return 0;
    }
    return 1;
}

extern "C" int dm32_imbe_pack(const uint16_t u[8], uint8_t bytes[11]) {
    for (unsigned i = 0; i < 8; ++i)
        if (u[i] >= (1u << widths[i]))
            return 0;
    std::memset(bytes, 0, 11);
    unsigned bit = 0;
    for (unsigned i = 0; i < 8; ++i)
        for (int j = static_cast<int>(widths[i]) - 1; j >= 0; --j, ++bit)
            bytes[bit / 8] |= ((u[i] >> j) & 1u) << (7 - bit % 8);
    return 1;
}

extern "C" void dm32_imbe_unpack(const uint8_t bytes[11], uint16_t u[8]) {
    unsigned bit = 0;
    for (unsigned i = 0; i < 8; ++i) {
        u[i] = 0;
        for (unsigned j = 0; j < widths[i]; ++j, ++bit)
            u[i] = static_cast<uint16_t>((u[i] << 1) |
                ((bytes[bit / 8] >> (7 - bit % 8)) & 1u));
    }
}
