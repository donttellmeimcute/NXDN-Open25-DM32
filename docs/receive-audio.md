<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# On-radio P25 decode and playback development

November 6, 2026. The restricted DM32 image decodes the saved Harris P25
capture on the radio and reproduces the exact host-reference PCM checksum.
The owner has confirmed clear live P25 speech after the playback correction.
See the speaker playback investigation and resource presets below.
TX and persistent data-chip writes remain compiled out.

## Hardware and software boundary

The HR_C7000 demodulates C4FM and delivers hardware-decided bits. No software
C4FM demodulator, ADC sample modem or I/Q processing is used. The CPU reverses
Figure 70's PHY-buffer field layout, finds P25 units, applies BCH/RS/Golay/
Hamming checks, decodes IMBE, and sends PCM to the existing hardware audio path.

Registers use base `0x11000000`:

| Offset | Register | RX setting |
|---|---|---|
| 0x100 | WORK_MODE | 0x02, continuous PHY |
| 0x400 | LAYER2_CONTROL | 0x40, passive RX |
| 0x404 | LAYER2_SLOTON | 0, slot scheduling off |
| 0x408 | LAYER2_TXRX_CTRL | 0x41, receive enable |
| 0x39C | SYS_INTERP_MASK | 0x11800, RX_DONE / PHY_READY / abnormal-slot |
| 0x13C / 0x140 | RECV_MS_SYNC_H/L | 0x805575F5 / 0x00FF77FF |
| 0x144 / 0x148 | RECV_BS_SYNC_H/L | same P25 sync |

On SYS_INTERP_LIST (0x398) bit 0x800, the interrupt handler saves RX_TYPE_INFO
(0x390) and exactly 36 bytes at RX RAM base `0x160004B0`, offsets 0x30–0x53
(absolute `0x160004E0`–`0x16000503`). It retains all 288 bits, including the
fields the hardware calls CACH, in each nominal 30 ms record. Figure 70
reconstruction returns air order according to RX_TYPE_INFO[1:0]. Type 3 is
unsupported and causes stream/admission reset. This is a structured PHY record,
not a P25-ready frame or an unrestricted byte FIFO. RF_MODE, modulation bias,
FIR and board calibration remain under RadioFoundry's existing board setup.
The initial HDU body failure from the earlier live capture remains open.

## Decoder choice and memory

The decoder is Pavel Yazev's fixed-point IMBE implementation from pinned
`nostar/imbe_vocoder` source. It is C++ with a C interface, a statically allocated
context, no exceptions/RTTI and no heap use in the codec path. Its actual
CK803S object size is 5,736 bytes (also confirmed on the host); the context
now reserves 6,144 bytes, enforced by compile-time size/alignment checks.
C is not required for a small implementation. Measured
machine code, RAM and deadline behavior determine suitability.

An optional `--speed` build compiles the same audited DSP source together at
O3 so small fixed-point helpers can inline across files. It does not use
fast-math or change the vocoder algorithm. A zero-based FFT indexing repair
preserves every accessed element and removes an original pointer-before-array
construction exposed by optimization. Existing separate-file O2 builds remain
available. A duplicate file-local bit-mask name is renamed only within the
unified translation unit. Source notices and pristine dependencies are retained.

The optimized whole codec object contains 66,082 bytes of text/constants and
12 static bytes; unused sections are removed at final link. The installed
early on-radio application used 458,852 bytes of flash and 188,464/211,456 bytes of main RAM,
leaving 22,992 bytes in that region. After starting the audio task, measured
heap free is 8,144 bytes. The P25 worker has 5,724 bytes of stack free in the
capture replay and the audio task has 5,988. These are observed tests, not all
possible call or codec payloads.

