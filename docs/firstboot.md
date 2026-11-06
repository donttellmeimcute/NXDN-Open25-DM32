<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Restricted first-boot laboratory image

November 5, 2026. A separate `dm32-p25-firstboot` image is built, tested offline,
merged with this unit's exact `.048` codec, and installed on the radio.
**Application readback, boot and cable health checks pass.** A stable screenshot
shows the home screen. See the [installation record](firstboot-installation.md)
and [live results](../results/firstboot-installation.json).

This image now supports boot/UI/cable checks and bounded native RX experiments.
See [live receive results](live-receive-tests.md) and
[on-radio audio development](receive-audio.md) for subsequent revisions. It is not
an operational P25 mode. The original full-chip backups and separate stock
restore candidate remain preserved. Application installation/readback is now
demonstrated; actual stock restoration remains untested.

## Fixed restrictions

`CONFIG_FIRSTBOOT=y` compiles these restrictions into the image. There is no
command, setting, token or UI action that enables them at runtime. Removing
them requires a different build.

| Path | Restriction |
|---|---|
| Ordinary FM/DMR TX, including physical PTT and injected keys | Board TX entry records refusal, forces RF off and returns false before tuning or TX setup |
| Native modem TX | Controller rejects TX ARM even with the ordinary explicit RF opt-in marker; native RX remains available |
| PA callbacks and DMR RF switch | Key attempts are refused; RF-off hook is called |
| GPIO output | Final GPIO write path forces both board PA selects low, including output-direction setup |
| PA drive | DAC B initializes powered down; requests to power it on are forced off |
| RF transceiver | FD6818 register writer refuses the documented TX state, including TX tables and burst setup |
| Data flash | HAL write/erase entry points refuse all addresses; independent SPI NOR write-enable guard prevents direct driver bypass |
| Stock settings area | Both direct and deferred storage-claim requests refuse before marking a claim or attempting erase |
| Cable protocols | CPS and fast CPS are omitted; RAM-loader entry is omitted; hotspot, PPP, APRS, gateway and zone sharing remain disabled by the lab preset |
| Debug commands | Explicit allowlist permits diagnostics, screenshots, key/navigation and RAM radio controls, P25 diagnostics and bounded RX audio, native RX interface and restriction status; PTT command, storage commands, arbitrary audio/register probes, crash/watchdog tests and unknown commands refuse; normal restart alone is allowed with exact mode/length and the existing token |

The PA pins come from the board's existing pin definitions. The generic policy
has no board pin numbers. Counters use the platform interrupt-save hook, work
before the scheduler, saturate at `UINT32_MAX`, and have no reset API. They
count refused software requests, not measured RF emissions or flash contents.
Existing fault/boot RF-off paths still use their original RAM-resident low
output writes. These code guards are not protection against arbitrary memory
corruption, electrical faults, or code loaded through the separate OEM/ROM
loader outside this application.

The radio can receive using its ordinary startup defaults. The stock codeplug
has not been converted; new setting changes are temporary and may report save
failure. The `.048` compatibility audit found occupied stock bytes throughout
the proposed writable regions, so no storage migration or claiming is part of
this bring-up.

## Cable status and health collector

Private debug command `0x22`, payload `00`, returns normal status followed by
13 little-endian u32 words: version `1`, restriction flags `31`, then denial
counters in this order:

`board_tx, pa_key, rf_switch, pa_gpio, flash_write, flash_erase,
nor_write_enable, fd_tx, storage_claim, debug_command, native_tx`.

Flag bits 0..4 indicate TX inhibit, data-write inhibit, debug allowlist,
PA GPIO guard and omitted RAM-loader entry. Bad operation/length or a short
output buffer returns BAD_ARG without changing counters. There is no mutation
operation on `0x22`.

