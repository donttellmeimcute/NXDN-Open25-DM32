# DM-32 conventional P25 progress

## 2026-11-06 transmit handoff trace

Two mic calls recorded both the first 192 modem handoffs and independent
over-the-air I/Q. All 31 complete voice units in each firmware trace decoded
with valid metadata and no bit corrections. On air, the first three bytes of
each 30 ms block instead tracked the previous block: 96.8% and 94.9% of those
symbols matched the previous block, against 24.5% and 23.2% for the intended
block. Separate offline demodulation of the I/Q fits the previous head
(median waveform correlation 0.963 in both calls) and the current remainder.
The TX-slot interrupt won source selection in both calls. The modem's PHY
ready interrupt was about 3.33 ms before RF start; the next accepted TX-slot
handoff was 0.43–0.45 ms after that notification. This points to updating TX
RAM after the modem has already fetched the block head. Substituting the three
intended bytes in the two saved traces increased the number of identifiable
voice units within the comparison window from 11 and 13 to 31 and 31; some
bit and metadata errors remained. The next bench test should separate TX start
from the ongoing buffer writes and test the earlier PHY-ready interrupt for
those writes against I/Q.

The roughly four-symbol step is separate from this corruption. It was
absent in the second call; when present, it appeared 3.5–4.0 s or about
8.1 s into RF, consistently close to a five-second mark in the captured
file. The step is approximately 0.819 ms (41 samples at 50 kHz), pointing
toward a capture discontinuity. An independent capture is needed to locate it.

Both calls raised one RF-start interrupt after the first handoff, and the
PA callback followed about 28.4 ms later. A quick reply to a Harris call then
reproduced a silent call: 146 handoffs at 30 ms, the RF-start interrupt enabled
at the processor, zero RF-start handler calls, zero PA requests and zero PA
callbacks. This rules out an RF-start interrupt discarded by the controller in
that call. PHY ready won source selection in the silent call, while TX-slot
won in both keyed calls. This narrows the failure to modem start or interrupt
delivery; the reason for the missing notification needs a startup trace that
samples the raw modem/PIC state around the first send. Calibration is unchanged.

## 2026-11-06 live P25 mic and PTT

The Harris heard clear speech from the DM-32's own microphone and PTT at
145.500 MHz, NAC 293, talkgroup 1. That first image later restarted. The
retained watchdog record pointed at the P25 encoder worker. The updated worker
blocks for a scheduler tick after encoding each block so the watchdog and
other tasks can run even with a microphone backlog.

The updated development channel keeps P25 receive selected between calls.
Select it once per boot with `python tools/p25_channel.py --port COM5 enable`,
then unplug the programming cable for mic, PTT and speaker tests. `status`
reports the receiver state and the last TX counters. `disable` leaves the
development channel. Selection is stored in RAM. The cable tool does not
provide a way to key the transmitter. PTT release closes and drains the P25
call, and the next physical press can start another. There is no attempt count,
arming expiry, or bench call length limit in this mode.

The HR_C7000 captures microphone PCM and modulates the P25 symbols. The CPU
encodes IMBE and assembles P25 frames, reusing the receiver's PCM buffer. The
independent modem lease, stalled-block and battery shutdowns, shared cable/PTT
filter, and the 145.500 MHz lowest-power restriction still apply. Existing
factory calibration was used for this test.

The first continuous-channel trial did not restart. The DM-32 consistently
received clear speech from the Harris, but the Harris heard intermittent
speech and noise from the DM-32. The last of nine TX attempts stopped on a
hardware queue underrun after sending 83 blocks. The encoder averaged 20.04 ms
for 20 ms of input and exceeded 20 ms on 91 of 126 blocks.

The next image reuses the two pitch windows that overlap consecutive blocks.
The saved PCM storage keeps the cache inside the existing codec context. The
encoded output matched the previous build on speech, synthetic audio, silence
and high-amplitude noise under ASan/UBSan. The build uses 467,216 bytes of flash
and 185,664 bytes of main RAM. The controller and first-boot checks, 101 Python
tests, target stack check and reproducible build passed.

