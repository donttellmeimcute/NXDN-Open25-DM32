<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Native modem laboratory interface — November 5, 2026

This is a hardware-first experiment interface for the DM32's HR_C7000 PHY.
The CPU supplies/captures raw 36-byte blocks; the modem handles symbols.
There is no MCU C4FM modulator or operational P25 voice mode in this interface.
The original preparation results below are host tests and static analysis.
Subsequent live tests recover a valid P25 sync/NID; full voice continuity remains
unproven. See [live receive tests](live-receive-tests.md) for current hardware
results, fixed snapshots and restricted RX warning behavior. The later
[on-radio audio worker](receive-audio.md) uses a separate internal RX-only
60-second API with a 500 ms device-worker lease; ordinary cable ARM keeps
its original bounds.

The implementation builds on RadioFoundry commit
`dfbaf20c400c73e2ccf6edb80af7d1d255bee418` and its tested raw DMR TX sequencing.
Native RX register choices follow the saved HR_C7000 guide, sections 9.4–9.6,
printed pages 294–297, Tables 46–49 and Figures 70–71. These sources establish
the experiment, not a verified native P25 modem. P25 sync/waveform references
remain the exact TIA editions and clauses recorded in [conventional.md](conventional.md).
No third-party P25 firmware or new online implementation lead is required.

## Components and ownership

| Component | Responsibility |
|---|---|
| `src/modem/native_test.c` | Portable state machine, lease, hard duration, bounded queues, counters and fault latch |
| `integration/radiofoundry/native/hrc_native_chip.c` | Actual register/buffer accesses, raw RX records, duplicate TX halves, interrupt acknowledgments |
| `integration/radiofoundry/native/hrc_native_port.c` | Dedicated modem handlers, PA timer 3, independent 1 ms guard on timer 6, handler/mask restoration |
| `integration/radiofoundry/native/dm32_native.inc` | Existing board tuning/calibration/RF route, lowest calibrated TX power, fast PA off and FM restoration |
| `integration/radiofoundry/native/native_lab.c` | Private cable command 0x21, admission and exclusive radio-task ownership |
| `tools/native_modem.py` | Explicit-port host client, heartbeat, TX refill, raw capture and result files |

ARM only allocates a software session. START is a mailbox request executed by
the radio task. It requires an idle FM channel, matching current RX or TX
frequency, no PTT/scan/hotspot/beep/AM/broadcast activity, and a powered-on unit.
TX also checks RX-only, TX inhibit and fresh battery voltage against the board's
critical threshold. The board's normal band checks and calibrated power level
0 apply. The interface does not modify a channel, save settings or write flash.
An in-progress offline IMBE diagnostic and a native session exclude each other.

The normal radio tick/poll yields hardware ownership during a test. Ordinary
radio requests cancel and restore the test before being forwarded; a PTT-down
request that interrupts a test is consumed. STOP immediately disables the PA
and native send/interrupt paths; task cleanup restores the board and forces a
full retune of the unchanged channel. A STOP during slow board setup is latched,
then shutdown is repeated after setup rather than activating the canceled test.
Terminal faults require explicit acknowledgment before another ARM; unread RX
records must be drained before RESET or ARM can discard the session.

## Bounds and fault behavior

- Sessions: RX or TX, 1–60 blocks, duration at most 2 seconds, heartbeat lease
  100–500 ms (client default 250 ms). TX duration must cover `30*blocks+150` ms.
- TX ring: eight blocks, ordered session/index validation, at least two blocks
  prefilled (one for a one-block test). No fill/repeat when the queue runs dry.
- RX ring: sixteen complete raw records. Overflow stops the test, increments
  dropped count and retains the sixteen unread records. Unknown RX layout
  metadata is preserved for investigation rather than passed to the voice codec.
- Timer 6 checks lease, duration, start/stream stall and drain every 1 ms without
  depending on the host or radio-task service. Borrowing a running timer 3 or 6
  is refused. This assumes a running CPU with interrupts serviced; it is not
  protection against a hung CPU or physical power fault.
- TX boundary policy: nominal 30 ms, arrivals within 15 ms treated as duplicate
  notifications, accepted subsequent gaps 27–33 ms. First boundary timeout
  100 ms; active-stream stall timeout 60 ms. A duplicate never dequeues twice.
