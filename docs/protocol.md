# P25 Phase 1 portable CAI implementation

Status: software tested and CK803S cross-compiled; **no DM-32 hardware or RF interoperability test has run**. The code is a real framing/FEC component, not a complete radio firmware image. It performs no external processing for an operating radio; the host tools here generate development vectors and run tests only.

## Source and evidence

Pristine references are under `research/protocol/`, with original URLs, pinned commits, per-file sizes/SHA-256 and licenses in `docs/protocol-sources.json`. Fetch the same inputs with `sh tools/fetch_protocol_sources.sh`; it refuses a changed or different checkout.

| Reference | Pin | Use |
|---|---|---|
| [OP25](https://github.com/boatbod/op25/tree/3f44d43eb449bb4753247ce848bb42a403823dea) | `3f44d43eb449bb4753247ce848bb42a403823dea`, retrieved 2026-10-26 | Independent packet oracle, voice-channel FEC and interleave comparison, shortened RS decoder oracle |
| [MMDVM](https://github.com/g4klx/MMDVM/tree/046e1df16105cbc33de3da7b7d6a03af402ed81d) | `046e1df16105cbc33de3da7b7d6a03af402ed81d`, retrieved 2026-10-26 | Frame lengths, modem/reference waveform investigation; no target code copied |
| [Tait Introduction to P25](https://www.taitradioacademy.com/wp-content/uploads/2016/02/TRG-00001-01-M_Intro_to_P25.pdf) | TRG-00001-01-M, Issue 1, October 2015; retrieved 2026-10-26 | Background only; not a current full normative CAI specification |
| [Zetron P25 Radio Systems Training Guide](https://www.zetron.com/wp-content/uploads/2023/12/TG-001-5-0-0-P25-Training-Guide_rebrand.pdf) | TG-001 revision 5-0-0, May 2023, retrieved 2026-10-26; SHA-256 `5b9126a7d3b3d4cc8e6199b64002ddaab4265a79643c64e80ca2d8e1b7ce37e0` | Manufacturer CAI diagrams and status definitions, particularly printed pages 27, 28 and 51-57 |

Exact packet structures come from the pinned implementation, especially OP25 `apps/tx/p25craft.py`: BCH at line 165, RS matrices 199-284, Golay/Hamming/cyclic encoders 286-348, sync/NID 520-536, group LC 584-624, and HDU/LDU/TDU 881-1082. OP25's interleave map is in `lib/op25_imbe_frame.h`, and frame lengths/NID receive conventions in `lib/p25_framer.cc`. The test suite compiles unmodified OP25 headers as a separate C++ oracle and loads only function definitions from pristine p25craft's AST. It does not execute p25craft's CLI or its import-time file creation.

The upstream p25craft header says its test vectors were checked against TIA-102.BAAB-B. **That is the upstream author's claim, not a conformance test performed in this project.** We have not obtained the current complete TIA standard or its licensed normative vectors. Two programs agreeing with code derived from the same standard is useful software evidence, but does not establish RF interoperability or compliance.

The Zetron manufacturer's guide was also visually checked at printed page 51 (PDF page 60): direct/talk-around subscribers use status `00`; `01` means a busy repeater inbound channel. It confirms the HDU's 99-byte size, null padding, outer RS and shortened Golay layout. Printed page 28 identifies both MFID `00` and `01` as standard formats; this library accepts both. The full training guide is retained pristine in `research/protocol/` for local research, not relicensed as project source.

## Implemented behavior

`src/p25/p25.h` is the public C99 API. It has no dynamic allocation, operating-system calls, device addresses, thread creation or transmitter keying.

| Unit | DUID | On-air length | Implementation |
|---|---:|---:|---|
| HDU | 0 | 792 bits / 99 bytes / 82.5 ms | Clear MI/ALGID 0x80/KID 0, standard MFID and selected talkgroup; RS(36,20,17), shortened Golay(18,6,8), padding and status |
| LDU1 | 5 | 1728 bits / 216 bytes / 180 ms | Nine distinct IMBE words, standard group LC with source ID/talkgroup, RS(24,12,13), Hamming(10,6,3), cyclic low speed data |
| LDU2 | A | 1728 bits / 216 bytes / 180 ms | Nine distinct IMBE words, clear sync word, RS(24,16,9), Hamming/cyclic data |
| TDU | 3 | 144 bits / 18 bytes / 15 ms | Simple terminator and padding |
| TDULC | F | 432 bits / 54 bytes / 45 ms | Standard group LC, RS(24,12,13), Golay(24,12,8), padding |

Durations use the required 9600 raw bits/s; these are protocol arithmetic, not measurements of the DM-32 modem. All public packet buffers contain MSB-first packed on-air bits, including a status dibit after every 35 logical data dibits. Sync is `0x5575f5ff77ff`. The NID contains a 12-bit NAC, 4-bit DUID, 47 BCH parity bits and DUID-specific final parity. The NID decoder implements binary BCH correction using GF(64), Berlekamp-Massey and Chien search, reconstructs the complete codeword, and rejects more than 11 differing bits or unsupported DUIDs. The optional receive NAC filter is exact or `P25_NAC_ANY`; protocol-specific reserved NAC behavior is left to the radio's configuration policy.

The RS implementation uses GF(64), polynomial `0x43`, first consecutive root 1, and a bounded error locator plus an at-most 8x9 magnitude solve. It validates all syndromes after correction and commits output only on success. Hard-decision unknown inner codewords fall back to their systematic data so that the outer RS can still repair them. Erasure-assisted RS is not implemented. Like all bounded-distance hard decoders, some error patterns beyond correction capacity can be miscorrected; these are not universally detectable.

`p25_rx_push_dibit` acquires normal or sign-inverted sync, validates NID and NAC, collects an entire supported packet, rejects bad NIDs and discards partial packets when a new sync is found. The caller must copy/process a completed packet before the next input call. `p25_decode_frame` performs inner/outer FEC and exposes `metadata_valid`, `clear_voice` and per-voice correction counts. **The caller must gate speaker audio and track call state; the framer itself does not play audio or establish a call.** Wrong NAC never emits a frame. Unknown/nonstandard link-control formats do not produce valid group metadata. Damaged metadata never asserts clear voice unless successfully recovered. Frames with clear and protected/ciphered metadata are distinguished; no encryption implementation is included.

TX supports standard MFID 0/1 conventional simplex group voice with source ID 24 bits and talkgroup 16 bits. Service options accept only emergency and priority bits; protected, duplex, data and reserved bits are rejected. The caller selects NAC and status symbol explicitly; golden subscriber/direct test vectors use status 0. RF frequency and retained configuration are platform responsibilities and remain unimplemented.

## IMBE parameter convention and an upstream trap

The codec boundary is the Yazev/nostar eight-word vector: `u0..u3` 12 bits, `u4..u6` 11 bits, `u7` 7 bits verbatim. Channel coding produces 144 bits: four Golay(23,12,7) words, three Hamming(15,11,3) words, the required PN masks and seven uncoded final bits. The LDU encoder overrides only `u7` bit0 to alternate the IMBE sync pattern across the 18 frames of LDU1+LDU2.

OP25 `imbe_header_encode`/`decode` use a shifted internal `u7` representation; its decoder adapter explicitly shifts back before calling Yazev. Passing the raw nostar vector straight to that helper loses a bit and moves the pitch bits. The latest inspected OP25 voice-TX adapter appears to make that mistake. Our independent channel oracle passes `raw_u7 << 1` into the OP25 helper and shifts its output back. The companion codec investigation independently verified the raw convention against mbelib harmonic/pitch parsing; see `docs/codec.md`. No ARM codec instructions were transplanted.

The inner voice interleave is expressed as a formula, not a copied 1296-entry map: coded bit `j` goes to local voice position `6*(j%24) + ((j/24) XOR (j&1))`, then the global logical voice start and status gaps are applied. All 1296 positions were compared with OP25. The Golay syndrome table is mathematically generated by enumerating error masks of weight 0..3 for generator `0xc75`, using `src/p25/generate_tables.py`.

## Reproduce the software checks

From the checkout root:

```sh
sh tools/fetch_protocol_sources.sh
python3 tests/protocol/test_p25.py
sh tests/protocol/run_sanitizers.sh
sh tests/protocol/build_target.sh
```

The first test requires a host C/C++ compiler and Python 3. It verifies the exact reference commit, clean checkout, and regenerated mathematical tables before compiling/importing the oracle. It emits deterministic results and fixture hashes to `tests/protocol/results.json`.

Observed pass results on this Mac:

* All 20480 combinations of 4096 NACs and five voice DUIDs match p25craft and decode correctly.
* 1000 random NIDs with 0..11 flipped bits are corrected, and excluded DUIDs are rejected.
* 4200 RS error-recovery cases agree with the independently compiled OP25 ezpwd decoder; encoding agrees with p25craft for all 600 random messages.
* 1000 random IMBE144 channel vectors match the separately compiled OP25 helper, including randomized correctable Golay/Hamming errors. Our error count includes parity errors; OP25's Hamming count omits errors confined to parity.
* 250 complete packets across HDU/LDU1/LDU2/TDU/TDULC match p25craft byte-for-byte, with decoded NAC/clear/group/source/low-speed data checked.
* Normal/inverted input, wrong NAC, two-bit sync damage, invalid dibits, dropped-body reacquisition and damaged inner/outer metadata are exercised. RS repairs up to 8 damaged HDU symbols and 6 damaged LDU1 metadata symbols. A chosen over-capacity damaged header is rejected for metadata.
* AddressSanitizer+UndefinedBehaviorSanitizer pass 100000 arbitrary NIDs/dibits, random packet lengths/data and 3000 damaged LDUs. These are deterministic host tests, not a proof against every malformed input.

The fixture directory has packed synthetic packets using NAC `0x293`, TG `123`, source `123456`, status `0`, and explicit arbitrary IMBE parameters. `clear-call.bin` is HDU+LDU1+LDU2+TDU (549 bytes); it is **not live speech, a radio recording, a firmware image or interoperability evidence**. See the companion codec pipeline for actual host-generated speech through CAI; that remains offline software validation.

## CK803S build and resources

`tests/protocol/build_target.sh` uses the locally built `toolchain/bin/csky-elf-gcc` 14.3.0 and binutils 2.45.1, with `-mcpu=ck803s -mlittle-endian -msoft-float -ffreestanding -std=c99 -Os -fstack-usage`. The result is a genuine little-endian ELF32 C-SKY relocatable object and static archive at `build/p25-ck803s/`; it is deliberately not packaged as a flashable update.

The measured object has approximately 5.9 KB of code and 4.7 KB of read-only data, 10.6 KB combined; writable static data/BSS are 0 bytes. See `build/p25-ck803s/size.txt` for current exact sizes. The main constant is the 4096-byte Golay syndrome table. Public RX state is 256 bytes and a decoded packet is 200 bytes on the host ABI (also confirmed with target object symbol sizes). CK803S per-function `.su` reports max 260 bytes for RS decode; the largest evident nested CAI call chain is frame-decode 164 + RS-decode 260 + locator 96 = 520 bytes, **excluding runtime functions, libgcc, callers, interrupt nesting and compiler-dependent paths**. This is a static compiler estimate, not an observed target stack high-water mark.

Undefined symbols are `__lshrdi3`, `memcpy`, `memmove` and `memset`. Link the matching libgcc and an audited byte-memory runtime. `p25_runtime.h` declares the standard byte-memory dependencies for freestanding compilation; it does not pretend a hosted libc/sysroot exists. There are no float operations or floating-point runtime dependencies in this CAI object.

No execution-time, CPU-frequency, audio deadline, buffer occupancy, power, battery or RF measurements have been made on the radio. The correct next native test is continuous frame acquisition with valid NID and complete byte-exact packets at actual hardware buffer boundaries. TX must independently demonstrate continuous timing, correct dibit/deviation mapping, pulse shaping, no DMR content insertion and clean PTT release. Those gates belong to `docs/modem.md` and the recovery/hardware work.

## Remaining scope and licensing

Still absent here: a DM-32 modem/audio driver, microphone/speaker scheduling, persistent on-radio settings, a call/PTT supervisor, RF tuning/calibration controls, a target interrupt/timer integration, frame-loss audio concealment policy, deadline/stack measurements, a flashable application and real radio tests. RX framer scans for sync inside a partial body to reacquire; an accidental sync pattern in payload can prematurely discard that packet. No claim of general simulcast support is made. Trunking, Phase 2, data service, individual calls, encryption and 700/800 MHz are excluded.

New portable CAI code and generated tables are GPL-3.0-or-later. Packet coding matrices/layout are adapted from p25craft (Copyright 2011 Michael Ossmann, GPL-2.0-or-later), compatible with the chosen GPL 3 distribution. OP25/ezpwd headers used only as separate test oracles retain their original notices in pristine checkouts; no ezpwd decoder implementation is linked into this CAI library. MMDVM remains research/reference only. Preserve these notices and provide corresponding source when distributing any eventual GPL-linked application. No assertion is made that a stock-proprietary application and these GPL components can be distributed together without resolving their licensing terms.