The final image was verified after a cable-only update with application SHA-256
`b8a845c1a24a60b32007dba84d02ef62a2ee10b22c880d32abb7259d5d3bacca`.
The synthetic test averaged 13.0 ms per 20 ms block and peaked at 15.5 ms.
The longest measured live call sent 206 blocks, with 5.8 seconds of mic input,
13.3 ms average encode time, and an 18.6 ms maximum. It ended with one PA key,
zero modem faults, zero PCM overruns, and a peak PCM backlog of one block.
Four local PTT calls were attempted across the final two trials without a
restart. The device returned to P25 receive. The Harris heard the short call
clearly and still had trouble with the longer call.

An independent Airspy/VCE capture showed two strong RF bursts consistent with
the DM-32 transmissions. The first yielded 23 alternating voice units with
valid metadata, although 189 of 207 voice words needed bit corrections. The
second yielded only four voice units with decodable network identifiers, and
one had invalid metadata. The later firmware counters showed a clean 152-block
transmission. The capture provided no separately identifiable Harris reference,
so it establishes residual RF/decode errors without isolating their cause.

## 2026-11-06 spoken P25 test received on the Harris

The Harris decoded a synthesized “Hello” clearly at 145.500 MHz with NAC 293
and talkgroup 1, using the existing modulation settings. The first streamed
attempt lost cable replies and produced only 130 ms of RF. The second attempt
loaded all 26 blocks before keying and produced 738 ms of RF in the independent
Airspy capture. The controller reported all blocks sent, one PA key, zero radio
faults, and return to idle. Cable status replies still timed out during RF.

The explicit `--preload --air` bench path accepts a complete sample of up to
28 blocks before START and uses a device-enforced one-second cutoff. The bench
buffer adds 800 bytes of storage. Ordinary streaming keeps its eight-block
queue and heartbeat. Independent application readback SHA-256 for the installed
voice bench image is
`3849fba3524bdf55d652ecb67660b1d30da5925884a5d6cf9243f9e3dbee6802`.

## 2026-11-06 update: first native hardware TX captured independently

The scoped TX bench profile is installed and independently new-app-verified on
PC/Mac. Its padded application SHA-256 is
9cc64e8bae718fdabfbd33b0d291e1b583b1342a7aea1921b2b87c69f8c01c74.
It keeps ordinary PTT/storage restrictions, but permits the current native
ownership/session to key only on 145.500 MHz with short duration/heartbeat
limits, lowest power index zero and valid factory calibration. Target use is
431,092 flash bytes / 180,296 main RAM bytes, leaving 31,160 main RAM bytes at
link time. This is 264 more RAM bytes than the earlier lean measuring receiver,
including the warm updater. No new PCM or TX queue was added.

The first run stopped safely on a 26,355-us gap between different interrupt
sources, before any PA key. Each run now selects its first boundary source;
other sources are counted without advancing data or renewing its stall clock.
Same-source 27..33-ms timing, the 60-ms stall guard, heartbeat expiry and hard
duration remain. The exact observed sequence and loss of the selected clock
are covered by host regression tests. Real GPIO/DAC/FD6818 paths, board setup,
calibration absence, ownership, PTT, battery loss, late callbacks and storage
denial pass ASan/UBSan tests. Python tests total 100; relevant Windows tests,
RX-only/feature-off builds, stacks, zero warnings and identical target rebuilds
pass. These checks do not establish full RF safety or P25 interoperability.

A four-block constant-pattern run completed without fault, keyed the PA once
and returned idle. VCE/Airspy independently captured a roughly 78-ms RF burst,
about 46 dB above the recorded noise, with a +602-Hz median offset and no clipped
samples. A seven-block fully preloaded level pattern also completed cleanly.
Across captures the approximate stable level offsets were +602, +1,878, -709,
and -2,027 Hz, in the requested order. These are Airspy-based estimates, not
traceable service-monitor measurements. Spacing/center calibration, shaping,
byte-exact continuity and a decoded P25 voice frame remain unverified.