- After the last block handoff, the next boundary cancels SEND; the PA shuts
  down after a bounded 45 ms drain. A delayed RF/PA callback cannot rekey a
  stopped/expired session. The existing board's 28 ms PA delay is retained.
- Abnormal-slot interrupt, underrun, early/late boundary, expired lease,
  deadline, failed setup/write/restore, RX overflow or unexpected RF in RX
  latches a reason and shuts down. The first fault reason is retained.

The restricted first-boot preset now counts/ignores RF timing and abnormal-slot
notifications during RX only, to examine P25 continuity without DMR slot rejection.
Its TX paths remain inhibited. Other presets retain the fault behavior above;
lease/duration/overflow and all other fault checks remain enforced.

These timer values, duplicate rules, drain policy and queue limits are local
experiment policies, not TIA mandates or hardware measurements. In particular,
`tx_sent` counts blocks handed to the modem, not bytes measured over the air.
Counters cannot establish RF continuity, successful PA output or compliant P25.

The driver uses continuous PHY WORK_MODE 0x02. RX uses L2 control 0x40 and
TXRX control 0x41; TX uses L2 control 0x82 and one-send TXRX 0x80. Every TX block
is written identically into both RAM halves, matching upstream's tested default.
Alternate-half ownership remains unmeasured. Optional P25 sync replaces the MS
and BS matchers and disables other matchers while preserving their saved words.
`--sync inherited` leaves the board-prepared DMR sync configuration in place.
The experiment does not directly change RF_MODE, modulation bias/deviation or
RX FIR settings; existing board preparation still applies its normal calibration.

## Cable protocol v1

This private derivative uses debug command **0x21**, separate from offline
diagnostic **0x20**. Standard debug-link framing/status codes remain upstream's.
Integers below are little-endian; request lengths include the operation byte.
Replies start with the standard status byte. No arbitrary register-write command
is exposed. Session IDs prevent stale same-boot requests; they are not security
credentials and can recur after reboot.

| Op | Request | Successful reply after status |
|---|---|---|
| 0 STATUS | op (1 byte) | 37 uint32 words |
| 1 ARM | op, mode u8, P25-sync u8, blocks u16, duration-ms u16, lease-ms u16, frequency-Hz u32, TX-opt-in u32 (17 bytes) | session u32 |
| 2 LOAD | op, session u32, index u16, raw[36] (43 bytes) | empty |
| 3 START | op, session u32 (5 bytes) | empty; only queues start |
| 4 PING | op, session u32 (5 bytes) | empty |
| 5 STOP | op, session u32 (5 bytes) | empty; repeat is idempotent |
| 6 READ | op, session u32 (5 bytes) | sequence, microseconds, SYS flags, RX_TYPE_INFO (four u32), raw[36] |
| 7 RESET | op, session u32 (5 bytes) | empty; refuses ownership/unread data |
| 8 SNAPSHOT | op (1 byte) | 56 uint32 words; fixed current and last-event modem registers |

Mode is 1 RX or 2 TX. TX requires the explicit opt-in word `0x3532504e`
(bytes `NP25`); this is a deliberate-action marker, not authentication.
Malformed lengths and short output buffers are refused without popping data.

STATUS word order is defined identically by `nt_status()` and client `FIELDS`:
version, state, reason, session, owned, mode, frequency, blocks, duration, lease,
RX/TX queue counts; RX seen/saved/dropped/read; TX loaded/sent; PHY/TX-slot/RX-slot/
RF/abnormal IRQ counts; duplicates, ignored IRQs, PA keys, quiets, faults,
rejected requests; maximum/last boundary gap, restore failures, start/boundary
timestamps, P25-sync selection, key-requested and keyed flags. Counters restart
on a successful ARM. State/reason names are included in client result reports.
Restore failure clears ownership after another quiet attempt and latches a
fault; a software cleanup acknowledgment does not prove a physical RF-off state.

The development TX build also exposes the most recent physical PTT transmit
trace on op 11. A one-byte request returns 22 little-endian status words. A
seven-byte request (op, session u32, first block u16) returns as many whole
44-byte records as fit: modem interrupt time in microseconds, queue depth before
the handoff, and the 36 bytes handed to the modem. The reader refuses stale or
incomplete sessions and indexes past the saved blocks. The completed record
survives return to P25 receive and is replaced by the next TX start or lost on
reboot. It records the first 192 handoffs (5.76 s), with counts and key events
for the entire call. This uses about 8.5 KiB of RAM in the development build.

