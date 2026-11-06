<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Conventional Phase 1 integration — 2026-11-05

Scope: FDMA CAI, conventional clear group voice, half-duplex subscriber-side
audio/framing; RadioFoundry DM32 development derivative. Trunk tracking is the
next stage. No radio was connected. There is no operational P25 RF mode yet.

The next development increment now provides a bounded native PHY experiment
interface: raw RX/TX, cable control, counters and fault/lease shutdown. See
[native-modem-tests.md](native-modem-tests.md) and its separate results. This
adds an optional RF test path beyond the offline worker described below;
neither path has run on hardware. The earlier resource figures below describe
the offline-only preset; current native-preset figures are in the new results.

## What is implemented

`src/p25/voice.c` joins the actual fixed-point IMBE encoder/decoder to the CAI
library. One caller-owned codec context is shared between RX and TX; a direction
change resets it. All codec/FEC work runs in one task, never in an interrupt.
This avoids the codec's shared arithmetic flags and saves a second context.
The November 6 context capacity is 6 KiB, guarded against the pinned object's
actual size; the historical build measurements below predate this reduction.

TX accepts live-format 160-sample/20 ms microphone blocks. It holds the HDU until
the first nine-frame LDU is ready, then emits HDU, alternating LDU1/LDU2, and TDU
without inserted DMR CACH or sync. Release finishes a partial LDU using encoded
silence. Empty PTT cancels. An active underrun, overflow or codec fault aborts;
the eventual RF supervisor must unkey immediately. This is not a channel-access
controller: band/power/battery/busy-channel checks, time-out and PA ownership
must remain with the radio core when the hardware mode is bound.

RX requires good metadata, supported MFID, a clear header or encryption-sync
word, and admitted group information. Selective group filtering admits All Call;
group filter zero ignores TGID. NAC F7E and the internal ANY sentinel accept any
NAC. A NAC change, tuning/carrier-loss reset, bad metadata, unsupported raw
layout or missing raw record clears prior admission. Late entry waits for LC
and ESYNC. Ciphered voice is muted; no decryption is implemented. Voice error
thresholds are conservative local policies, not a complete TIA erasure algorithm.
The caller must supply a carrier-loss/hang timer and activity indication.

`phy_stream.c` packs the continuous stream into 36-byte native-PHY blocks and
reconstructs the manual's three supported RX layouts without dropping CACH
bits. It checks sequence continuity including wraparound. A final short TX
block explicitly reports the number of valid symbols; zero-dibit transport
padding is not a CAI terminator. It contains no register writes or RF control.
Actual buffer ownership, interruption timing and hardware drain are unverified.

## RadioFoundry derivative

Pristine upstream: `dfbaf20c400c73e2ccf6edb80af7d1d255bee418`.
`tools/build_radiofoundry.py` creates an independently modified development
checkout under internal `build/radiofoundry-p25`, refuses unexpected edits, and
keeps the original checkout unchanged. Source additions/patch recipes are
tracked in this repository. Original MIT notices remain; added P25/IMBE is
GPL-3.0-or-later. This is a GPL-compatible derivative, not a clean-room MIT-only
upstream submission. Original stock firmware and TIA PDFs are not distributed.

The derivative adds `CONFIG_P25` (default off), a separate low-priority laboratory
worker and private debug command 0x20. Operation 0 queries results; operation 1
requests a test. Repeated requests while busy are refused. Tests use synthetic
PCM in memory and exercise both IMBE directions, CAI framing and the raw-block
adapter. They neither key RF nor route audio nor write flash. Nothing runs
automatically at boot. The command reports RF mode unavailable and hardware
unverified even after a software test passes.

The laboratory worker uses an 8 KiB stack; the development preset reserves an
additional 8 KiB RTOS heap. FM and DMR source paths, framebuffer and the stock
AMBE code/table windows remain. The plain application image contains no AMBE
donor. `--with-local-stock` uses RadioFoundry's own tool to merge the recognized
local DM32.01.01.049 donor into a separate ignored image; this preserves the
codec material needed for DMR without committing it. Neither image was flashed.

Measured application RAM use is about 173 KiB of 206.5 KiB, leaving about
33 KiB in that region (the RTOS heap is already included). The target session
object is 9,312 bytes. This does not reserve a future live speaker queue or
prove the final radio-mode memory budget. Full byte counts/hashes are in
`results/radiofoundry-integration.json` and generated build manifests.