A 24-block streaming attempt sent only its eight preloaded blocks, then stopped
on underrun and restored ownership. Subsequent instrumented short operation
showed an unanswered native heartbeat during RF while other requests and idle
5-ms pings worked. Radio diagnostics show zero UART overruns/line errors/TX
stalls, zero parser CRC/length/timeout errors and no unhandled interrupts. The
exact fault was saved and explicitly acknowledged; there is no automatic reset
or RF retry. Queue feeding/heartbeat transport is the next blocker alongside
level calibration. Do not claim sustained TX or voice TX.

VCE's existing test channel supplied four bounded raw I/Q capture windows. Raw
recording is restored off and the channel remains running; the DM32 is idle
after its clean seven-block run. No Airspy capture software was downloaded.
See results/native-tx.json and docs/development-cycle.md.

## 2026-11-06 update: cable-only update entry and sector installation verified

The development bridge installed and its new application matched independently
on PC/Mac. Physical whole-sector writing took 45.10 s for 105 sectors; full new
application verification took 45.60 s. The warm_probe then entered the pinned
helper at 0x44000, checked configuration/chip identities and executed exactly
seven restart-register writes, with no flash-array reads or flash writes. It
returned to healthy application operation. The following 106-sector application
update used that warm entry, required no power knob, wrote in 41.92 s and fully
verified the new application in 41.63 s. The owner's no-existing-firmware-read
constraint was enforced throughout. This establishes faster iterative entry and
installation, not P25 TX operation. See docs/development-cycle.md.

## 2026-11-06 update: faster development entry prepared, not yet hardware-tested

The next RX-only image retains the working lean receiver and all TX/storage
inhibits. CONFIG_DEV_UPDATE permits the existing runtime RAM-loader interface
only for the same pinned 7,784-byte helper used by the power-on installer.
Ordered chunks, a five-second inactivity lease, native/codec ownership exclusion,
power-off cancellation and a final image CRC limit entry. The host additionally
checks the helper SHA-256. CRC is not a security boundary against a malicious host.
The helper reuses the existing OEM-codec RAM window; added link use is 192 RAM
bytes and 1,960 flash bytes versus the installed lean measurement image
(180,224 RAM / 429,528 flash total). This entry is development-only.

The installer can use protocol-7 whole-sector erase/write/CRC operations: one
exact, preplanned mutation frame per changed application sector, no mutating
retry, then the complete newly written application is compared byte-for-byte
before reset. Bootloader and data-chip writes remain excluded. Saved predecessor
files supply the comparison basis; the owner's prohibition on existing-firmware
reads remains enforced. The prepared bridge needs 105 sector mutations rather
than the generic page protocol's multiple frames per sector. Physical operation
and transfer-time improvement are still unverified.

A separate warm_probe enters the approved helper, checks its configuration and
both JEDEC identities, then issues only the seven fixed restart-register writes.
Its transport rejects all flash-array reads and writes. This is the first planned
warm-entry hardware test after installing the bridge; it avoids a no-op firmware
verification that would read the existing application.

Content-checked codec caching now permits incremental development builds while
hashing sources, compiler, archive, objects and call graphs. Observed incremental
build was 7.21 seconds, versus 12.47 seconds to seed the cache; a forced-rebuild
checkpoint still matched exactly. Development packaging requires an explicit
flag when the forced-rebuild proof is absent. The current bridge package has
that proof. Mac Python tests (94), real-helper upload gate, actual driver guards,
native ownership/fault tests, PCM/codec tests and Windows installer/warm-entry
tests pass. RAM entry and sector writes still require physical verification.

The prepared raw image SHA-256 is
41f6b72f244adc5bd1f14b8ce8eefdbafec765b0796f1d394e786c875f4830b1;
the padded application is af8dcc9726b1f308c0d0423de4d00121378f7dca1622e60877b6b2e578a95811.
The installed radio remains the preceding lean RX-only image. No TX experiment
has been run. The next TX step needs a separate narrowly scoped native-modem
grant while retaining ordinary PTT/storage guards and independent fault timing.

## 2026-11-06 update: resource reduction and optional CPU measurement

The known-good speaker image/source (4936fc0) is preserved internally before
new builds. A lean receive-only preset removes the 2,368-byte replay buffer,
synthetic self-test and per-sample PCM CRC/nonzero scans. The pinned fixed-point
codec context is 6,144 bytes instead of 8,192; existing compile-time size/alignment
assertions enforce fit. The lean worker stack is 4,096 instead of 8,192 bytes,
with a 2,368-byte static worst path plus 196-byte allowance leaving 1,532 bytes.
Its reserved RTOS heap is reduced by the matching 4,096 bytes. The ordinary
DMR/audio task stack and 18-block P25 sample buffer are unchanged.

