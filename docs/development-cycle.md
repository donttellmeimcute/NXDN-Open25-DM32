# Faster DM32 test cycles

The development bridge is RX-only. It changes update entry, not RF capabilities.
Install it once through the proven power-on loader. Subsequent updates can enter
the same approved RAM helper through the running application's cable interface.
Remote entry and application installation have now passed on the physical unit.
The RAM-only rehearsal entered the helper and restarted with zero flash-array
reads/writes. The subsequent 106-sector native-TX bench update entered remotely,
took 41.92 seconds to write and 41.63 seconds for full new-app verification,
then passed boot/cable health. This does not prove RF transmission.

## Approved RAM entry

Build with `--first-boot --lean --speed-codec --cpu-profile --dev-update`.
The first-boot health flags are 15 instead of 31: TX, data writes, cable filtering
and final PA GPIO guards remain, while the approved runtime helper is available.
Only its exact length, load/entry address, ordered chunks and CRC are accepted;
the host also requires the pinned helper SHA-256. A five-second inactivity lease
releases ownership after an abandoned upload. Power-off cancels the entry. The
pending jump rechecks the lease after allowing the cable reply to leave.

Use `tools/warm_probe.py --port COM5 --out <unique-receipt.json>` for the first
rehearsal. It performs no flash-array reads or writes. Its monitor transport
allows hello/configuration, chip identification/status and the seven exact
restart-register writes, then checks application health after reboot.

## Scoped application installation

`tools/start_firstboot_install.ps1 -WarmUpdate` selects runtime entry. The starter
pins the host tools, checks the explicit CH340 COM5 identity and holds its SSH
parent open until completion. The package and previously verified application
file must be updated and reviewed for each revision; do not reuse a stale plan.

`--sector-write` uses the pinned monitor's protocol-7 operation 0x0e. Each frame
contains one exact 4-KiB program-flash sector or an explicitly planned erase-only
sector. The guard excludes the first 48 KiB and the entire data flash. Returned
CRC must match; an error stops without retrying a mutation or restarting the
radio. The full newly written application must then match before reset.

The owner's no-existing-firmware-read mode remains required. Saved original and
predecessor files supply planning data. Only newly written sectors and, after the
whole plan, the installed target application may be verified. Chip identity and
status queries do not read firmware arrays. Backups and receipts remain private.

## Incremental builds and checkpoints

`--reuse-codec` hashes source inputs, build options and compiler identity, then
validates cached archive/object/call-graph bytes before reuse. Make still tracks
firmware dependencies. Missing or changed cache outputs rebuild the codec.
Use `--verify-rebuild --stack-check` at checkpoints and before release. Packaging
without a forced-rebuild proof requires the explicit `--development` option;
the package receipt states that limitation. Never present an incremental build
as an independently reproduced checkpoint.

These changes do not remove flash endurance limits or guarantee recovery after
power failure. The OEM recovery entry was identified; complete stock restoration
has not been exercised on hardware. No TX permission is introduced here.

## Separate native TX bench profile

`--tx-bench` additionally requires first-boot, native P25 and development update
build flags. Its health flags are explicitly 47. Ordinary PTT and storage writes
stay guarded. Only the current native ownership/session can obtain a temporary
grant, and only for 145.500 MHz, at most 60 blocks/two seconds and a 100..250 ms
heartbeat lease. The board fixes power index zero and requires valid factory
calibration. RF switching requires a sent block and key request; the PA also
requires the controller's keyed state. Fast-off revokes the grant first. Late
callbacks, expired leases and ownership/session changes cannot open it again.

`tools/native_tx_bench.py --port COM5 --allow-rf --out <unique.json>` makes one
attempt using four constant blocks by default, a one-second hard limit and a
250-ms lease. It requires health profile 47, an idle FM channel on the test
frequency and no previous unacknowledged native fault. It never retries or
resets a fault automatically. The transport permits only health and the scoped
native protocol, excluding firmware reads, updater entry and register access.
Independent I/Q recording must be running before requesting a measured burst.
TX counters alone are not proof of a valid RF waveform or P25 interoperability.

The first actual modem attempt stopped on an early-boundary fault: one block
loaded, a TX-slot-to-PHY notification gap of 26,355 us, zero RF/key interrupts,
zero PA keys and successful cleanup/idle health. A correction now keeps one
timing source per run, counting other sources without advancing the buffer or
refreshing the stall clock. Same-source 27..33 ms limits, the 60-ms stall guard,
heartbeat and hard duration remain. Host regression tests include that exact
event sequence and prove another source cannot hide a stalled selected clock.

With that correction and the calibration-admission check installed, four- and
seven-block runs completed and returned idle without faults. Independent VCE
I/Q confirms RF and four ordered modulation levels. The measured offsets still
need calibration; no P25 voice-frame decode is claimed. A 24-block stream
underran after its eight preloaded blocks. Instrumented requests show a missing
heartbeat reply during RF, so cable queue feeding remains unresolved. The
standalone short pattern establishes hardware TX, not sustained voice TX.
