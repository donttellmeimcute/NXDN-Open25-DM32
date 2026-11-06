# Hardware completion and experiment procedure

Original October 26 plan; updated November 5: a real `DM32.01.L01.048` unit
has now answered the startup loader and run the pinned temporary RAM monitor.
Both main flash chips were read twice with matching captures and independently
hash-verified Mac copies. See `results/readonly-backup.json`. No firmware was
changed, and the P25 RF/audio tests below remain unperformed. RX and TX
native-path gates remain independently open.

| Required result | Actual evidence | Status |
|---|---|---|
| Independent live source -> DM-32 speaker | Host codec/CAI tests | No hardware test |
| DM-32 microphone/PTT -> independent receiver | Host encoder and independent decoder parameter checks | No hardware test |
| Battery operation, programming cable removed | No physical radio | Unperformed |
| Native VHF/UHF operation | Manufacturer range and exact-image UHF switching map | No RF validation |
| Repeated PTT, speech, loss/reacquisition and transitions | Portable framer/error tests | No device validation |
| Symbol rate, deviation, shaping and protocol measurements | Known bitstreams/comparison tools | No RF measurements |
| Protected deployment and proven restore | Cross-builds, writer audit and matching full-chip backup | Actual flash/restore and image compatibility still unvalidated |

No RF recording, BER result, battery/thermal measurement, microphone capture,
speaker output or target deadline/stack measurement exists. Available WAVs are
locally generated host-development audio, identified in the codec report.

## 1. Identity, backup and custom code execution

Follow `docs/recovery.md`. Record installed version and actual chip capacities;
demonstrate ordinary updater and startup-loader handshakes separately while the
radio works. Read **both entire chips twice**, compare every stable page, export
the codeplug and preserve this unit's calibration/identity/licensing data.
Check high-address reads for wrap/aliasing. Dump length and two equal hashes do
not identify a chip. Keep private unit identifiers local.

Only afterward, run the narrow RAM diagnostic and require actual CRC, volatile
endian and WORK_MODE output, not just an execute ACK. Power cycle and verify
normal operation. This establishes custom execution/UART behavior only.

## 2. Observe initialized stock execution

Trace existing audio/system IRQs using the exact-image map. The register
allowlist in `experiments/modem/register-snapshot.json` has explicit widths;
do not read FIFO/EOI/clear-on-read registers as passive snapshots. Version-specific
stock globals require the initialized application; the ROM probe cannot validate
them. Measure sample rate, buffer ownership, queue bounds, IRQ cadence and watchdog
behavior. Copies of 80 samples into 160-sample rings suggest 10/20 ms at 8 kHz;
actual timing needs measurement. Verify microphone and speaker paths separately.

Observe physical PTT and the existing admission/release sequence. GPIO B11 and
four-poll debounce are static candidates. Do not directly call the callbacks:
service branches can write storage. Identify PA enable and the meaning of
restoration/protection events. FM_PTT alone is not the PA key.

Read actual PLL/divider/fraction state and determine timer behavior. Static
168/168/84 MHz CPU/AHB/APB reasoning has explicit reference/fraction assumptions;
it is not a measurement. Compare codec execution with actual measured audio
deadlines instead of substituting a datasheet maximum.

## 3. Native RX continuity

First validate Figure 70 permutations with independent known DMR data and full
36-byte continuous buffers, retaining CACH, sequence, IRQ flags, type, polarity
and timestamps. `stream_probe.py rx-air` implements layout hypotheses only.

Supply an independent known P25 C4FM stream. Test programmable sync/physical
modes without assuming a P25 sync match changes the DMR frame engine. Require
valid BCH NID, expected NAC and **complete independently comparable** HDU,
LDU1/LDU2 and termination frames across many 36-byte boundaries. Count every
lost, repeated, inserted and reordered bit. Exercise interruption/reacquisition.
Sync detection alone fails this gate.

If the deframer cannot supply the entire stream, characterize internal FM RX
RAM: actual sample rate, DC/low-frequency response, passband, de-emphasis, AGC,
limiting and aliasing. Verify existing board AF/IF/IQ routing; reference
schematics and voice PCM availability do not establish baseband access.
If all viable paths require wiring changes, record the measured limitation
without changing the no-modification requirement.

