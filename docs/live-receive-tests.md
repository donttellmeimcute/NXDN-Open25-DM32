<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Live receive tests — November 5, 2026

The DM32 hardware has captured eight consecutive complete P25 Phase 1 voice
units from the owner's Harris XG-100P on 145.500 MHz, NAC `0x293`. Figure 70 RX
RAM reconstruction recovers alternating LDU1/LDU2 with valid BCH NIDs, clear
metadata and zero corrections in metadata or voice FEC. Independent pinned
OP25 channel decoding matches all 72 IMBE parameter blocks. Host IMBE decoding
produces 11,520 samples / 1.44 seconds of audio. This demonstrates a usable
hardware receive route in one bounded bench capture, not live on-radio playback
or longer/repeated-call continuity. The owner listened to the exported audio
and confirmed their voice sounds good. The initial HDU body still fails FEC.

The programming plug disconnects the internal speaker. These tests judge
captured modem data and internal PCM diagnostics; speaker silence while the
cable is attached is not a receive failure. Normal FM reception of P25 sounds
like digital noise. The installed image still has no operational P25 audio mode.
DM32 transmit and persistent settings/data-chip writes remain inhibited.

## Observations and corrections

1. The on-device offline IMBE/CAI diagnostic matches the host reference: 549
   air bytes, 18 PCM blocks, CRC `4e158373`, 2,715 nonzero samples. No RF or
   speaker output is involved. Heap free was 8,144 bytes both before and after;
   worker stack free was 3,160 bytes. The measured maximum encode block took
   58,782 microseconds versus a 20,000-microsecond budget, and an RX unit took
   293,179 microseconds versus 180,000. These elapsed times include preemption;
   the current codec is functionally correct but has not met real-time deadlines.
2. A host PING could race normal RX-duration completion. The client now accepts
   that refusal only after a fresh status confirms the same session is terminal;
   transport failures and real fault reasons remain visible.
3. An RF timing notification stopped an RX experiment. In the restricted
   first-boot image only, RX records/counts that notification without invoking
   any PA/key callback. It is not evidence that RF was emitted. Unrestricted
   controller behavior still faults on that event during RX.
4. Our sync-register packing was wrong. Each comparator holds 24 bits per
   register, not a 16/32 split. Actual readback exposed the missing byte. The
   driver now writes `H=0x805575f5`, `L=0x00ff77ff`; reconstructing the hardware
   words gives the canonical P25 sync `0x5575f5ff77ff`.
5. With that correction, one Harris transmission delivered three 36-byte raw
   records, 30,000 and 30,002 microseconds apart. Figure 70 reconstruction,
   identity dibit mapping, finds valid P25 sync/NID at bit 132. Raw concatenation
   alone does not validate its NID. The available 732 bits from sync are 60 bits
   short of a complete 792-bit HDU. No missing bits are padded or fabricated.
6. A subsequent abnormal-slot warning stopped that capture. The revised
   restricted image counts/retains those warnings during RX instead of ending
   the experiment, including RX_DONE data co-occurring with a warning. Duration,
   lease, overflow and restoration faults remain enforced. This is a bounded
   experiment to establish whether the modem supplies usable continuous data;
   it does not assume a DMR time-slot warning is harmless in every mode.

7. A later transmission delivered 57 consecutive records, sequence 0–56, with
   29,996–30,004 microsecond gaps. Eight complete 1,728-bit LDUs alternate at
   offsets 924, 2652, 4380, 6108, 7836, 9564, 11292 and 13020. All have exact
   sync, NAC 0x293, zero NID corrections, valid clear metadata and zero inner
   and outer FEC corrections. The next LDU is explicitly incomplete. Native
   counters show 57 seen/saved/read, zero dropped, no faults/PA requests,
   nine abnormal warnings retained, and normal duration cleanup.
8. The complete initial HDU's body fails outer FEC even though its NID and the
   following voice units pass. It is not accepted as a valid HDU. Exploratory
   alternate field permutations and circular shifts did not resolve it; none
   were adopted. Late entry through valid LDU1 metadata allows the following
   clear voice run to be evaluated independently. This issue remains open.
9. OP25 commit `3f44d43eb449bb4753247ce848bb42a403823dea`, using its own
   deinterleave table and channel decoder, independently recovers identical
   parameters for all 72 voice blocks, with zero corrections. The fixed-point
   IMBE decoder then produces a private mono 8 kHz, signed 16-bit WAV with
   11,520 samples, peak 30,626 and 9,671 nonzero samples. The owner listened and confirmed it is their voice and sounds good.
   Decoding ran on the Mac; there is
   no claim of on-radio real-time audio. No raw voice or subscriber IDs are
   committed to source control.

## Diagnostic interface

