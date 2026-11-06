/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Offline integration test. This executable has no radio device driver. */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include "dm32_imbe.h"
#include "p25.h"
extern "C" {
#include "mbelib.h"
}

static void require(bool ok, const char *message) {
    if (!ok) { std::cerr << message << "\n"; std::exit(1); }
}

static void write_pcm(std::ofstream &out, const int16_t *samples) {
    for (unsigned i = 0; i < 160; ++i) {
        const uint16_t word = static_cast<uint16_t>(samples[i]);
        const char pair[2] = {static_cast<char>(word), static_cast<char>(word >> 8)};
        out.write(pair, 2);
    }
}

int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "usage: codec_pipeline mono-8k.s16le output-prefix\n";
        return 2;
    }
    std::ifstream input(argv[1], std::ios::binary);
    require(input.good(), "cannot open PCM input");
    std::vector<int16_t> samples;
    char pair[2];
    while (input.read(pair, 2))
        samples.push_back(static_cast<int16_t>(static_cast<unsigned char>(pair[0]) |
                         (static_cast<unsigned char>(pair[1]) << 8)));
    require(!samples.empty() && input.gcount() == 0, "empty or odd-byte PCM input");
    const size_t speech_frames = (samples.size() + 159) / 160;
    const size_t ldus = (speech_frames + 8) / 9;
    samples.resize(ldus * 9 * 160, 0);
    const std::string prefix(argv[2]);
    std::ofstream air(prefix + ".p25", std::ios::binary);
    std::ofstream dibits(prefix + ".dibits", std::ios::binary);
    std::ofstream fixed(prefix + ".fixed.s16le", std::ios::binary);
    std::ofstream reference(prefix + ".mbelib.s16le", std::ios::binary);
    require(air.good() && dibits.good() && fixed.good() && reference.good(), "cannot create outputs");

    dm32_imbe_context encoder, decoder;
    dm32_imbe_init(&encoder); dm32_imbe_init(&decoder);
    mbe_parms current, previous, enhanced;
    mbe_initMbeParms(&current, &previous, &enhanced);
    p25_call call = {0x293, 1, 1, 0, 0, 0};
    p25_rx rx;
    p25_rx_init(&rx, call.nac, 0);
    size_t parsed = 0;
    auto emit = [&](const uint8_t *frame, size_t count) {
        air.write(reinterpret_cast<const char *>(frame), count);
        for (size_t i = 0; i < count; ++i)
            for (unsigned b = 0; b < 4; ++b) {
                const uint8_t symbol = (frame[i] >> (6-2*b)) & 3;
                dibits.put(static_cast<char>(symbol));
                if (p25_rx_push_dibit(&rx, symbol)) {
                    require(rx.nid.nac == call.nac, "stream parser NAC mismatch");
                    ++parsed;
                }
            }
    };
    uint8_t hdu[P25_HDU_BYTES], tdu[P25_TDU_BYTES];
    require(p25_encode_hdu(&call, hdu) == 0, "HDU encoding failed");
    emit(hdu, sizeof(hdu));
    size_t words = 0, fixed_samples = 0, reference_samples = 0;
    for (size_t ldu = 0; ldu < ldus; ++ldu) {
        uint16_t u[9][8];
        for (unsigned f = 0; f < 9; ++f)
            require(dm32_imbe_encode(&encoder, &samples[(ldu*9+f)*160], u[f]), "IMBE encoding fault");
        uint8_t frame[P25_LDU_BYTES];
        const uint8_t lsd[2] = {0, 0};
        const uint8_t duid = (ldu & 1) ? P25_DUID_LDU2 : P25_DUID_LDU1;
        require(p25_encode_ldu(&call, duid, u, lsd, frame) == 0, "LDU encoding failed");
        emit(frame, sizeof(frame));
        p25_decoded decoded;
        require(p25_decode_frame(frame, sizeof(frame), &decoded) == 0, "CAI decoder rejected LDU");
        require(decoded.nid.nac == call.nac && decoded.nid.duid == duid, "LDU NID mismatch");
        require(decoded.metadata_valid && decoded.clear_voice, "clear call metadata invalid");
        if (duid == P25_DUID_LDU1)
            require(decoded.talkgroup == call.talkgroup && decoded.source_id == call.source_id,
                    "call identity mismatch");
        else
            require(decoded.algorithm_id == 0x80 && decoded.key_id == 0,
                    "LDU2 clear encryption synchronization mismatch");
        for (unsigned f = 0; f < 9; ++f) {
            for (unsigned j = 0; j < 8; ++j) {
                const unsigned mask = j == 7 ? 0x7e : 0xffff;
                require((u[f][j]&mask) == (decoded.imbe[f][j]&mask), "CAI changed codec parameters");
            }
            int16_t pcm[160], mbe_pcm[160];
            require(dm32_imbe_decode(&decoder, decoded.imbe[f], pcm), "IMBE rejected recovered codeword");
            uint8_t packed[11];
            require(dm32_imbe_pack(decoded.imbe[f], packed), "recovered codeword widths invalid");
            char bits[88], error[128];
            for (unsigned b = 0; b < 88; ++b)
                bits[b] = (packed[b/8] >> (7-b%8)) & 1;
            int errs = 0, errs2 = 0;
            mbe_processImbe4400Data(mbe_pcm, &errs, &errs2, error, bits,
                                   &current, &previous, &enhanced, 3);
            require(error[0] == '\0', "independent mbelib rejected recovered voice parameters");
            write_pcm(fixed, pcm); write_pcm(reference, mbe_pcm);
            for (unsigned i = 0; i < 160; ++i) {
                fixed_samples += pcm[i] != 0;
                reference_samples += mbe_pcm[i] != 0;
            }
            ++words;
        }
    }
    require(p25_encode_tdu(&call, tdu) == 0, "TDU encoding failed");
    emit(tdu, sizeof(tdu));
    require(parsed == ldus + 2, "stream parser did not recover every complete data unit");
    require(rx.bad_nids == 0, "stream parser rejected NID");
    std::cout << "{\n  \"test\": \"host PCM -> full IMBE encoder -> CAI HDU/LDU1/LDU2/TDU -> dibit stream -> frame decode -> fixed and independent mbelib decoders\",\n"
              << "  \"nac\": 659,\n  \"source_id\": 1,\n  \"talkgroup\": 1,\n"
              << "  \"speech_frames\": " << speech_frames << ",\n"
              << "  \"padded_imbe_frames\": " << words << ",\n"
              << "  \"ldus\": " << ldus << ",\n"
              << "  \"complete_stream_frames\": " << parsed << ",\n"
              << "  \"mbelib_rejected\": 0,\n"
              << "  \"fixed_nonzero_samples\": " << fixed_samples << ",\n"
              << "  \"mbelib_nonzero_samples\": " << reference_samples << ",\n"
              << "  \"radio_or_rf_test\": false\n}\n";
}
