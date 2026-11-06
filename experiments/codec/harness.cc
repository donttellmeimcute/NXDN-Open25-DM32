/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include "dm32_imbe.h"
extern "C" {
#include "mbelib.h"
}
/* Include this last: legacy typedefs.h defines false/true macros. */
#include "imbe_vocoder_impl.h"

namespace {
constexpr double pi = 3.14159265358979323846;
using Clock = std::chrono::steady_clock;

void require(bool test, const char *message) {
    if (!test) {
        std::cerr << message << "\n";
        std::exit(1);
    }
}

std::vector<int16_t> generated_input(size_t frames) {
    std::vector<int16_t> samples(frames * 160);
    uint32_t rng = 0x12345678u;
    for (size_t i = 0; i < samples.size(); ++i) {
        const unsigned type = (i / (160 * 50)) % 6;
        rng = rng * 1664525u + 1013904223u;
        double value = 0;
        if (type == 1)
            value = 12000.0 * std::sin(2 * pi * 300 * i / 8000.0);
        else if (type == 2) {
            const double f = 85.0 + 115.0 * ((i % 8000) / 8000.0);
            value = 5000 * std::sin(2 * pi * f * i / 8000.0)
                  + 3500 * std::sin(2 * pi * 2 * f * i / 8000.0)
                  + 1800 * std::sin(2 * pi * 4 * f * i / 8000.0);
        } else if (type == 3)
            value = static_cast<int16_t>(rng >> 16) / 2;
        else if (type == 4)
            value = (i & 1) ? 32767 : -32768;
        else if (type == 5)
            value = (i % 97 == 0) ? 32767 : 0;
        samples[i] = static_cast<int16_t>(value);
    }
    return samples;
}

void write_samples(std::ofstream &out, const int16_t *samples, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const uint16_t word = static_cast<uint16_t>(samples[i]);
        const char bytes[2] = {static_cast<char>(word & 255),
                               static_cast<char>(word >> 8)};
        out.write(bytes, 2);
    }
}

void print_timing(const char *name, std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    double sum = 0;
    for (double x : samples) sum += x;
    std::cout << "  \"" << name << "_host_microseconds\": {\"min\": " << samples.front()
              << ", \"mean\": " << sum / samples.size()
              << ", \"median\": " << samples[samples.size()/2]
              << ", \"p99\": " << samples[(samples.size()-1)*99/100]
              << ", \"max\": " << samples.back() << "},\n";
}
}