Private command `0x21`, operation 8, returns exactly 56 little-endian u32 words
after the normal status byte: snapshot version 1, prepared, saved-valid, last
event, last-event microseconds, last SYS flags, last RX type, last SYS-event
microseconds, then 24 current and 24 last-event register values. The register
list is fixed by `diag_regs` and host `SNAPSHOT_REGS`; no caller-selected address
or register write is exposed. The reply needs 225 bytes including status.
Event IDs are start=1, SYS=2, TX-slot=3, RX-slot=4 and RF-notice=5. Last-event
values survive cleanup; current values show restored FM state.

`tools/receive_bench.py` pins the upstream protocol module and opens only the
explicit serial port with RTS/DTR inactive. It permits queries, offline codec
diagnostics and RX-only native operations. It refuses TX ARM, LOAD, PTT,
settings/storage, reboot and arbitrary memory/register probes. Each RX session
is at most two seconds with a 250 ms lease. A batch has at most 120 windows,
stops on fault and waits for ordinary FM to become idle between windows.
Raw records and full live receipts remain in ignored private files.

`experiments/modem/analyze_capture.py` performs offline Figure 70 and dibit
mapping checks with the actual local BCH/frame decoder. It splits on session
or sequence gaps and reports incomplete frames explicitly. Concatenation
inside a segment remains a hypothesis: valid NID alone does not prove body
continuity. Complete frames must pass their relevant FEC/metadata checks;
voice correctness and real-time playback need additional measurements.

```sh
cc -shared -fPIC -O2 -Isrc/p25 src/p25/p25.c -o build/receive-analysis/libp25.dylib
python3 experiments/modem/analyze_capture.py \
  --lib build/receive-analysis/libp25.dylib \
  --out logs/NEW_ANALYSIS.json artifacts/PRIVATE_CAPTURE/rx-10.jsonl
```

`experiments/modem/decode_capture.py` exports an offline WAV only from a
single consecutive session, starting with a complete clear LDU1 and alternating
complete LDU1/LDU2. It requires supported clear metadata and zero corrected
voice bits, and refuses gaps or encryption. Build the host codec shared library
from the existing host release archive, then supply both libraries explicitly:

```sh
clang++ -dynamiclib -Wl,-force_load,experiments/codec/out/release/libdm32_imbe.a \
  -o build/receive-analysis/libdm32_imbe.dylib
python3 experiments/modem/decode_capture.py artifacts/PRIVATE_CAPTURE/rx-24.jsonl \
  --cai-lib build/receive-analysis/libp25.dylib \
  --codec-lib build/receive-analysis/libdm32_imbe.dylib \
  --wav artifacts/NEW_PRIVATE_AUDIO.wav --report logs/NEW_AUDIO_REPORT.json
```

## Normal restart and installation

The revised restriction allowlist permits only a normal restart (command 13,
five-byte payload, mode 0 and the existing chip token). ROM-DFU/stub modes and
malformed payloads remain refused. `tools/normal_reboot.py` requires no active
native/codec experiment, observes boot output without answering a ROM loader,
then checks the renewed health receipt and lower uptime. Normal restart passed:
uptime fell from 26,037 to 3,314 milliseconds, with
flags 31 and healthy cable replies. No ROM greeting was observed; hands-free
entry into the installer remains unproven.

All receive revisions use the saved verified previous application file to
compute changed sectors. They omit all pre-install firmware/data-array reads,
protect the first 48 KiB boot region and separate data chip, verify every written
sector, and compare the whole new application before restart. Exact image and
application hashes distinguish revisions; their displayed version is unchanged.
The initial installation record remains historical. See
[current receive results](../results/live-receive-tests.json) for subsequent
installation and hardware checkpoints.

## Validation and remaining gates

Host tests cover the actual C controller/chip/cable paths under ASan/UBSan,
100,000 adversarial events, co-occurring warning/data, no RX PA callback,
retained lease shutdown, fixed snapshot bounds and the terminal PING race.
All 77 host Python tests pass; 14 relevant installer/restart/RX guard tests also
pass on Windows. Offline analysis tests use real encoded HDUs and FEC, checking
complete, truncated, sequence-gap and session-boundary cases. Target builds
have zero compiler warnings and pass task/IRQ static stack checks. Restricted
build bytes reproduce after forced compilation: flash 422,340 bytes, RAM
180,096/211,456 bytes. The unrestricted regression uses 432,388 flash and
182,584 RAM bytes. These build checks do not replace live timing or RF tests.

Next gates are resolving the initial HDU failure, longer and repeated RX
continuity, codec deadline optimization, and actual on-radio audio routing. Native TX buffer handoff and C4FM waveform/deviation/shaping
measurements follow separately. Trunk tracking follows conventional RX/TX.