RadioFoundry configures the CPU/AHB for 168 MHz, APB 84 MHz; see its pinned
`docs/spec/hardware-dm32.md` 1.1 and clock driver. This is a source/configuration
fact, not our measurement of an attached unit. Firmware, newlib and IMBE are
built without high registers to match its RTOS port. The linked disassembly has
no r16-r31 instructions and no unresolved symbols. The P25 static stack estimate
is roughly 5.3 KiB against 8 KiB. Static analysis contains library estimates;
on-device timing/stack/heap measurements are still required. Host codec timings
cannot be converted into DM32 timing claims.

## Hardware-first route and its open questions

The preferred route is native PHY hardware modulation/demodulation, with CAI and
IMBE in software. RadioFoundry's modem specification 2.10 reports off-air DMR
tests with raw 36-byte TX and continuous-mode variant 1. That establishes useful
hardware capability, not P25 operation. Its default writes both RAM halves; the
exact independent ownership of the two halves is still not measured there.

The remaining gates are:

1. RX: recover complete independently generated HDU/LDU1/LDU2/TDU units through
   the DMR-oriented deframer, with no inserted/deleted/repeated bits across all
   36-byte boundary phases. Programmable P25 sync alone is insufficient.
2. TX: prove continuous arbitrary CAI delivery and independently measure the
   waveform. Matching baud/dibit mapping and scaling DMR's nominal deviation
   by 25/27 does not prove the required P25 shaping.
3. Measure maximum encode plus CAI cost per 20 ms input and maximum nine-frame
   decode burst per 180 ms, with interrupts/UI active. Then bind mic, speaker,
   radio/PTT supervision, fault unkey and mode transitions; validate RF both ways.

`tools/c4fm_reference.py` is a host measurement oracle, not a software modem
selected for the MCU. It computes the specified combined H(f)P(f) response at
38.4 ksample/s, using numerical integration and ±24-symbol truncation. Tests
bound response approximation at selected frequencies and independently check
the 1.2 kHz deviation calibration pattern. The comparison tool reports both
absolute error and fitted gain/DC offset; fitting cannot hide a deviation error.
It is not a modulation-fidelity or certification measurement system.

An 8 kHz internal FM sample modem remains a fallback if native PHY proves
unusable. It is not implemented by this change. The FM path's filters and
simultaneous mic capture while injecting modulation would need separate tests.

## Standards decisions and exact sources

The saved local baseline was checked against the corpus catalog/gap audit.
These are exact saved editions, not a claim of latest publication or complete
conformance. PDFs control; relevant figures/tables were rendered and inspected.
No standard text or PDF is vendored. The following aliases give full identities:

- **CAI:** TIA-102.BAAA-B (2017), `p25_d1460d9ae0713992`;
  `documents/tia-library/8.15_TIA-102.BAAA-B_Final Published Document_2017_06_22.pdf`.
- **Conventional:** TIA-102.BAAD-B (2015), `p25_1dc3596fa817c3e5`;
  `documents/tia-library/8.15_ANSI-TIA-102.BAAD-B_Final Published Document_2015_08_05.pdf`.

| Decision | Source clause / figure | Physical PDF page; printed label | Classification |
|---|---|---|---|
| Nine 20 ms voice frames per LDU; HDU, LDU1/LDU2, terminator ordering | CAI 4.1, Figures 9/10; Annex A.1 | 24; 10, and 64; 50 | Normative structure |
| Header metadata and supported standard MFID | CAI 4.2, Figure 11 | 26; 12 | Normative fields; existing CAI FEC implementation |
| ESYNC and group LC metadata | CAI 4.4/4.5, Figures 13/14 | 30; 16 | Normative field roles; full LC/addenda audit remains open |
| TX TGID nonzero; routine source ID 1..9,999,999 | Conventional 6.1.1.1 | 40; 24 | Normative subscriber constraints |
| NAC/TG selection, All Call and F7E receive behavior | Conventional 6.1.1.3, Table 3 | 42-43; 26-27 | Normative group receive basis; this implements a restricted clear-voice subset |
| Dibit levels and 4800-symbol rate | CAI 8.2, Table 20 | 60; 46 | Normative mapping |
| H(f), P(f), deviation calibration | CAI 8.3/8.4/8.4.1, Figure 36 | 60-61; 46-47 | Normative waveform basis; finite numerical oracle is an implementation choice |
| Integrate/dump receiver response | CAI 8.6, Figure 38 | 63; 49 | Reference for a fallback; no MCU sample demodulator implemented |
| 180 ms prefill, three-packet queue, silent partial-LDU padding, stricter late-entry gate, error thresholds | This change | Not applicable | Implementation choices; not attributed to a TIA timer/concealment mandate |