Lean link use is 180,024 / 211,456 bytes RAM and 427,368 flash bytes: savings
8,552 RAM / 32,488 flash against the working image. Main RAM unallocated at link
time is 31,432 bytes. Encoder/self-test code becomes unreachable in this RX-only
preset and is removed by the linker; this is not an encoder speed improvement.
The full bench preset keeps the deeper encoder stack and replay support.

An optional CPU_PROFILE switch enables existing-clock RTOS task counters with no
new task, interrupt, task-status array or libc statistics formatter. The normal
preset compiles them out. The measurement image can toggle per-sample checks in
RAM while idle, then repeat identical saved captures. Host reports distinguish
RTOS-attributed busy time from pure task/ISR CPU time: interrupts are charged to
the interrupted task. Counter wrap/reset and task-set changes invalidate a run.

Host real-codec/CAI/PCM/lean-worker ASan/UBSan tests and profile-on/off serializer
checks pass, as do 88 Python and 25 relevant Windows tests. Lean and measurement
targets pass zero warnings, static stacks and identical forced rebuilds; feature
variants also build cleanly. The first measurement image is installed/readback verified. CPU replay with
checks on was 51.46% / 51.37% RTOS-attributed busy, versus 50.93% with checks off;
all checked replays matched CRC 895f93ce. Receive/audio idle was 28.86% busy,
with radio service 12.81% and empty worker polling 4.66%. The lean candidate now
uses 5 ms empty-worker and internal RX radio-service waits (raw tests keep 2 ms);
independent hardware IRQ/lease timing and PCM cadence remain unchanged. It is
installed and new-app readback verified on PC/Mac. Three receive-idle runs are
18.12%, 18.38%, 18.46% RTOS-attributed busy versus 28.86% before; median savings
10.47 percentage points. The UI/host workload varies and IRQ time is attributed
to the interrupted task; this is not a codec speedup or worst-case voice budget.
The owner reports the final lean Harris test "sounds perfect." It decoded and
played all 270 PCM blocks with no erasures, receive gaps, queue overflows or
driver drops; one PCM FIFO rebuffer event was recorded. Max record processing
was 102,021 us. Worker stack minimum free was 1,652 bytes of 4,096, heap minimum
8,056 bytes. The test ended cleanly and the radio is idle.
The lean measuring preset adds 8 static
bytes for optional counters (180,032 RAM / 427,568 flash); counters also cost
88 bytes inside the reserved heap in the measured 11-task workload. TX remains disabled.
See results/resource-lean.json and tools/cpu_bench.py.

## 2026-11-06 update: intelligible live speaker audio; pacing revision ready

The owner heard intelligible P25 speech on the DM32 itself, but described it as
choppy, accelerated and/or separated by gaps. The complete five-minute session
decoded 522 PCM blocks with zero codec erasures or receive discontinuities;
all 522 reached the audio callback. Maximum IMBE decode was 16,230 us, whole
record processing 131,323 us, PCM FIFO high-water 13/18, no overflow and one
rebuffer event. Native receive ended normally at its duration limit. No PTT
event or cancellation was recorded, so a physical shared-line cause is still
unproven. These counters do not prove that every sample reached the DAC.