With the cable reconnected after PTT release, run
`python tools/p25_channel.py --port COM5 trace`. The header distinguishes the
first modem RF notification, the request to enable the PA, and the board timer
callback that enables it. The block timestamps and payload establish what
firmware handed to the modem. An RF capture is still needed to verify what was
transmitted. The header's `pic_pending_1` field is a historical interrupt
request latch at handler setup; it does not establish a live pending request
for the call.

## Reproduce without hardware

Run from the repository root:

```sh
make native-test
python3 tools/build_radiofoundry.py --stack-check --verify-rebuild --with-local-stock
python3 tools/build_radiofoundry.py --disable-native --stack-check
python3 tools/build_radiofoundry.py --disable --stack-check
```

`--with-local-stock` is optional, uses the existing recognized local AMBE donor
and creates a separate ignored `.codec.bin`. No donor or generated binary is
committed. Default build preset enables CONFIG_P25 and CONFIG_P25_NATIVE; both
feature definitions default off outside the preset. Outputs are separate
`dm32-p25-native`, `dm32-p25-lab` and `dm32-p25-off` build directories.

ASan/UBSan tests execute the real controller, chip driver, cable-command layer
and interrupt-port translation unit using host register/board/timer doubles.
They exercise normal bounded RX/TX, ordered refill, wraparound, overflow,
underrun, duplicate/missing/late IRQs, lease/deadline shutdown, late PA callbacks,
timer conflicts, start/restore failures, startup cancellation and exact port
handler/mask restoration. There are also 100,000 deterministic adversarial
events and nine host-client tests, including disconnect, interrupt/finally STOP,
and refusal of TX without opt-in before opening a port.

Target checks cover zero compiler warnings, no unresolved symbols/high-register
instructions, identical application bytes after forced compilation, task stacks
and all six native IRQ entry paths against the shared 2 KiB non-nesting IRQ
stack. Included board callbacks are supplied as explicit call-graph edges.
Static stacks use library estimates and margins; no device stack/timing/heap
measurement has been made. See [results](../results/native-modem-interface.json).

## First attached-radio work

The interface requires this derivative already running; it cannot control OEM
firmware. It has no flashing or power-cycle operation. Unit/version identity,
recovery access and verified backups remain the installation prerequisites in
[recovery.md](recovery.md). A webcam could observe the display, but cannot
replace a physical power cycle when firmware/cable access is lost.

After those prerequisites, the intended bench sequence is native RX first,
then bounded TX into a suitable bench RF setup. Example commands below were
not executed; replace the port and frequency with the actual bench setup:

```sh
python3 tools/native_modem.py --port /dev/EXPLICIT_DEVICE status
python3 tools/native_modem.py --port /dev/EXPLICIT_DEVICE run --mode rx --frequency 145500000 --output artifacts/native/rx
python3 tools/native_modem.py --port /dev/EXPLICIT_DEVICE run --mode tx --frequency 145500000 --pattern counter --allow-rf --output artifacts/native/tx
```

The client writes `.json` status/result and `.jsonl` untouched RX records;
TX also saves its expected `.air` bytes and SHA-256. Raw capture format feeds
`experiments/modem/stream_probe.py`. `--air FILE` accepts up to 2160 packed CAI
bytes, pads to a whole 36-byte block and records that non-CAI tail length.
Constant dibit and `p25-cal` byte patterns are transport/waveform probes whose
actual symbol mapping must be measured. No software acknowledgment proves it.

The client polls/refills/pings, attempts STOP in `finally`, and waits for
ownership release. Transport loss produces an unconfirmed-cleanup result;
it never claims the radio stopped merely because STOP was attempted. Device
lease shutdown is the independent control, subject to the CPU/IRQ limitation.
Faults remain latched and there is no automatic restart or infinite test loop.

The decisive tests remain complete independent HDU/LDU1/LDU2/TDU recovery
across all native block phases, byte-exact continuous TX, and independent
deviation/shaping/clock measurements. Then connect the existing CAI/IMBE engine
to mic/speaker/PTT and measure real-time CPU/stack budgets. Trunk tracking stays
after conventional P25 Phase 1 RX and TX are demonstrated.