Other implementations were considered without replacing this working engine:
[JMBE](https://github.com/DSheirer/jmbe) is Java;
[DSD-FME](https://github.com/lwvmobile/dsd-fme/blob/audio_work/examples/Install_Notes.md)
uses mbelib; [BLIP25](https://github.com/OpenBLIP25/blip25-vocoder/blob/main/DESIGN.md)
documents a fixed-point, allocation-free Rust core, but this CPU's Rust build
and footprint have not been demonstrated. [MMDVMHost P25Audio](https://github.com/g4klx/MMDVM-Host/blob/master/P25Audio.cpp)
extracts and repairs encoded IMBE rather than synthesizing PCM speech. The
codec interface remains replaceable; no managed runtime was added to the radio.

## Ownership, pacing and faults

One worker decodes outside all interrupts, at priority 3, below the radio and
PCM tasks. The PCM callback only copies a 160-sample block; it never runs IMBE.
A fixed 18-block FIFO (5,760 sample bytes) prebuffers nine blocks, returns
silence and re-buffers after starvation, refuses overflow, and never repeats
stale speech. Producer and consumer exclude interrupts around queue access.

The built-in receive session owns the existing native modem controller through
private internal APIs. It is RX-only, at most 60 seconds / 2,000 records, with
the same 16-record ISR ring and a 500 ms lease renewed by the worker. The
independent timer still stops a stalled worker, overflow or hard deadline.
The ordinary cable ARM remains limited to two seconds and cannot enable this
internal mode. Cable mutators cannot steal or drain the worker's session.
Radio input interrupts it; PTT cannot turn an interrupted RX test into TX.
There is no automatic fault acknowledgment or restart.

Only this receive/audio configuration releases the board's speaker-FIFO hold.
The existing PCM HAL routes through the C7000 prompt FIFO/audio hardware and
speaker amplifier. Normal raw captures retain the hold. Stop/failure shuts down
PCM, releases native ownership and restores the unchanged FM channel. The
programming plug physically disconnects the speaker, so audible checking needs
it removed. No settings are saved; this is a bounded lab mode, not a permanent
P25 channel/UI implementation.

Clear group-voice admission is unchanged: a valid header or late-entry LDU1
followed by clear LDU2 is required. The saved capture's damaged HDU means its
first nine PCM blocks are intentionally muted. The following 63 are decoded;
this differs from the earlier host export that admitted the first LDU1 directly.

## Measured RAM replay

The PC sends 57 saved raw modem records into a fixed 64-record RAM staging
buffer, then the radio runs the complete PHY-layout / CAI / IMBE path. The PC
neither demodulates nor decodes during this test. The host reference uses the
same conservative admission path and is independently runnable through
`experiments/modem/replay_reference.c`.

| Measurement | Decode only | Paced replay with playback |
|---|---:|---:|
| Records / CAI units | 57 / 9 | 57 / 9 |
| PCM blocks / muted blocks | 72 / 9 | 72 / 9 |
| PCM CRC32 | 895f93ce | 895f93ce |
| Nonzero samples | 9,382 | 9,382 |
| Maximum FEC/decode-frame step | 4,575 us | 4,056 us |
| Maximum IMBE block | 12,226 us | 14,958 us |
| Maximum whole PHY-record processing | 99,219 us | 123,485 us |
| Sum of record processing | 435,103 us | 534,973 us |
| Audio callbacks / played blocks | 0 / 0 | 89 / 72 |
| PCM queue overflows / high water | 0 / 0 | 0 / 11 |
| Empty-buffer/rebuffer events | 0 | 1 |

The 20 ms IMBE and 180 ms CAI-unit budgets are met for this replay, including
preemption. Every PCM block and nonzero sample reaches the playback callback.
The single empty-buffer event remains explicit; it is not counted as a dropped
voice block or claimed to be inaudible without listening evidence. Longer,
varied and live RF calls need additional measurement.

## Synchronized live RF check

With the programming plug still connected, the owner transmitted from the
Harris inside a confirmed active session. The DM32 recovered 14 CAI units and
generated 99 PCM blocks: nine muted, zero codec erasures, 11,279 nonzero samples.
All 99 blocks / nonzero samples were consumed by the playback task. Observed
maximum IMBE time was 14,003 us and whole-record processing 123,145 us. There
were zero queue overflows, sequence/timing gaps or unsupported layouts, and one
empty-buffer event. No DM32 RF transmit or persistent write was requested.
This establishes the live RF-to-on-device-decode-to-PCM-callback path, while the
plug physically mutes the internal speaker.

An earlier unplugged speaker attempt ran its full minute with zero P25 records;
the owner heard digital noise but could not confirm transmitting within the
window. A second speaker attempt was deferred because the owner was not ready.
A fresh synchronized unplug/listen check was still pending at that checkpoint;
the later November 6 speaker result supersedes it. `inspect` now snapshots an active session
without waiting or stopping it, with a regression test for that behavior.

## Private command 0x20 extensions

Existing operations 0 (text status) and 1 (synthetic codec diagnostic) remain.
All multibyte fields below are little endian; lengths include the operation.
No arbitrary memory/register access, RF TX or persistent write is exposed.

| Op | Request | Behavior |
|---|---|---|
| 2 | op, expected u16 (3 bytes) | Begin RAM replay staging, 1–64 records |
| 3 | op, index u16, kind u8, raw[36] (40) | Sequential exact-size load; kind 0–2 only |
| 4 | op, playback u8, delay-seconds u8 (3) | Run fully loaded replay; delay 0–15, play 0/1 |
| 5 | op, frequency u32, duration-ms u16, NAC u16, group u16 (11) | RX/playback for 1,000–60,000 ms; frequency must match current FM channel |
| 6 | op (1) | Request stop |
| 7 | op (1) | Version 1, 32 u32 audio/profile words after status; 129-byte reply |

The exact status field order is `tools/p25_audio_bench.py:FIELDS`. The host
transport pins the upstream protocol and permits only these bounded operations
and read-only health/diagnostic queries. Capture gaps and unsupported layouts
are rejected before replay upload. `--no-wait` records `pending=true`; it only
acknowledges start, not a completed reception test.

## Validation and installation

`make receive-audio-test` runs actual replay/receive-worker code with real IMBE
and CAI under ASan/UBSan. Tests cover the known synthetic PCM CRC, paced PCM
callbacks, simulated raw live reception, clean terminators, native release,
queue prefill/wrap/starvation/overflow/drain, malformed lengths and upload bounds.
Native tests also cover the 60-second receive-only bounds, cable exclusion,
already-open FM squelch admission, PTT cancellation and a missing worker lease.
All 82 Python tests and 19 relevant Windows guard tests pass. Existing native,
first-boot guard, default/feature-off builds and static task/IRQ stacks pass.
The first-boot build is identical after forced compilation and has zero target
warnings, unresolved symbols or high-register instructions.

The new image was installed with 113 changed sectors and 3,812 exact mutation
frames. Every changed sector and the whole 999,424-byte padded application
matched; the Mac independently verified its copy. No pre-install program/data
array read was performed. Normal remote restart was tested for installer entry:
it returned to the application without a boot-ROM greeting, attempted zero
flash writes, and a health check passed. A physical power cycle was then used
for installation. Normal remote restart works, but hands-free installation
still does not.

Current artifacts, measurements and the eventual live-speaker verdict are in
[receive-audio results](../results/receive-audio.json). Raw user audio, captures,
unit images and detailed private logs remain excluded from Git.


## November 6 receive-control correction

A synchronized unplugged attempt ended with state 3 / native reason 15
(interrupted), after 1,394 audio callbacks, approximately 27.88 seconds, with
zero P25 records. It did not hit the 60-second duration limit. The owner heard
P25 noise over FM. The old image did not retain which request caused the abort.
The event timing points to unplugging; the board source confirms that the
programming serial line shares PTT, and the laboratory handler previously
canceled on every radio request.

The new internal RX-audio policy consumes both PTT edges without returning to
FM or forwarding a transmit request. Other radio requests still cancel and
restore the channel. Ordinary raw native sessions retain their prior PTT abort
behavior. Tests cover the press/release pair, unchanged state, no sm.ptt flag,
cable ownership exclusion, a channel-retune cancellation, and independent lease
shutdown. The PTT policy applies only while the internal RX-audio owner exists.

Native command 0x21 operation 9 is a read-only 33-byte reply: normal status,
then eight little-endian u32 words: version 1, last request type, last argument,
last request millisecond timestamp, consumed PTT count, cancellation count,
last canceling type and last canceling argument. Counts saturate and reset at
boot. This will distinguish a shared-line event from another control action in
the next physical test; the original interruption's exact request is unproven.

Internal RX-audio now supports up to five minutes / 10,000 records, retaining
the 500 ms worker lease and independent hard deadline. The controller's
millisecond field is u32; ordinary cable ARM's wire layout and two-second bound
remain unchanged. P25 command 0x20 operation 8 has the same 11-byte payload as
operation 5, but its duration field is seconds, 1–300. Operation 5 retains its
millisecond format and original limits. Duration and record-limit completions
are both normal bounded stops; faults remain distinct and latched.

The host live-start command now waits for a new native session to reach RX
before returning, rather than accepting the prior session's state or an armed
mailbox. This and the longer window remove the need to rush a one-minute test.
The corrected build passes 83 Python tests, 20 relevant Windows tests, actual
native/PCM/codec ASan/UBSan tests, forced identical rebuild, zero target warnings,
and default / no-native / P25-off target regressions. Its app uses 459,232 flash
and 188,496 main RAM bytes. See [control results](../results/speaker-control.json)
for installation and physical speaker-test outcomes.
# Speaker playback investigation — 2026-11-06

The owner confirmed intelligible live P25 speech on the radio, but with audible
choppiness. A full five-minute RX test decoded 522 PCM blocks / 78,063 nonzero
samples, with zero erasures, receive gaps or queue overflows. Every block reached
the HAL callback. These are upstream counters: the old driver's hardware FIFO
underrun field is always zero and cannot establish uninterrupted DAC playback.

The playback revision keeps the digital speaker route enabled for the entire
audio session. Silence is sent while buffering; changing the route from inside
the callback previously cleared the driver's queue and invalidated that block.
Replay completion also waits for the driver/FIFO tail, bounded by two seconds.

An optional HAL pacing hook supplies one 32-sample chunk per 4 ms (8 kHz), still
checking the hardware FIFO-full bit. The clock preserves phase through ordinary
scheduling jitter, never retries a successful chunk immediately, and resynchronizes
after a whole missed period without a catch-up burst. No additional task or
interrupt is created. This is a testable hypothesis about FIFO feeding, not a
proven diagnosis of the audible fault. Legacy and paced modes are available in
one image for comparison. Normal non-P25 audio retains legacy FIFO service.

Private command `0x20`, operation 9, takes one byte: 0 legacy / 1 paced (default).
It changes RAM only and is refused while the worker or native controller is busy.
Operation 10 takes no arguments and returns a status byte plus sixteen LE u32s:
version (1), selected pacing, pushed chunks, hardware-full refusals, late paced
deadlines, minimum and maximum successful-push gaps in microseconds, push pairs
under 500 us, discarded callback blocks, generation-invalidated blocks, queued
speaker samples, first/last push times, driver flags (running/routed/held/TX-gated
in bits 0..3), fixed AUDIO_CONTROL readback, and driver callback count. Minimum
gap is UINT32_MAX until two chunks have been pushed. Pacing setup resets these
counters; stop preserves them. Driver callback count is cumulative since boot.
No arbitrary register access, RF transmit or persistent settings writes are added.

`tools/p25_audio_bench.py --pacing paced|legacy` selects a mode before replay/live
and includes the fixed playback snapshot in its report. Inspection leaves a live
session running. Tests cover the actual generated audio driver and prompt FIFO
sample layout using RAM-backed registers, plus the real codec/worker and bounded
controller tests. Hardware listening and timing counters remain the final check.

The revised image is now installed and boot/cable verified; new application
SHA-256 is `cb77f47fc8f32e94070afd13823786393dfed326b9b2c8d09bfb5cb7c0bcdb25`.
No pre-install firmware or data-array reads were requested. Both hardware RAM
replays reproduce CRC 895f93ce / 72 PCM blocks and report no driver discards or
generation drops. Legacy service: 452 chunks, 546 full refusals, 86 push pairs
under 500 us, minimum gap 41 us. Paced service: 452 chunks, no full refusals,
late deadlines or burst pairs, gaps 1,988..6,032 us, average approximately 4,000 us.
These timing measurements establish the changed feeding behavior; they do not
establish the audible cause or correction. A five-minute live speaker test was
started at 2026-11-06T14:02:31Z; the owner reports "works perfectly." The live
test produced 297 PCM blocks / 44,029 nonzero samples, zero erasures, receive
gaps, PCM FIFO underruns/overflows, driver discards, late deadlines or burst
pairs. Max IMBE was 12,643 us; max record processing 104,544 us. The explicit
stop completed and the ordinary radio state returned idle.

Resource budget: this revision adds 80 bytes of static RAM and 624 bytes of
application flash; no task, interrupt, heap allocation, codec or demodulator was
added. Main RAM use is 188,576/211,456 bytes (22,880 unallocated at link time);
FreeRTOS heap minimum/free during this test was 8,144 bytes, inside the reserved
heap. These are distinct headroom figures, not a measured maximum future feature
budget. Total CPU utilization is not yet measured. PCM routing stays active
through silence, so the existing audio task services the FIFO during idle gaps
as well; no claim of minimum idle CPU or battery use is made.

Before broader features, separate the 2,368-byte bench replay buffer and optional
PCM CRC/profiling from the normal receive build, audit stack budgets using static
checks and multiple hardware workloads, and measure per-task CPU use. The
18-block PCM buffer is 5,760 sample bytes; keep its margin until varied signals
and longer calls justify reduction. Preserve the working image as a reference
when making these optimizations.

## Resource presets and CPU accounting — November 6

`tools/build_radiofoundry.py --first-boot --speed-codec --lean --stack-check
--verify-rebuild` builds the lean receive-test preset in its own output directory.
CONFIG_P25_BENCH=n removes replay storage and commands 1..4, the legacy/paced
selector (9), and sample-check selector (12); those commands return UNAVAILABLE.
Live receive, stop, state, playback diagnostics and fixed resource information
remain. Sample CRC/nonzero fields are zero when checks are compiled out; they
must not be mistaken for the checksum of silence. The context allocation shrinks
to 6,144 bytes with audited codec size/alignment assertions unchanged. The lean
worker has a 4 KiB stack; the bench worker retains 8 KiB for the encoder self-test.
Heap reservations follow those stack budgets, leaving the audio queue unchanged.

`--cpu-profile` independently enables CONFIG_CPU_PROFILE. Private operation 11
returns bounded text: resource.version=1, p25.bench, p25.sample_checks, context
capacity/actual object size, cpu.available, cpu.us, and cpu.task.<name> counters
in microseconds. It uses the existing task-handle table and individual raw
FreeRTOS counters under scheduler exclusion. No heap, stack scan, task-status
array, new task or timer is needed. Kernel counter work is excluded when the
switch is off. Command 12 takes one boolean byte and changes per-sample PCM
checks/profiling in RAM only; it is bench-only and refused while busy.

`tools/cpu_bench.py` measures idle, receive/audio idle, or paced RAM replay, with
bounded windows and no TX/settings writes. A warmup creates the audio task before
replay sampling. It reports per-task fractions and RTOS-attributed non-idle time;
IRQ time is charged to whichever task was interrupted, and host-reporting work
is included. These fractions are not ISR-separated utilization or a guaranteed
worst-case feature budget. Comparisons reject clock resets/wrap, changing task
sets and inconsistent counters; restart the measurement image before its clock
reaches an hour. Raw counters use the existing microsecond clock and wrap near
71.6 minutes. The installed working image remains the fallback reference.

The November 6 baseline measurement image passed installation/new-app readback
and cable health. Repeated saved Harris replays matched CRC 895f93ce. RTOS busy
fractions were 17.46% ordinary idle, 28.86% receive/audio idle, 51.46% and 51.37%
checked replay, and 50.93% checks-disabled replay. The empty worker and native
radio service were larger CPU costs than per-sample checks. The lean worker now
waits 5 ms when empty; the radio-task wait cap for internal RX audio is also
5 ms. Ordinary raw tests retain 2 ms. The timer/IRQ lease, 30 ms capture cadence,
500 ms worker lease, speaker queue and 4 ms chunk clock are unchanged. Host final
speaker PCM remains bit-identical (CRC 71608162) between lean and full builds.
The lean measurement preset retains optional runtime counters for comparison;
it uses 180,032 main RAM / 427,568 flash bytes and is now installed/new-app
readback verified on PC and Mac.
The counters add 8 static bytes and consumed 88 reserved-heap bytes in the
11-task bench run; they are excluded from the normal lean preset.

Three hardware receive-idle runs on the lean measuring image reported 18.12%,
18.38%, 18.46% RTOS-attributed busy, versus the reference 28.86%. Median reduction
is 10.47 percentage points. Radio-service time fell from 12.81% to roughly 5.3–5.7%,
and empty worker time from 4.66% to roughly 1.2%. Driver timing remained clean:
no late chunks, bursts, FIFO-full refusals, driver discards or generation drops.
Heap minimum/free remained 8,056 bytes inside the smaller 37,376-byte heap.
The initial ordinary-idle sample had a UI redraw spike, so it is not used for a
cross-image percentage claim. These are workload measurements with interrupt
attribution/reporting overhead, not a worst-case CPU or battery specification.
The final live voice test began at 2026-11-06T14:54:43Z; the owner reports
"sounds perfect." All 270 PCM blocks reached playback. There were no codec
erasures, receive gaps, queue overflows, driver drops, late chunks or burst
pairs; one PCM FIFO rebuffer event was recorded. Max record processing was
102,021 us. Worker stack free reached 1,652/4,096 bytes; heap minimum remained
8,056 bytes. Per-sample CRC/nonzero and per-codeword profiling fields are zero
by design in the lean build. The explicit stop completed and the radio is idle.
The installed measuring image saves 8,544 main RAM and 32,288 flash bytes against
the preserved working reference; the counter-off build saves another 8 static
and 200 flash bytes. Future features should record link and measured task costs
against this baseline, with live audio/fault checks before reducing margins.