The next image keeps PCM routed throughout the session (silence while waiting),
avoiding the driver's queue-clearing route transitions. It adds a bounded
32-sample/4-ms speaker FIFO pace, a RAM-only legacy/paced selector and sixteen
read-only playback counters. Hardware-full refusals retain the same chunk;
missed deadlines never cause an instantaneous catch-up burst. Replay also waits
for the driver's audible tail. Real generated driver tests exercise the prompt
writer against RAM-backed registers, sample order, gates, full-buffer retries,
legacy bursts and paced 8-kHz service. Actual PCM/CAI/codec ASan/UBSan, Python,
Windows, first-boot guards, target feature variants, stacks and an identical
forced rebuild pass. Added RAM is 80 bytes; total flash 459,856, main RAM 188,576.
The image is installed and its new application is independently readback-verified
on PC/Mac, with cable health passing and no pre-install reads. Both hardware
replays reproduce CRC 895f93ce and deliver all 72 PCM blocks. Legacy feeding
recorded 86 push pairs under 500 us and 546 hardware-full refusals; paced feeding
recorded neither, zero late deadlines or driver block drops, and an average
4,000 us/chunk (1,988..6,032 us individual gaps). The owner reports the revised live
speaker test "works perfectly." Its 297 PCM blocks / 44,029 nonzero samples all
reached playback, with zero erasures, receive gaps, FIFO underruns/overflows,
driver discards, late deadlines or rapid push pairs. Max IMBE 12,643 us; record
processing 104,544 us. The test was explicitly stopped and radio returned idle.
TX remains disabled. See results/playback-pacing.json.

## 2026-11-06 update: shared-PTT receive control corrected

The unplugged speaker attempt stopped after about 27.88 seconds with native
reason 15 (radio-input interruption), zero P25 records, and a return to FM.
This was not a duration expiry. The original request type was not logged;
timing points to the programming plug, whose serial connection shares PTT.

Internal RX audio now consumes PTT press/release rather than aborting. Channel
changes and stop controls still end it; raw native sessions retain their old
PTT behavior. A fixed read-only event receipt records consumed PTT events and
canceling requests so the next physical test can verify the cause. Internal
RX audio now supports five minutes through a seconds-based command; the raw
cable ARM stays at two seconds and the worker lease stays 500 ms. The host
waits for a new session to reach RX before announcing readiness.

The revised image passes 83 Python and 20 relevant Windows tests, actual
native/PCM/fixed-point-codec ASan/UBSan tests, target variants, static stacks,
zero warnings and identical forced rebuild. App use is 459,232 flash and
188,496 main RAM bytes. It is installed and app-readback verified on PC/Mac,
without pre-install firmware/data-array reads. Post-install replay matches
CRC 895f93ce and consumes all 72 PCM blocks, with max IMBE 13,539 us and record
processing 116,044 us. The five-minute unplug/listen test produced intelligible
but choppy speech; its complete counters are recorded above. TX remains disabled.
See results/speaker-control.json and docs/receive-audio.md.

## 2026-11-05 update: on-radio decoder and paced PCM playback

The installed restricted image decodes the saved 57-record Harris capture on
its own CPU. Its 72 PCM blocks (nine intentionally muted for conservative late
entry) match the exact host-reference CRC 895f93ce. Paced playback consumes all
72 blocks / 9,382 nonzero samples, with zero overflow and one empty-buffer
rebuffer event. Maximum measured IMBE decode is 14,958 us and whole-record
processing is 123,485 us, within 20 ms / 180 ms budgets for this capture.
The same synthetic self-test still matches CRC 4e158373; decode now peaks at
116,571 us versus the earlier 293,179 us. Encode is still over budget at
33,765 us per 20 ms block; TX remains disabled.

A priority-3 worker, fixed 18-block PCM FIFO, HAL playback callback and internal
receive-only native ownership now connect the pieces. Raw cable ARM retains
its original two-second bounds. Internal RX audio is at most 60 seconds with
a worker-renewed 500 ms lease and independent timer/overflow shutdown. A short
RAM replay buffer supports repeatable tests without further RF transmissions.

The O3 unity fixed-point C++ build permits cross-file DSP-helper inlining, with
no algorithm/fast-math/runtime change. Host ASan/UBSan tests cover actual worker,
real IMBE/CAI, paced PCM, simulated live raw records, terminators, ownership and
faults. All 82 Python and 19 relevant Windows tests pass; default/feature-off
builds, forced rebuild, zero target warnings and static stack checks pass.
The app uses 458,852 flash and 188,464/211,456 main RAM bytes; heap free with
audio is 8,144. Codec object size is 5,736 bytes; its reserved context is 8 KiB.

