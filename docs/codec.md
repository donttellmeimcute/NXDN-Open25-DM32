# Full-rate IMBE codec: implementation and measured software evidence

Status as of 2026-10-26: a real fixed-point IMBE **encoder and decoder** are built, exercised on this Mac, and cross-compiled for CK803S. They have not executed on a DM-32. This does not establish microphone/speaker access, target execution time, or P25 RF interoperability. The target artifact is a relocatable library, not a flashable radio image.

## Pinned sources, provenance and licenses

| Dependency | Exact revision | Role and license |
|---|---|---|
| [nostar/imbe_vocoder](https://github.com/nostar/imbe_vocoder/tree/03423cb00b8e9ba1be164493e8b98b5bd7c1508c) | `03423cb00b8e9ba1be164493e8b98b5bd7c1508c`, committed 2021-11-27; retrieved 2026-10-26 | Pavel Yazev full encoder and decoder; GPL-3.0-or-later notices in DSP sources |
| [szechyjs/mbelib](https://github.com/szechyjs/mbelib/tree/9a04ed5c78176a9965f3d43f7aa1b1f5330e771f) | `9a04ed5c78176a9965f3d43f7aa1b1f5330e771f`, committed 2019-05-28; retrieved 2026-10-26 | Independent, floating-point, host-only IMBE decoder; ISC |
| [OP25 voice encoder fixture](https://github.com/boatbod/op25/blob/3f44d43eb449bb4753247ce848bb42a403823dea/op25/gr-op25_repeater/lib/p25p1_voice_encode.cc#L1283) | `3f44d43eb449bb4753247ce848bb42a403823dea` | Independent DEBUG_TX parameter fixture; OP25's source notices apply; not a normative TIA vector |

Pristine clones remain in `research/codec/{imbe_vocoder,mbelib}`. `experiments/codec/record_sources.py` records original URLs, commits, commit dates, tracked-file sizes and SHA-256 values, and makes deterministic pristine `git archive` tar files. The captured record is [experiments/codec/results/sources.json](../experiments/codec/results/sources.json). Source archive SHA-256 values are:

* IMBE: `a815a6a80cc9b1445fd84bb9ae9d483cf2e2b597c94824b04ff900217159483f` (337920 bytes).
* mbelib: `01cf7640c493e18cf2c1550e27ce9e87f1c752b4aa854d07320744d36db566dd` (2252800 bytes).

These are archives generated from fetched upstream Git objects, not vendor-signed releases. The IMBE repository omits the COPYING file its notices reference; the complete GPL 3 text is retained in `LICENSES/GPL-3.0-or-later.txt` from GNU. mbelib's original notice is retained in `LICENSES/mbelib-ISC.txt`. New bridge/experiment code is GPL-3.0-or-later. Any eventual application distribution must retain notices and corresponding source; compatibility with distributing a patched proprietary stock application has not been resolved.

## Actual implementation boundary

The C API is [src/codec/dm32_imbe.h](../src/codec/dm32_imbe.h), implemented by [src/codec/imbe.cc](../src/codec/imbe.cc). It directly embeds `imbe_vocoder_impl` in caller-owned, aligned storage. The upstream public wrapper allocates its implementation with `new`; that allocating wrapper is deliberately excluded from the build. Placement construction here allocates no heap memory.

`dm32_imbe_encode` accepts 160 signed 16-bit, 8000 Hz PCM samples and returns eight real compressed parameter words. `dm32_imbe_decode` synthesizes 160 PCM samples from those parameters. Both return a detected-fault status; the radio integration must handle a failed operation by cancelling TX or muting RX and resetting the affected codec context. Reserved/invalid pitch and oversized words are rejected and decoded audio is cleared. This is a codec-level guard, not a complete P25 erasure/concealment policy.

The widths are `12,12,12,12,11,11,11,7` bits. `u7` is the **verbatim seven-bit Yazev word**, including the synchronization bit in bit0. `dm32_imbe_pack`/`unpack` concatenate those widths MSB first into eleven bytes and agree with upstream `encode_4400`. The separate CAI code adds FEC, interleave and signaling; eleven codec bytes alone are not a complete P25 voice unit.

OP25's `imbe_header_encode` helper expects a shifted internal `u7`, and its RX adapter shifts back before calling Yazev. Feeding an unshifted raw codec word to that shifted helper corrupts pitch/gain/signaling bits. Independently parsing the raw representation with mbelib matched every tested frame; deliberately shifting `u7` changed pitch in 1641 of 2000 synthetic frames. The CAI adapter uses the verbatim representation consistently; see [protocol.md](protocol.md).

Use separate encoder and decoder contexts, and serialize codec calls in one scheduled task. DSP history is stateful. The upstream `Overflow` and `Carry` primitive flags remain global, so codec calls must not run concurrently or from a competing interrupt. Audio/modem interrupts should move bounded buffers; they should not invoke this codec while a task is processing a frame. The encoder maintains two 621-sample pitch/history buffers and performs lookahead pitch analysis. Its 20 ms call cadence does not imply zero lookahead or a measured 20 ms end-to-end audio latency.

## Audited out-of-tree changes

`build.py` refuses a different commit or modified tracked dependency files. It copies sources into its output directory and checks every replacement's expected occurrence count before patching. Pristine clones remain untouched. The complete recorded delta is [experiments/codec/results/portability.patch](../experiments/codec/results/portability.patch).

* The original synthesis PRNG used a file-global seed even though the implementation class already had an initialized seed member. It now uses that member. Two independently initialized decoders produce identical PCM when interleaved, proving that this tested state no longer leaks between instances.
* `div_s` printed and aborted on illegal arguments. It now records a bounded arithmetic fault and returns; the bridge reports failure and clears output. Illegal division and very large shift tests exercise these paths.
* Intentional modulo phase arithmetic, signed high-word deposition, and saturation-helper overflow intermediates now use explicit unsigned arithmetic. Large shifts are handled before evaluating a shift expression. This removes observed signed-overflow/negative-left-shift UB without changing regression outputs.
* Unused legacy `FILE*` declarations were removed to allow honest freestanding compilation.
* The independent mbelib reference had a real sanitizer diagnostic: interpolation evaluated `log2Ml[57]` when the maximum harmonic was 56, despite that neighbor's zero coefficient. An out-of-tree reference copy clamps the upper interpolation neighbor at the endpoint. Its clone remains pristine. This reference change is separate from target codec code.

The port requires two's-complement 16/32-bit types and arithmetic signed right shift, with compile-time checks. `-fwrapv` and `-fno-strict-aliasing` are defensive build settings. Full host AddressSanitizer/UndefinedBehaviorSanitizer checks additionally run with **`-fno-wrapv`**, without suppressing shift or signed-overflow diagnostics. Active codec arithmetic is fixed point; the target object has no floating-point, libm, allocator, abort, printf or division-runtime dependencies. A typedef named `Float` in the legacy header does not imply floating-point execution.

## Reproduce the host experiments

From the checkout root, with Python 3 and a host C/C++ compiler:

```sh
python3 experiments/codec/build.py
experiments/codec/out/release/codec_harness --frames 2000 --out-prefix experiments/codec/out/release/synthetic
experiments/codec/out/release/codec_pipeline experiments/codec/out/release/synthetic.input.s16le experiments/codec/out/release/synthetic-call
python3 experiments/codec/build.py --full-ubsan
experiments/codec/out/ubsan/codec_harness --frames 2000
```

The script fetches exact pinned dependencies on first use. Synthetic input is generated deterministically in the harness: silence, tone, changing harmonic mixtures, noise, full-scale alternating samples and impulses. Host executables and reference libraries are development tools only; none is proposed as an operational dependency of the radio.

These mathematical-input commands reproduce the complete host codec/CAI gate without macOS speech synthesis or a separately licensed speech file. On a system using GCC instead of Clang, pass `--compiler g++ --c-compiler gcc` to the build script; the CK803S build uses the repository's crosscompiler independently.

Recorded results in `experiments/codec/results/` show:

| Test | Observed result | What it establishes |
|---|---|---|
| 2000 synthetic speech-frame intervals, 40 seconds total | Upstream packed payloads matched 2000/2000; mbelib harmonic count, band count and every voiced/unvoiced harmonic matched 2000/2000 | Encoder payload convention and independently parsed key codec parameters |
| Fundamental frequency comparison | Maximum difference `2.23215e-8` radians | Fixed-point pitch interpretation matches the independent floating-point decoder to tested precision |
| 10000 deterministic arbitrary payloads | 8105 valid-pitch frames synthesized; 1895 reserved-pitch frames muted; zero detected arithmetic faults | Bounded behavior for these malformed/extreme samples; not exhaustive fuzz proof |
| OP25 independent fixture | 100 frames accepted; both implementations recovered 11 harmonics and four bands with matching voicing | Decoder accepts an externally supplied implementation vector |
| Full sanitizers, including `-fno-wrapv` | Zero diagnostics for synthetic, local speech, malformed payload and CAI pipeline runs | Observed host memory/arithmetic behavior after the documented fixes |
| Output preservation across arithmetic patch | Synthetic encoded SHA-256 `87ad1f63e9d8712b994ee7862480c585b00fb5364d0746fcabb040e25dbc4e29`; decoded PCM SHA-256 `141f04a395ec1aec8af5042e6dfb56131d070f5dc953a3f6a2c163096783ce7e`, identical before/after | Recorded regression preserved the intended two's-complement behavior |

The host JSON logs separately record encode and decode min/mean/median/p99/max times. They are Mac measurements with host scheduling effects, not CK803S timing estimates. Identical synthesized PCM is not required between independently written decoders; pitch/harmonic/voicing parameters are compared, and both synthesized outputs are retained locally. No formal perceptual score or listening-panel result was measured.

A local 14-second macOS `say` speech sample was also encoded: 700 frames matched upstream packing and independent mbelib parameter parsing with zero error strings. The original test sentence, exact conversion command, hashes and origin limitations are recorded in [speech-origin.json](../experiments/codec/results/speech-origin.json). The configured default system voice was not pinned, so that synthesis command is not a byte-reproducible corpus. Generated voice recordings stay in ignored local `out/`; no redistribution license for the system voice/audio is asserted. Deterministic mathematical input is the primary redistributable regression input.

For any owned mono signed-16-bit little-endian 8000 Hz PCM input:

```sh
experiments/codec/out/release/codec_harness --input input.s16le --out-prefix experiments/codec/out/release/owned-speech
experiments/codec/out/release/codec_pipeline input.s16le experiments/codec/out/release/owned-call
```

The joint offline pipeline uses NAC `0x293`, source ID 1, talkgroup 1, and status `00` for direct/talkaround operation. It generates HDU, alternating LDU1/LDU2 and TDU, emits a packed `.p25` stream and one-byte-per-dibit `.dibits` replay file, then recovers voice and independently synthesizes it with mbelib. The captured local speech run recovered all **80 complete data units / 78 LDUs / 702 padded IMBE words** with correct clear metadata, unchanged codec parameters except the prescribed sync bit, and zero mbelib errors. This is useful integration evidence; its CAI roundtrip is still our own framing code and **not independent radio interoperability**.

## Real CK803S object build and resource measurements

After building the repository's native `toolchain/bin/csky-elf-*` tools:

```sh
python3 experiments/codec/build.py --target
python3 experiments/codec/analyze_target.py experiments/codec/out/ck803s > experiments/codec/out/ck803s/stack.json
```

The compiler is GCC 14.3.0 with binutils 2.45.1. Flags include `-mcpu=ck803s -mlittle-endian -msoft-float -ffreestanding -O2 -fno-exceptions -fno-rtti`; emitted ELF is little-endian C-SKY ABI v2, architecture `ck803`, CPU `ck803s`. The minimal headers in `experiments/codec/freestanding/include` provide GCC scalar headers, string prototypes and placement new; they do not invent a libc, sysroot or hardware driver. The resulting full codec/bridge object has just one unresolved symbol: `memset`, which the final application must supply and audit.

| Resource | Measured value | Limit of interpretation |
|---|---:|---|
| Full codec/bridge text plus read-only tables | 23394 bytes | Relocatable aggregate before final section garbage collection; no modem/UI application included |
| Static writable data / BSS | 0 / 12 bytes | Does not include caller-owned contexts or audio/modem buffers |
| Actual implementation state per instance | 5736 bytes | Confirmed by host `sizeof` and the target function's `movi r0,5736` disassembly |
| Initial fixed storage capacity per context | 8192 bytes | Historical measurement; reduced to 6144 bytes on November 6 with the same size/alignment assertions |
| Longest known static encode call-frame sum | 3900 bytes | `28+40+2116+1688+28` through encode → pitch estimation → energy → autocorrelation |
| Longest known static decode call-frame sum | 1632 bytes | `36+336+1236+24` through decode → voiced synthesis → cosine |

Stack numbers come from GCC `.su` and callgraph files, not stack painting. External `memset`, application callers, interrupt nesting and any future integration are excluded. They must not be used as an observed whole-radio stack high-water mark or a final allocation bound. Full compiler/resource records are in [target-build.json](../experiments/codec/results/target-build.json) and [target-stack.json](../experiments/codec/results/target-stack.json).

Two independent target output directories produced bit-identical objects after stable source/debug prefix mapping and deterministic GNU `ar rcsD`:

* `libdm32_imbe.a`: `18d6ce5c772472f44f57380c05ecfcdad04e9eaf41241be6344c1cabef612c3a`.
* `codec-all.o`: `49172958f17f1d6e4d42daebfc662c8b4b6ce874fb492efbdf28a237e0e05c1d`.

See [reproducibility.json](../experiments/codec/results/reproducibility.json). The archive is hardware-unverified and has no entry point, target interrupt setup, verified RAM placement or flash container. Never flash an archive, relocatable object, or host replay file as a radio image.

## Remaining target gate and next experiment

No actual radio CPU clock, encode/decode worst-case execution time, stack high-water mark, audio sample rate/routing, interrupt latency, deadline miss, buffer underflow/overflow, battery behavior or speaker/microphone result has been measured. The modest compiled size does not prove that an encoder meets its 20 ms scheduling deadline, nor that stock firmware has enough unused memory. No target overclocking assumption is made.

After read-only backup/recovery and a verified RAM-helper entry path, run the codec on known input vectors in RAM with the transmitter forced off. Measure encode and decode separately using an independently checked timer at the actual radio clock; log each frame's duration, maximum and missed deadline count. Paint dedicated codec/interrupt stacks, exercise extreme input plus representative speech, and record high-water marks. Then repeat with modem/audio interrupts and radio control active, using queue occupancy and underrun/overrun counters. Compare encoded parameters and decoded output statistics with the pinned host vectors before connecting live PCM to the codec. The radio's hardware TX-off guard must remain outside the codec and run on faults, mode changes, reset and PTT release.

RX integration must require accepted NAC/NID, complete frame data and appropriate valid clear-call signaling, and apply an explicit policy to corrected/damaged voice words before speaker playback. A plausible pitch value alone is not evidence that FEC succeeded. Missing frames, reserved pitch, uncorrectable metadata and signal loss need controlled muting/reacquisition. Those call-state, concealment and hardware paths remain unimplemented here. The independent RF source/receiver and disconnected-cable speaker/microphone tests in the main project acceptance criteria are still required.