int main(int argc, char **argv) {
    size_t frames = 2000;
    size_t fuzz_frames = 10000;
    std::string input_path;
    std::string out_prefix;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            frames = std::stoul(argv[++i]);
        else if (std::strcmp(argv[i], "--fuzz") == 0 && i + 1 < argc)
            fuzz_frames = std::stoul(argv[++i]);
        else if (std::strcmp(argv[i], "--input") == 0 && i + 1 < argc)
            input_path = argv[++i];
        else if (std::strcmp(argv[i], "--out-prefix") == 0 && i + 1 < argc)
            out_prefix = argv[++i];
        else {
            std::cerr << "usage: codec_harness [--frames N] [--fuzz N] [--input mono-8k.s16le] [--out-prefix PATH]\n";
            return 2;
        }
    }
    require(frames > 0 && frames <= 100000, "frames outside 1..100000");
    require(fuzz_frames <= 1000000, "fuzz frames exceed 1000000");
    std::vector<int16_t> pcm;
    if (input_path.empty()) {
        pcm = generated_input(frames);
    } else {
        std::ifstream in(input_path, std::ios::binary);
        require(in.good(), "cannot open PCM input");
        char pair[2];
        while (in.read(pair, 2))
            pcm.push_back(static_cast<int16_t>(static_cast<unsigned char>(pair[0]) |
                          (static_cast<unsigned char>(pair[1]) << 8)));
        require(!pcm.empty() && in.gcount() == 0, "empty or odd-length PCM input");
        frames = (pcm.size() + 159) / 160;
        pcm.resize(frames * 160, 0);
    }

    dm32_imbe_context encoder, decoder, decoder_duplicate;
    dm32_imbe_init(&encoder);
    dm32_imbe_init(&decoder);
    dm32_imbe_init(&decoder_duplicate);
    imbe_vocoder_impl upstream_encoder;
    mbe_parms current, previous, previous_enhanced;
    mbe_initMbeParms(&current, &previous, &previous_enhanced);
    mbe_parms shifted_current, shifted_previous, shifted_enhanced;
    mbe_initMbeParms(&shifted_current, &shifted_previous, &shifted_enhanced);

    size_t upstream_pack_matches = 0, parameter_matches = 0, voiced_matches = 0;
    size_t shifted_parameter_mismatches = 0, mbelib_rejected = 0, fixed_nonzero = 0, mbe_nonzero = 0;
    double max_frequency_error = 0;
    std::vector<double> encode_us, decode_us;
    std::ofstream encoded, original, fixed, reference;
    if (!out_prefix.empty()) {
        encoded.open(out_prefix + ".imbe88", std::ios::binary);
        original.open(out_prefix + ".input.s16le", std::ios::binary);
        fixed.open(out_prefix + ".fixed.s16le", std::ios::binary);
        reference.open(out_prefix + ".mbelib.s16le", std::ios::binary);
        require(encoded.good() && original.good() && fixed.good() && reference.good(), "cannot create outputs");
    }
    for (size_t f = 0; f < frames; ++f) {
        uint16_t u[8], roundtrip[8];
        uint8_t packed[11], upstream_packed[11], shifted_packed[11];
        int16_t output[160], output_duplicate[160], reference_output[160];
        const auto t0 = Clock::now();
        require(dm32_imbe_encode(&encoder, &pcm[f * 160], u), "codec arithmetic fault while encoding");
        const auto t1 = Clock::now();
        require(dm32_imbe_pack(u, packed), "out-of-range encoder parameters");
        dm32_imbe_unpack(packed, roundtrip);
        require(std::memcmp(u, roundtrip, sizeof(u)) == 0, "codec payload packing mismatch");
        upstream_encoder.encode_4400(&pcm[f * 160], upstream_packed);
        require(std::memcmp(packed, upstream_packed, 11) == 0, "bridge differs from upstream encode_4400");
        ++upstream_pack_matches;

        const auto d0 = Clock::now();
        require(dm32_imbe_decode(&decoder, u, output), "encoder produced invalid pitch");
        const auto d1 = Clock::now();
        require(dm32_imbe_decode(&decoder_duplicate, u, output_duplicate), "duplicate decoder rejected pitch");
        require(std::memcmp(output, output_duplicate, sizeof(output)) == 0, "per-context RNG or decoder state leaked");
        encode_us.push_back(std::chrono::duration<double, std::micro>(t1-t0).count());
        decode_us.push_back(std::chrono::duration<double, std::micro>(d1-d0).count());

        char bits[88];
        for (unsigned b = 0; b < 88; ++b)
            bits[b] = (packed[b / 8] >> (7-b%8)) & 1u;
        const int reject = mbe_decodeImbe4400Parms(bits, &current, &previous);
        require(reject == 0, "mbelib rejected codec-generated parameters");
        const auto *params = reinterpret_cast<imbe_vocoder_impl *>(decoder.storage)->param();
        require(current.L == params->num_harms && current.K == params->num_bands, "independent harmonic/band parameter mismatch");
        ++parameter_matches;
        bool voicing_equal = true;
        for (int l = 1; l <= current.L; ++l)
            voicing_equal = voicing_equal && (current.Vl[l] == params->v_uv_dsn[l-1]);
        require(voicing_equal, "independent harmonic voicing parameter mismatch");
        ++voiced_matches;
        const double fixed_frequency = pi * static_cast<double>(params->fund_freq) / 2147483648.0;
        max_frequency_error = std::max(max_frequency_error, std::fabs(fixed_frequency-current.w0));
        require(std::fabs(fixed_frequency-current.w0) < 0.0001, "independent fundamental-frequency mismatch");

        /* Evaluate the tempting OP25 shifted-u7 convention independently.
         * This observation guards integration, not a claimed air test.
         */
        uint16_t shifted_u[8];
        std::memcpy(shifted_u, u, sizeof(u));
        shifted_u[7] >>= 1;
        dm32_imbe_pack(shifted_u, shifted_packed);
        char shifted_bits[88];
        for (unsigned b = 0; b < 88; ++b)
            shifted_bits[b] = (shifted_packed[b/8] >> (7-b%8)) & 1u;
        const int shifted_reject = mbe_decodeImbe4400Parms(shifted_bits, &shifted_current, &shifted_previous);
        const unsigned pitch = ((u[0] >> 4) & 0xfcu) | ((u[7] >> 1) & 3u);
        const unsigned shifted_pitch = ((u[0] >> 4) & 0xfcu) | ((shifted_u[7] >> 1) & 3u);
        shifted_parameter_mismatches += shifted_reject || pitch != shifted_pitch;
        mbe_moveMbeParms(&shifted_current, &shifted_previous);

        int errs = 0, errs2 = 0;
        char err[128];
        mbe_processImbe4400Data(reference_output, &errs, &errs2, err, bits,
                               &current, &previous, &previous_enhanced, 3);
        mbelib_rejected += err[0] != '\0';
        for (unsigned j = 0; j < 160; ++j) {
            fixed_nonzero += output[j] != 0;
            mbe_nonzero += reference_output[j] != 0;
        }
        if (!out_prefix.empty()) {
            encoded.write(reinterpret_cast<char *>(packed), 11);
            write_samples(original, &pcm[f * 160], 160);
            write_samples(fixed, output, 160);
            write_samples(reference, reference_output, 160);
        }
    }
    /* Negative checks: reserved pitch must mute, and width violations reject. */
    uint16_t invalid[8] = {0xfc0, 0, 0, 0, 0, 0, 0, 6};
    int16_t mute[160];
    require(!dm32_imbe_decode(&decoder, invalid, mute), "reserved pitch accepted");
    for (int16_t s : mute) require(s == 0, "invalid frame not muted");
    invalid[0] = 0x1000;
    uint8_t bytes[11];
    require(!dm32_imbe_pack(invalid, bytes), "oversized parameter silently truncated");

    size_t fuzz_valid = 0, fuzz_reserved = 0, fuzz_faults = 0;
    uint32_t fuzz_rng = 0xb385f673u;
    for (size_t i = 0; i < fuzz_frames; ++i) {
        uint16_t arbitrary[8], unpacked[8];
        for (unsigned j = 0; j < 8; ++j) {
            fuzz_rng = fuzz_rng * 1664525u + 1013904223u;
            const unsigned width = j < 4 ? 12 : j < 7 ? 11 : 7;
            arbitrary[j] = (fuzz_rng >> 16) & ((1u << width) - 1);
        }
        require(dm32_imbe_pack(arbitrary, bytes), "valid-width arbitrary payload rejected");
        dm32_imbe_unpack(bytes, unpacked);
        require(std::memcmp(arbitrary, unpacked, sizeof(arbitrary)) == 0, "random pack roundtrip mismatch");
        const unsigned pitch = ((arbitrary[0] >> 4) & 0xfcu) | ((arbitrary[7] >> 1) & 3u);
        const int ok = dm32_imbe_decode(&decoder, arbitrary, mute);
        if (pitch > 207) {
            require(!ok, "fuzz reserved pitch accepted");
            ++fuzz_reserved;
        } else if (!ok) {
            ++fuzz_faults;
            dm32_imbe_init(&decoder);
        } else {
            ++fuzz_valid;
        }
        if (i % 101 == 0) dm32_imbe_init(&decoder);
    }
    require(L_add(2147483647, 1) == 2147483647, "positive saturating addition broken");
    require(L_add(-2147483647-1, -1) == -2147483647-1, "negative saturating addition broken");
    require(L_sub(-2147483647-1, 1) == -2147483647-1, "saturating subtraction broken");
    require(L_deposit_h(-1) == -65536, "signed high-word deposition broken");
    require(shl(1, 32767) == 32767 && shl(0, 32767) == 0,
            "large shift was not handled before evaluating shift expression");
    require(div_s(-1, 1) == 0 && div_s(1, 0) == 0,
            "invalid fixed-point division did not return bounded fault");

    /* Independently supplied OP25 DEBUG_TX fixture, pinned source:
     * boatbod/op25 3f44d43eb449bb4753247ce848bb42a403823dea,
     * p25p1_voice_encode.cc:1283..1290. This is an implementation test
     * vector, not a TIA conformance vector or an RF capture.
     */
    dm32_imbe_init(&decoder);
    mbe_initMbeParms(&current, &previous, &previous_enhanced);
    for (unsigned i = 0; i < 100; ++i) {
        const uint16_t fixture[8] = {0x09b, 0x088, 0x0cc, 0x621,
                                     0x7b4, 0x02a, 0x04c, static_cast<uint16_t>(i&1)};
        require(dm32_imbe_decode(&decoder, fixture, mute), "independent OP25 vector rejected");
        dm32_imbe_pack(fixture, bytes);
        char bits[88];
        for (unsigned b = 0; b < 88; ++b)
            bits[b] = (bytes[b/8] >> (7-b%8)) & 1;
        require(mbe_decodeImbe4400Parms(bits, &current, &previous) == 0,
                "mbelib rejected OP25 fixture");
        const auto *params = reinterpret_cast<imbe_vocoder_impl *>(decoder.storage)->param();
        require(params->num_harms == 11 && params->num_bands == 4 &&
                current.L == 11 && current.K == 4, "OP25 fixture harmonic/band mismatch");
        for (int l = 1; l <= current.L; ++l)
            require(current.Vl[l] == params->v_uv_dsn[l-1], "OP25 fixture voicing mismatch");
        mbe_moveMbeParms(&current, &previous);
    }

    std::cout << "{\n  \"platform\": \"host-only; no CK803S or RF timing claim\",\n"
              << "  \"frames\": " << frames << ",\n"
              << "  \"pcm_sample_rate\": 8000,\n"
              << "  \"frame_duration_us\": 20000,\n"
              << "  \"state_bytes\": " << dm32_imbe_state_bytes() << ",\n"
              << "  \"context_capacity_bytes\": " << sizeof(dm32_imbe_context) << ",\n"
              << "  \"upstream_pack_matches\": " << upstream_pack_matches << ",\n"
              << "  \"mbelib_harmonic_and_band_matches\": " << parameter_matches << ",\n"
              << "  \"mbelib_voicing_matches\": " << voiced_matches << ",\n"
              << "  \"max_fundamental_frequency_error_radians\": " << max_frequency_error << ",\n"
              << "  \"shifted_u7_pitch_mismatches\": " << shifted_parameter_mismatches << ",\n"
              << "  \"mbelib_error_strings\": " << mbelib_rejected << ",\n"
              << "  \"fixed_nonzero_samples\": " << fixed_nonzero << ",\n"
              << "  \"mbelib_nonzero_samples\": " << mbe_nonzero << ",\n";
    print_timing("encode", encode_us);
    print_timing("decode", decode_us);
    std::cout << "  \"negative_pitch_and_width_checks\": \"pass\",\n"
              << "  \"saturation_large_shift_and_division_fault_checks\": \"pass\",\n"
              << "  \"independent_op25_debug_vector_frames\": 100,\n"
              << "  \"random_payload_checks\": {\"decoded\": " << fuzz_valid
              << ", \"reserved_muted\": " << fuzz_reserved
              << ", \"arithmetic_faults_muted\": " << fuzz_faults << "},\n"
              << "  \"heap_in_codec_path\": false,\n"
              << "  \"rf_or_radio_validation\": false\n}\n";
}