The application was verified on PC and Mac without pre-install firmware/data
array reads. Remote restart did not re-enter the power-on ROM; it attempted
zero flash writes and health passed. A physical cycle installed the image.
The first audible live-RF attempt returned zero P25 records over its full
minute; the owner heard digital noise but was unsure of stimulus timing.
A synchronized connected-cable live check now generates and delivers 99 PCM
blocks from 14 CAI units, with zero codec erasures/overflows and maximum IMBE
time 14,003 us. Audible unplugged-speaker checking awaits owner availability;
no audible live RF success is claimed.
See docs/receive-audio.md and results/receive-audio.json.

## 2026-11-05 update: hardware P25 voice capture demonstrated

The Harris transmission on 145.500 MHz, NAC 0x293 now yields eight consecutive
complete LDU1/LDU2 units: valid NIDs, clear metadata and every voice word with
zero FEC corrections. Independent pinned OP25 channel decoding matches all 72
IMBE parameter blocks; host IMBE decoding produces 1.44 seconds / 11,520 samples.
The owner listened and confirmed it is their voice and sounds good.
The 57 raw records are 29,996–30,004 microseconds apart, with no sequence gaps,
RX drops, PA requests or restoration failures. This proves the hardware RX
route for this bounded capture, not standalone audio or arbitrary call duration.
The initial HDU body still fails FEC and remains an explicit open issue.

We corrected our 24/24 sync-register packing, added fixed read-only snapshots,
and retained RX after DMR RF/slot-format warnings only in the TX-inhibited
first-boot preset. Lease, duration, overflow and restoration checks remain.
The offline on-device IMBE/CAI test matches the host CRC 4e158373 (18 PCM blocks),
with unchanged heap before/after; its elapsed times exceed real-time budgets.
The cable disconnects the speaker. On-radio audio routing is not implemented.

Revised installations verify on PC and Mac without pre-install firmware/data
array reads. Normal remote restart works; hands-free installer entry is unproven.
All 77 host Python tests, actual C ASan/UBSan first-boot/native tests and target
stack/rebuild checks pass; 14 relevant Windows tests pass. TX and data writes
remain inhibited. Next: initial-HDU fault, repeated/longer RX, codec speed and
live audio, then TX waveform tests. See docs/live-receive-tests.md and results/live-receive-tests.json.

## 2026-11-05 update: first-boot image installed and communicating

The owner authorized installation. The restricted `.048` own-codec image was
written through the pinned temporary RAM helper: 234 application sectors and
2,024 nonblank pages, with 4,516 exact planned mutation frames. Every changed
sector and then the entire 999,424-byte application readback matched; the Mac
independently verified the copied readback. The helper restarted the radio.
Two cable health checks pass with increasing uptime, restriction flags 31,
zero denial counters, idle FM and zero storage writes/claims. A stable 240x320
cable screenshot shows the home screen. Runtime heap free is 14,384 bytes;
the least task stack free is 452 bytes in these idle checks.

An extra original-program read had already completed when the owner requested
no existing-firmware reads. That initial run was stopped before writes. The
revised run reused saved backups and the still-running helper: no further
pre-install firmware/data-array reads, and no post-install boot/data readback.
The host guard excluded those ranges from all mutations. All 64 Python tests
pass; eight installer tests also pass on Windows. No RF transmit, on-device
P25 codec diagnostic or native RX experiment ran. Actual stock restoration is
still untested. See `docs/firstboot-installation.md` and its live results JSON.

## 2026-11-05 update: restricted first boot prepared

Built and packaged a separate first-boot preset with immutable TX and data-write
inhibits, final PA-output guards, storage-claim refusal and a debug allowlist.
CPS and RAM-loader entry are omitted. Linked/object-code audits and forced
rebuild pass; main RAM free is 31,688 bytes. Actual generated driver tests pass
ASan/UBSan, including 8,194 refused flash operations, bus faults, FD6818 TX
suppression, receive setup, all command numbers and counters. All 56 Python
tests pass; four health-client tests also pass on Windows. Existing native tests
and feature-off target regressions pass.

The exact `.048` own-codec test package and distinct stock restore candidate
are staged/hash-verified on the bench PC, alongside a query-only health client.
No serial port was opened and nothing was flashed this stage. Actual boot,
install/readback/restore and P25 RF/audio remain untested. See `docs/firstboot.md`
and `results/firstboot.json`.