`tools/firstboot_check.py` is prepared for **after installation**. It first
requires the complete restriction receipt, then checks cable echo, the exact
first-boot version label and selected radio/task/storage views. It refuses a
TX/unknown state, missing or nonzero storage-write/claim fields, or counters
changing during collection. Its transport permits only health query commands;
RTS/DTR are inactive before opening the explicit serial port. It pins the
upstream debug module hash and requires no Git installation on the PC.

It does not run on stock firmware, flash/reboot, inject keys, start native RX
or TX, or run the offline codec diagnostic. A successful health receipt is not
a flash readback or firmware attestation. Two live health receipts now pass.

## Initial preparation verification (historical)

- Target build: zero compiler warnings, no unresolved symbols or high-register
  instructions. Forced recompilation produces identical application bytes.
- Flash: 421,928 bytes before codec-slot padding; main RAM: 179,768 of 211,456
  bytes, leaving 31,688 bytes. Static task and all six native IRQ stack checks
  pass. These are build estimates, not on-device high-water measurements.
- Linked image excludes SPI NOR program/erase/write-enable implementations,
  CPS writers, and RAM-loader entry functions. Object-code inspection verifies
  board TX calls only refusal/RF-off and returns false; GPIO references the PA
  guard. The builder enforces these audits for the first-boot preset.
- Host ASan/UBSan tests compile the actual generated GPIO, DAC, SPI HAL, NOR,
  FD6818, storage and native-controller sources. They exercise all 96 GPIO
  outputs, stale high PA bits, DAC off, 8,194 HAL write/erase refusals across
  the complete data-chip range, direct NOR bypass attempts, storage claiming,
  300 TX-state attempts through direct/state/burst paths, retained receive setup,
  and all 256 debug command numbers. Read/probe bus failures propagate and
  release chip select. Counter values, short replies and malformed status
  operations are checked.
- All 56 Python host tests pass. Four health-client tests also pass on Windows,
  including missing inhibits, wrong image identity, changed counters and
  malformed receipts. Existing native modem controller/chip/IRQ/protocol tests,
  100,000 adversarial events and nine client tests still pass with restrictions
  disabled. Feature-off target builds and static stacks also pass.
- The builder refuses and preserves an unreviewed extra derivative source.
  The packager refuses an unrestricted image when `--require-first-boot` is
  requested. Build recipe, source, generated-source and application hashes are
  checked before packaging. Repeated packaging is byte-identical.
- The unit's exact codec slot validates tag 48, CRC, known code/table hashes
  and relocations. Both updater containers pass offline bounds/unwrap checks.
  The three staged image hashes/lengths match independently on Windows. Staging
  and Windows tests did not open a serial port.

## Reproduce and package

From the internal checkout:

```sh
make firstboot-test
python3 tools/build_radiofoundry.py --first-boot --stack-check --verify-rebuild
python3 tools/prepare_unit_image.py \
  --backup backups/DM32-stock-048-20261105T233920Z \
  --image build/radiofoundry-p25/build/dm32-p25-firstboot/rfoundry_dm32.bin \
  --require-first-boot --out artifacts/NEW_UNIQUE_FIRSTBOOT_PACKAGE
```

The private package was staged on the bench and build hosts. It contains a raw restricted candidate, its BFUV32-wrapped
candidate, a distinct stock restore candidate, and manifests. Do not confuse
these with the earlier unrestricted native-test candidates. No payload contains
the first 48 KiB bootloader or the separate data chip. Do not flash a raw image
at chip offset zero. These are private unit artifacts, excluded from Git.

The Windows bench health command, now tested against the installed image, is:

```powershell
.\.venv\Scripts\python.exe tools\firstboot_check.py --port COM5 --out reports\NEW_UNIQUE_HEALTH.json
```

Installation, application readback and boot/UI/cable checks pass. Remaining:
establish complete native RX frames and voice continuity. The offline IMBE/CAI
self-test now passes and a valid RF P25 NID is recovered; see the live receive record. RF continuity, C4FM deviation/shaping, P25 audio interoperability
and eventual TX all remain unproven. Trunk tracking follows conventional RX/TX.
