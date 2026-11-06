# Exact-unit .048 compatibility preflight

2026-11-05. Offline checks pass for the saved `DM32.01.L01.048` unit.
This offline preflight preceded the now-completed
[restricted first-boot installation](firstboot-installation.md), whose
application readback and boot/cable checks pass. No RF test was performed.
The live recovery loader identified correctly as
`BFUV32-V2` after the operator confirmed green-LED update mode. The operator
then confirmed a normal restart into the stock screen. This establishes protocol
access, not an actual erase, installation or restoration.

## Codec, memory and update container

Pinned RadioFoundry commit `dfbaf20c400c73e2ccf6edb80af7d1d255bee418`
already recognizes `DM32.01.L01.048.bin`. Extraction from the unit's saved
program flash matches both complete known codec windows:

| Component | Length | SHA-256 |
|---|---:|---|
| Stock codec code | 83,456 | `cf275cd01ef8ba09c2873009ac5923dd5d2bc2384f6ceaa8b658b9d46d4478e2` |
| Stock codec tables | 12,152 | `10684c33e3cc8238376e455fd96b1c15a863a864420dbbaa4589803c4eb0b0e1` |

The laboratory application is merged with **this unit's .048 codec**, rather
than the earlier .049 donor. Upstream's slot parser validates tag 48, CRC,
both hashes and three relocations. Code fits the reserved codec RAM exactly;
tables fit reserved SRAM. The existing native-interface build manifest and
all its source/output hashes validate. No target source changed in this check.
This stock DMR codec is separate from the project's software P25 IMBE codec;
recognizing it does not establish a hardware P25 modem.

The board/update preset starts the application at `0x0300C000` and ends at
`0x03100000`, matching the identified 1 MiB program chip. The laboratory codec
slot occupies `0x030E8000..0x03100000`. Generated raw and BFUV32-wrapped
candidates pass upstream length/range checks. A separate stock restore
candidate wraps the original saved program bytes from offset `0xC000` onward;
unwrapping reproduces that complete range exactly. Neither candidate contains
the first 48 KiB bootloader or the separate 16 MiB data chip.

These are **candidate packages**, not demonstrated recovery tools. Actual
device erase bounds, padding, acceptance, post-write readback and stock restore
remain untested. Keep both original chip dumps regardless of these packages.
Private candidates, hashes and manifests remain in ignored internal artifacts;
they are not committed or distributed.

## Factory calibration and occupied storage

`tests/compat/radiofoundry_state.c` links the actual pinned upstream calibration
and storage implementations against the saved 16 MiB data image. Its flash
write/erase callbacks count and refuse every attempt. ASan/UBSan and strict
compiler warnings pass. The test requires all of these results:

- Factory calibration loads from the stock page, with no copy-area fallback.
- UHF power points are present and the requested lowest drive fits the DAC.
- Storage initialization reports a foreign region and refuses to initialize it.
- Neither calibration nor storage initialization attempts any write or erase.

The stock image has one calibration page with logical tag 2. An independent
BCD check finds all four five-point frequency tables valid, ascending and
within their VHF/UHF ranges. These are format checks, not measured RF power or
modulation calibration results.

Storage reports 129 foreign slots. The EEPROM, settings/journal, open-firmware
and native-storage allocations all contain existing stock bytes. They must not
be treated as empty simply because their addresses are assigned in the new
firmware. The observed initialization guard preserves them; this test does not
prove that every later UI action or storage-claim path is read-only.

First bring-up should use RAM defaults, inhibit physical and cable TX, and
disable persistent writes/claim operations. Stock codeplug conversion and any
intentional new storage layout need a separate reviewed migration plan. The
raw codeplug-page backup is not a CPS project file.

## Reproduce the offline checks

Run from the internal checkout, using the private verified backup directory:

```sh
python3 tools/prepare_unit_image.py --backup backups/DM32-stock-048-20261105T233920Z \
  --out artifacts/NEW_UNIQUE_PREFLIGHT
python3 -m unittest discover -s tests -p 'test_*.py' -v
mkdir -p build/compat
clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -Iresearch/upstream/radiofoundry/hal -Iresearch/upstream/radiofoundry/core \
  -Iresearch/upstream/radiofoundry/drivers/cal/baofeng \
  tests/compat/radiofoundry_state.c \
  research/upstream/radiofoundry/drivers/cal/baofeng/baofeng_cal.c \
  research/upstream/radiofoundry/core/storage/storage.c \
  -o build/compat/radiofoundry-state
build/compat/radiofoundry-state backups/DM32-stock-048-20261105T233920Z/q128-pass1.bin
```

The packager validates all 8,704 saved sector hashes, whole files and paired
reads before extraction. It refuses modified upstream/build inputs and existing
output directories. Repeated packaging produced identical candidate bytes.
All 52 host Python tests pass. The four new updater-probe tests also pass on
Windows against the actual upstream simulated bootloader.

## Completed live identification and next bring-up gates

The live probe passed on the bench PC through COM5 at 2026-11-06 00:30 UTC
(November 5 local time). The saved report records exactly `52`, `4d00000001`,
`06`, the expected `4d0109` protocol reply and `BFUV32-V2` model ID. It stopped
before step 4, with zero erase/program commands. The private report was copied
to the Mac and its content validated. No temporary RAM monitor was needed for
this separate OEM update-loader check.

`tools/dm32_update_probe.py` permits only updater identification requests:
`52`, `4d00000001`, `06` (the first request may retry up to three times).
It checks the exact protocol response and `BFUV32-V2` identifier and stops
before step 4. Its transport rejects erase, block data and every further request.
`tools/run_update_probe.ps1` verifies the reviewed script hash and CH340 COM
port before execution on the bench PC. Enter PTT+SK1 green-LED update mode
before running it; return to normal stock operation with an ordinary power cycle.
Do not open this probe against an unconfirmed normal operating mode.

The separate [restricted first-boot image](firstboot.md) is now prepared and
installed, with TX and data writes inhibited. Application readback and
boot/UI/cable checks pass. Next run the software codec diagnostic, then test
native RX continuity. Hardware TX
buffer ownership, C4FM shaping/deviation, actual P25 audio interoperability and
standalone operation remain separate gates. Conventional Phase 1 comes before
trunk tracking.