## 2026-11-05 update: exact .048 compatibility preflight

The saved unit matches RadioFoundry's exact `.048` codec profile byte for byte.
Own-codec test and stock restore candidates pass slot CRC/hash/relocation and
updater range checks; repeated packaging is identical. Nothing was flashed.
The actual calibration and storage readers pass host ASan/UBSan on the saved
data: factory calibration loads, 129 foreign slots are recognized, and no
write/erase is attempted. Existing proposed storage areas contain stock data;
first bring-up must avoid storage claiming or conversion.

All 52 host Python tests pass, including four identification-probe tests that
also pass on Windows. The guarded live update-loader check identified
`BFUV32-V2`, sending only three identification requests and no erase/program
commands. The operator confirmed normal stock operation after a power cycle.
Actual experimental boot, installation/readback,
restoration and P25 RF/audio remain unproven. See `docs/unit048-compatibility.md`
and `results/unit048-compatibility.json`.

## 2026-11-05 update: original radio backed up

Installed Python 3.13.17, hash-pinned pyserial 3.5 and self-contained vendor
CPS 1.45 on the Windows bench PC. The actual radio responds through the C7000
power-on loader and runs the pinned temporary RAM monitor. Its program flash
contains `DM32.01.L01.048`; reported JEDEC IDs are C84014 (1 MiB program) and
EF4018 (16 MiB data). No experimental firmware or RF command was sent.

Both complete main flash ranges were read twice. Every streamed sector and
final device-sector CRC passed, both pairs are identical, and the Mac copies
match all 8,704 recorded 4 KiB sector hashes plus whole-file hashes. The raw
stock codeplug/calibration page region is separately preserved. Unit images,
full hashes and transfer receipts stay in ignored private backup folders on
both computers. Main-array dumps do not include separate chip OTP registers.

The new transport guard permits only the exact pinned ROM/RAM sequence,
DM32 configuration, chip IDs and bounded reads/CRCs. Eight guard/reader tests
pass on both macOS and Windows; all 48 host Python tests pass. The monitor's
additional write handlers remain in its binary but are blocked by the host
transport; its C7000 source is absent from the pinned upstream repository.
See `docs/recovery.md` and `results/readonly-backup.json`.

This establishes real cable access, temporary RAM execution and a consistent
backup, not a proven restore or P25 modem. The separate `.049`-pinned RAM probe,
custom application installation and all P25 RF/audio/timing tests remain open.

## 2026-11-05 update: native modem experiment interface

Added a hardware-first raw RX/TX backend, private cable command 0x21 and host
client. Sessions have bounded queues/duration, heartbeat lease, independent
1 ms timer guard, exclusive radio-task ownership, ordered TX refill, raw RX
metadata/counters, immediate PA shutdown and deferred channel restoration.
Faults latch and require acknowledgment. Tests caught and fixed stale-clock
and STOP-during-setup races. No radio, serial access, flashing or RF was used.

Actual controller/chip/interrupt-port/cable-layer tests pass ASan/UBSan,
including 100,000 adversarial events; nine client tests cover refill, captures,
disconnect and finally-STOP behavior. Default firmware is reproducible with
zero target compiler warnings, 432,008 bytes flash and 182,232 bytes RAM,
leaving 29,224 bytes in the main region. Static task and native IRQ stack
checks pass; on-device timing/stack/RF measurements remain unperformed.

See `docs/native-modem-tests.md` and `results/native-modem-interface.json`.
Native P25 RX continuity, TX buffer ownership/drain, waveform compliance and
live mic/speaker/PTT remain open. Conventional Phase 1 is still first; trunk
tracking follows it. No further KD8TUO investigation was performed.

## 2026-11-05 update: RadioFoundry

Implemented a separate GPL-compatible RadioFoundry derivative, conventional
clear group-voice engine, native 36-byte PHY transport adapter and offline
on-device software diagnostic. Real IMBE + CAI lifecycle/pacing/admission/loss
tests pass with ASan/UBSan. Both CONFIG_P25 states cross-compile without firmware
warnings. P25 worker static stack check passes, application bytes reproduce on
forced recompilation, and the linked ELF has no high-register instructions or
unresolved symbols. A separate local image merges the recognized stock DMR
codec using upstream tooling. Nothing was flashed or run on a radio.