## 4. Independent TX waveform

Use distinct known 36-byte chunks. The stock routine's identical repeated
patterns can hide handoff failures. `stream_probe.py tx-plan` prepares explicit
chunks/tail padding from packed CAI fixtures; it is not a radio driver.

Audit the physical-TX routine's initialized caller/PA context before invoking
anything. Capture RF with an independent receiver/SDR and verify ownership,
dibit mapping/polarity, status positions, timing, complete fields and absence
of inserted DMR sync/CACH/gaps. Compare full decoded streams and waveform
response independently. Assess 4,800 symbols/s, nominal +/-600 and +/-1,800 Hz
levels and required P25 shaping. Stock DMR levels differ, and changing deviation
does not establish correct shaping. Use applicable current reference test
methods for tolerance limits; none have been invented or measured here.

If needed, characterize internal FM TX RAM injection with reviewed pre-emphasis
and bandpass settings, measuring modulation transfer function and reconstruction
images before implementing a software shaper. All processing stays inside the
unmodified radio; no external operational modem/DSP/audio interface is allowed.

Test PTT release, empty queues, codec errors, missed buffers, mode exit and
watchdog/reset. Every failure must unkey through the actual PA path and return
to RX. A host script cannot provide an operational failsafe after cable removal.

## 5. Embedded voice pipeline and settings

Integrate the codec in one serialized worker. ISRs transfer bounded buffers
and notify; no allocation or blocking processing belongs in the real-time path.
Reset differential state on fault. Measure encode/decode separately under full
modem/audio/control interrupt load: maximum per 160-sample/20 ms frame, deadline
misses, latency, queue high-water marks and under/overflows. Paint stacks and
measure actual high-water marks including nested IRQs. Static estimates of
3,900 bytes encode, 1,632 decode and about 520 for a known nested CAI path are
not whole-application bounds.

Implement explicit call supervision: validated HDU/acquisition, NAC/group and
clear-metadata audio gating, LDU1/LDU2 sequence, damaged/incomplete frame policy,
muting/concealment, termination/reacquisition, live-microphone PTT and release.
Libraries expose the units/errors; retained settings, radio supervision and
battery/thermal protection are still absent. Preserve calibrated power and
admission paths rather than replacing them with fixed DAC values.

Prioritize UHF using the mapped tuning/switching/calibration path. The
[manufacturer lists](https://www.baofengradio.com/products/dm-32uv) native TX
136-174 and 400-470 MHz. Initial conducted/shielded bench settings, subject to
actual target/reference support:

| Setting | Proposed test value |
|---|---|
| RX/TX frequency | 433.450 MHz simplex, no offset |
| Waveform | Conventional clear P25 Phase 1 C4FM group voice |
| RX/TX NAC | `0x293` |
| Source/radio ID and group | `1` and `1` |
| MFID/service options | `0` / `0` |
| Direct-mode status | `0` (unknown/talkaround) |
| Clear signaling | ALGID `0x80`, KID `0`, clear MI |
| RF power | Lowest calibrated stock setting; measure actual output |

These settings have not been retained or tested on a radio. Do not broaden
bands, replace calibration or infer simulcast support.

## 6. Independent live voice and standalone operation

RX: an independent known-good P25 source transmits live speech; record
intelligible output from the DM-32's own speaker. TX: speak into its microphone
and independently decode RF, checking NAC/source/group/clear metadata. Record
reference versions and actual input/output. Self-loop and two copies of this
new firmware do not establish independent interoperability.

Repeat both directions on battery with the programming cable removed. Exercise
short repeated PTT cycles/transitions, then representative sustained speech
within preserved stock operating limits, signal loss/reacquisition, successive
calls and extended receive. Log stuck TX, crashes, accumulated deadline failures
and thermal/battery behavior. Only measured passes advance the completion table.

## 7. Deployment

Finish exact application/protected-region layout, source/license integration,
retained settings and protection behavior before generating an update. Pin all
inputs, preserve correct container semantics, hash the candidate, validate erase/
range limits and implement post-write readback where supported. Install through
the existing connector on the identified target, demonstrate restoration from
its own originals, then repeat standalone RF tests. Compilation, emulation,
host speech and this procedure do not satisfy deployment or radio operation.