The existing licensed IMBE implementation remains pinned and independently
compared with mbelib; the presence of TIA-102.BABA-A does not constitute a full
codec example/conformance audit. Reserved-values, encryption-protocol and LC
dependencies/addenda require a complete profile audit before interoperability
claims. Missing performance-test documents in the corpus remain gaps. Trunk
control channels, channel plans, grants, retuning, registration and Phase 2 are
not implemented here.

## External implementation lead

The user supplied [KD8TUO's October 29 post](https://x.com/ElectronicsbyJH/status/2115986338017157406).
It explicitly reports P25 operation on a Baofeng DM32 and shows a photograph of
the handset. Follow-up investigation on November 5 accessed the full discussion
through the existing Chrome session after the unauthenticated reader hid replies.
The following primary-source replies materially narrow the lead:

- [October 30: firmware-link question](https://x.com/ElectronicsbyJH/status/2116258946898112854):
  he says the club's team is still developing it. No repository or download is given.
- [October 30: implementation question](https://x.com/ElectronicsbyJH/status/2116257753140175290):
  he attributes the work to people within his radio club, without naming them.
- [October 30: factory-firmware question](https://x.com/ElectronicsbyJH/status/2116327277529358617):
  he explicitly rules out factory firmware.
- [October 31: RX/TX and Phase 1/2 question](https://x.com/907tothe703/status/2116674426196615334):
  no answer appeared in the inspected discussion. A question about repeater
  cross-mode conversion was also unanswered there. Do not infer the RF/codec path.

The supported identification is an unnamed non-factory club development build.
Its project name, revision, base firmware, developers, release plans, license and
source availability remain unknown. Development status does not prove that its
source is private or that a public release does not exist. The photo and caption
are not an independent audio/RF demonstration, and do not establish a hardware
P25 modem, RX and TX, trunking, or Phase 2.

Search scope: his profile's linked [development site](https://www.metastable.dev/),
the linked `hongselectronics` GitHub account's public repository list, relevant
public posts, original replies and quote activity, GitHub repository/code/commit
searches for DM32/DM-32/Baofeng/HR_C7000 with P25, and RadioFoundry branches,
issue #99 and its comments. No attributable downloadable firmware or reusable
native-P25 implementation was located. His January Pocket25 endorsement points
to an Android/RTL-SDR project, not proof of the October handset's firmware.

The public sources found must be distinguished from the desired implementation:

| Source | Finding | Reuse implication |
|---|---|---|
| [RadioFoundry native PHY driver](https://github.com/RadioFoundry/RadioFoundry/blob/dfbaf20c400c73e2ccf6edb80af7d1d255bee418/drivers/modem/hrc7000/hrc7000_bs.c) | Actual raw 36-byte DMR TX driver, continuous-mode option; already our pinned base | MIT; usable native-PHY starting point. Not a verified P25 receive/transmit configuration |
| [RadioFoundry issue #99](https://github.com/RadioFoundry/RadioFoundry/issues/99) | Additional digital modes including P25 are a survey/spike proposal; no comments were returned | No connection to KD8TUO's build or native P25 implementation established |
| [HR_C70X0SDKlib](https://github.com/dongrat/HR_C70X0SDKlib) | Root contains only README and Apache-2.0 LICENSE | No SDK implementation or modem source to reuse at the inspected revision |
| [SarahRoseLives/dm-32uv](https://github.com/SarahRoseLives/dm-32uv) | Three files: programming-protocol memory map, serial log, sniffer script; memory map has no P25 discussion | Programming research, not handset P25 firmware; no declared repository license found |
| [dm32uv-qemu](https://github.com/awkaplan/dm32uv-qemu) | CPU/peripheral/UI emulator; README explicitly excludes RF, audio, vocoder and modem data paths | Potential separate software-test lead, not a native modem oracle. PolyForm Noncommercial overlay, not a GPL-compatible firmware component |

No new third-party implementation was copied into the firmware. No messages,
replies or contact requests were sent. The next information needed from the
authors is a project/repository/branch and license, confirmed conventional
Phase 1 RX/TX scope, and the native PHY RX/TX/filter/deviation setup or related
register notes. Those would resolve the key hardware-offload uncertainty.