Hardware modulation/demodulation is now the preferred route, following the
user's direction. RadioFoundry reports native raw/continuous TX for DMR; P25
waveform shaping and unrestricted native RX are unresolved hardware gates.
The image's P25 feature is an offline diagnostic, not a selectable P25 RF mode.
No live microphone/speaker/PTT binding or RF interoperability is claimed.
Trunk tracking remains the next stage after conventional operation.

`docs/conventional.md` records exact TIA decisions, build limits, resource
measurements, and the user-supplied KD8TUO implementation lead.
`results/radiofoundry-integration.json` records actual checks/artifact hashes.
The earlier `docs/radiofoundry-assessment.md` remains historical context;
its preference for an FM sample modem is superseded.

2026-10-26. **Hardware-unverified; no flashable P25 release candidate.**
User confirmed no radio is available and its exact installed version is unknown.

## Proven in source, disassembly or software tests

- Public target `DM32.01.01.049`, 869,688 bytes, SHA-256
  `03107b6f600e7fa95105af8324cff349c0f17fe30db249c14d11e79baa8da97a`.
  Original archive and mirror agree byte-for-byte; pristine inputs are preserved.
- Header/payload, startup relocation, ABIv2 and 51 exact-image anchors mapped:
  modem, audio/IRQs, tuning, UHF switching, calibrated power ramp and candidate
  physical PTT/admission dataflow.
- Real fixed-point full-rate IMBE encoder/decoder and P25 CAI implemented.
  Independent mbelib/OP25 checks, full-packet/error tests and ASan/UBSan pass.
  Host PCM -> IMBE -> CAI -> dibits -> independent decoder pipeline passes.
- Native CK803S builds pass. Codec objects/archives are identical across two
  output directories. Linked codec/CAI/runtime has 34,072 code/constant bytes,
  12 BSS bytes and no unresolved symbols; it has no radio driver.
- Two-pass backup reader validates ranges, lengths, framing and checksums,
  retaining interrupted files as partial. RAM helper flash-write dispatch is
  bypassed with exact hash/byte guards, assembler checks and command-path tests.
- A 435-byte independently hash-pinned RAM diagnostic builds, testing CRC,
  volatile endianness, UART and WORK_MODE. It has not executed on a radio.

## Inferred / unproven on hardware

- Stock uncoded continuous TX is promising. Actual ownership, uninterrupted
  timing, inserted content, dibit mapping, +/-600/1800 Hz deviation and shaping
  remain unknown.
- RX is a DMR deframer with field permutation. Complete P25 continuity is
  unproved; internal FM sample buffers require transfer-function/routing tests.
- Static clock setup suggests 168/168/84 MHz under explicit assumptions, not a
  measurement. Encode/decode deadlines and stack high-water marks are unmeasured.

## Failed / unavailable experiments

- Candidate Ghidra module fails at startup MTCR. GNU raw disassembly without
  ABI flags also misdecodes; corrected analysis ELF wrappers resolve that issue.
- `jasiek/baofeng-dm32uv-reveng` returned 404. Public buildable DM-32-specific
  OpenDM32 application source was not located.
- The bench host and a local programming port were unavailable for that test.
- Initial toolchain dependency/build failures were resolved with verified GNU
  prerequisites and system zlib; native cross-builds now pass.

## Incomplete

Native modem/audio drivers and call/PTT supervisor; proven PA/protection events;
persistent frequency/NAC/ID settings; physical target/family identity; complete
calibration/identity region map; verified flash erase/load/readback; integrated
application/update container; all live RX/TX, RF, standalone, repeated-operation,
battery/thermal and restoration tests.

## Next decisive experiment when hardware becomes available

Identify unit/version and both flash capacities. Prove startup-loader read
access, obtain two complete original dumps plus codeplug, resolve changing or
aliased regions, then run the narrow RAM diagnostic. Next acquire complete
independent P25 frames through native RX and inspect uncoded TX separately.
See `docs/hardware-tests.md`. No purchase or external operational processor is
substituted for these gates.
