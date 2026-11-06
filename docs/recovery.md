# Backup, updater audit and restoration

November 5 update: the actual `DM32.01.L01.048` radio was read through its
startup loader using a temporary RAM monitor. Both complete main flash ranges
were read twice, matched, and copied/hash-verified independently on the Mac.
The backup phase changed no firmware. Exact `.048` offline codec/calibration/package checks
now pass; see [unit compatibility](unit048-compatibility.md). The later restricted
application installation/readback and boot/cable checks pass; actual restoration
remains unproven. There is no validated P25 release candidate.
The separate identification-only probe reached the OEM `BFUV32-V2` update
loader, stopped before erase/transfer, and the operator confirmed the normal
stock screen after restarting. That check does not prove a restore write.
A [restricted first-boot image](firstboot.md) is now installed with TX/data writes
inhibited and CPS/RAM-loader entry omitted. See the
[installation record](firstboot-installation.md), including the owner's request
to omit additional original-firmware reads and the exact verification limits.
The October 26 reader/probe findings below retain their original limitations.

## November 5 addition: identified-capacity read-only reader

`tools/dm32_readonly_backup.py` uses RadioFoundry's pinned universal RAM monitor
from commit `dfbaf20c400c73e2ccf6edb80af7d1d255bee418`. It checks the host-module
and RAM-image SHA-256 before execution. No Git installation is needed on the
capture PC; Python 3 and the hash-pinned `requirements-hardware.txt` suffice.
The original reader documented below remains available separately.

The new reader asks for each chip's JEDEC ID before choosing a range. It accepts
1 or 2 MiB program flash and 16 MiB data flash, refusing unknown densities.
It replaces the upstream host profile's default 1 MiB bound with the detected
program size so a supported 2 MiB device is read completely. Capacity follows
the device's ID response, not an update-file length or a presumed board revision.

A transport guard validates **every outgoing packet**. During ROM loading it
allows only the exact pinned image at 0x10000 and execution after all its blocks
were sent. During the monitor session it permits HELLO, a single timer-register
read, the DM32 configuration, JEDEC reads, bounded SPI reads, sector reads and
sector CRCs. Erase, page/sector program, write enable, arbitrary POKE/CALL,
reset, LED commands, baud changes and unrelated controllers/addresses are
refused before serial transmission. Reads stay at 115200 baud, RTS/DTR inactive.

This guard is a host-side restriction. The upstream monitor binary also contains
write operations; they have not been removed from that image. Its C7000 source
is not present in the pinned repository, so this is not a claim of a fully
source-audited or formally verified device-side read-only program. The older
helper's patched dispatcher and different limits are described below. Neither
reader's successful backup alone establishes a proven restoration procedure.

Example, after confirming the explicit cable/port and installed version:
Use the bench virtual environment's interpreter (on Windows,
`.venv\Scripts\python.exe`), rather than the Windows Store Python shortcut.

```sh
python tools/dm32_readonly_backup.py --port ACTUAL_PORT \
  --installed-firmware EXACT_DISPLAYED_VERSION --out backups/NEW_UNIQUE_SESSION
```

Use normal power-on with the cable seated, **not** PTT+SK1 update mode. The
reader must be waiting when the radio powers on; it reports READY after opening
the port. The display can remain dark while this temporary RAM program runs.
Do not unplug or power off during transfer. Power cycle normally after capture
to return to the unchanged stock application. A stock radio's physical power
knob still requires a person; software installation does not automate it.

Both complete main flash address spaces are read twice. Each streamed sector
has a device CRC, and the upstream reader checks sector CRCs again after each
range. The wrapper records SHA-256 for every 4 KiB sector and each whole file,
compares the two passes, and refuses verification for changed data, all-blank
captures or suspicious repeated halves. Incomplete files retain `.partial`;
complete files use exclusive promotion and never overwrite an earlier capture.
These checks improve detection of transfer errors/aliasing but are not proof
of every possible addressing or device fault. Separate chip OTP/security
registers are not included in these main-address-space dumps.

`backup-manifest.json` is created only after the two matching passes and records
the source/RAM hashes, JEDEC IDs, bounds, comparisons and transmitted operation
counts. Failure leaves `backup-manifest.partial.json` with the error. The raw
stock codeplug/calibration page area 0x1000–0xC8FFF is also saved separately;
`stock-codeplug-pages.raw.bin` is **not** a vendor CPS project export. Retain the
complete data flash even when keeping this convenient subset.

Eight host tests exercise exact ROM packets, operation filtering, SPI writes,
range limits, both supported program densities, corrupted frames, changed/blank/
aliased captures, preservation of existing files, and the actual upstream sector
reader against a guarded simulated radio. They pass on macOS and Windows.
Live evidence and private unit backups belong in ignored local session folders.

## Identify the unit first

Record the exact displayed firmware string, hardware/board revision if already
known, cable chipset and a private local unit label. `DM32.00.*`, `DM32.01.*`
and `DM32.NRF.*` labels are not sufficient evidence of interchangeability.
`DP570UV` alone does not identify a compatible target.

The public analysis image is specifically `DM32.01.01.049`, SHA-256
`03107b6f600e7fa95105af8324cff349c0f17fe30db249c14d11e79baa8da97a`.
It is an update container, **not a full-chip backup**. The hardware revision of
its intended recipient has not been established here.

Identify both actual flash chips and capacities. A Q128 holds 16 MiB, Q16
2 MiB, and Q80/08 1 MiB. An early board photo establishes a particular
16 MiB + 1 MiB unit; another tool's Q16 selection does not identify your unit.
The available helper has no JEDEC-ID query exposed in its host protocol.
Its 24-bit SPI address generation can represent 2 MiB program flash, but
successful high-address access and absence of wrap/aliasing require target
validation. File length alone does not establish capacity.

## Two distinct entry paths

**Ordinary Baofeng update mode:** PTT + SK1 during power-on, green LED.
The stock host protocol starts with `52 -> 06`, followed by model negotiation
and `BFUV32-V2`. Community reports recommend entering green-LED mode before
seating the radio-end cable. Verify the sequence on this unit while it works.

**C7000 startup loader:** cable seated before power-on, wait for
`02 24 00 03`, reply `02 14 00 03`, download a temporary RAM program with
command `0x18`, then request execution with `0x1b`. This is not stock update
mode. The existing RAM helper subsequently interprets command bytes differently.
Never send a flash-helper request to the startup loader, or vice versa.

Both observed paths use 115200 8N1. Cable compatibility and control-line effects
must be tested independently. CPS success does not prove startup-loader access.
Our clients set RTS/DTR inactive before opening the explicit port. A cable
replug may change the device name. Do not infer a dead radio from a dark screen
or a stale serial-port path.

## Audit of the downloaded tools

The inspected source is `rogerclarkmelbourne/DM32` commit
`4b2185944a85a578ae33626922b763db6256533e`; exact source archive hashes are in
`sources.lock.json`. The repository provides tools, not a DM-32 firmware source
tree, and declares no repository-wide license.

| Component | Direct source observation | Limit |
|---|---|---|
| Ordinary updater | Detects `BFUV32-V2`, skips `0x100`, sends 1 KiB blocks with CRC16-XMODEM, zero-pads final block, requests erase and reboot | No whole-image hash/signature verification, target-family guard or post-write readback in this script; actual device erase/load boundaries unknown |
| Program reader | Defaults to address 0, stops at `0x100000`; parses an end argument but never uses it | A 1 MiB result cannot back up an actual 2 MiB chip |
| Q128 reader | Uses end-exclusive bounds, default `0x1000000`, 256-byte pages | Original checks only reply prefix, can silently accept truncated data or wrong payload checksum |
| Raw program writer | Fixed 1 MiB limit; optional start; pads pages; command `0x18` in the running helper | Can overwrite boot/identity/calibration; no expected hash/original-byte guard or readback; arbitrary start alignment unsafe |
| Raw data writer | Uses command `0x16`; copied 1 MiB size limit despite Q128 name | Not an audited full 16 MiB restoration tool; no readback or protected regions |

These are host-source findings. They do not audit the OEM Windows executable,
the ROM loader, or every device-side integrity/erase check. Header acceptance
does not establish unrestricted custom-image acceptance on the user's radio.

The helper read reply is **`02 05 checksum 03` plus 256 bytes**, where checksum
is the sum of payload bytes modulo 256. RAM download/execute replies instead
use fixed `02 05 00 03`. Disassembly confirmed this distinction; host tests
include nonzero checksums, corruption, fragmentation and truncation.

## Bounded flash-reading client

`tools/dm32_backup.py` extracts the helper using AST literals; it never executes
downloaded Python. It checks the original 3,840-byte helper SHA-256
`59de726d5a50cf17573d35e98d183a184d3cc5da81ca30e198fb061cd3ee0f46`.
Two exact original-byte guards then change conditional branches into
unconditional branches, bypassing the program/data flash-write dispatcher
entries. The resulting RAM helper hash is
`8d316eff7b2be89745162da739cecc8334e2befcd278fa6748bff44cc6bfde42`.

`experiments/modem/make_readonly_helper.py` reproduces these branch encodings
with GNU assembler and checks all 256 command routes. The patched dispatcher
retains reads `0x17` and `0x15`, rejects all other commands, and leaves original
files untouched. The write routines' bytes still exist; this is a bounded
dispatch audit, not formal verification of all helper behavior. The helper
also changes clocks/pinmux in RAM. Hardware behavior remains untested.

Start with an **offline plan**, substituting capacities only after identification:

```sh
python3 tools/dm32_backup.py --program-bytes 0x200000 --data-bytes 0x1000000 \
  --identity UNIT_LABEL --installed-firmware EXACT_DISPLAYED_VERSION
```

For a live read, install the hashed `requirements-hardware.txt` in a local
virtual environment (`python3 -m pip install --require-hashes -r requirements-hardware.txt`), power
the radio off, seat the cable, then add an explicit port and a new output folder:

```sh
python3 tools/dm32_backup.py --program-bytes 0x200000 --data-bytes 0x1000000 \
  --identity UNIT_LABEL --installed-firmware EXACT_DISPLAYED_VERSION \
  --port ACTUAL_PORT --out backups/UNIT_LABEL
```

Power on when the reader is waiting. It bounds the handshake, validates every
page's framing/checksum/length and reads both entire declared ranges twice.
At 115200, this is a substantial transfer; allow it to complete. Outputs use
exclusive creation. Interrupted dumps remain `.partial` and are never promoted
to a complete backup. Save the manifest and every untouched dump offline.
The manifest records source/helper hashes, ranges, unit identity, whole-file
and page hashes and every changed page. It does not silently mask changing
regions. Resolve differences and address aliasing before further experiments.

Export the unit's codeplug separately using its existing CPS; record the file
hash. Preserve its own calibration, identity and ALPU/licensing data wherever
located. Their exact ranges remain unknown, so retain **both complete chips**.
Never substitute another unit's dump or the archive's adjust-mode file.

## RAM diagnostic after backups

`python3 tools/build_probe.py` creates a 435-byte standalone CK803S diagnostic,
not a flash image. `python3 tools/run_ram_probe.py` validates it and shows an
offline plan. After two matching full backups, its specific live runner accepts
only a backup identifying the selected `DM32.01.01.049` target:

```sh
python3 tools/run_ram_probe.py --port ACTUAL_PORT --backup backups/UNIT_LABEL \
  --out backups/UNIT_LABEL/ram-probe-result.json
```

The probe inherits loader UART configuration, masks interrupts, uses the
stock-observed stack top, emits a CRC/endian check and reads WORK_MODE. It does
not configure RF, GPIO, PLL or PA, and issues no flash-write request. Require
the expected CRC/endian text, not merely an execute ACK. Power cycle afterward.
See `experiments/startup-probe/README.md` for exact boundaries and limitations.
The runner independently pins the reviewed probe hash and verifies the complete
ordered transcript. Its endianness check uses an actual volatile word store
followed by a byte load, verified in the generated C-SKY disassembly. Backup
identity/version fields remain user-attested; they are not cryptographically
bound to the attached port. Reconnect the same identified unit.

## Restoration and installation status

The restricted RadioFoundry laboratory image has been installed. Its application
write/readback, boot and cable checks pass. The host guard sent no bootloader or
data-chip mutations. At the owner's request those ranges were not read back
after installation. Actual stock restoration has not been required or demonstrated,
and no operational P25 installation is claimed.

For ordinary application recovery, use the original vendor image matching the
actual unit and its established stock update path. First prove that update-mode
handshake while the radio works. If that path fails, a proven startup-loader
read/helper path may support a later audited restore tool. This project's reader
does not restore chips; the downloaded raw writers remain research code.

Before authorizing any future persistent write in software, establish erase
boundaries, protect boot/unique regions, check the exact input hash and original
bytes, verify total size/range and padding, and read back every written byte.
Compare restored regions to this unit's originals. Never write an ordinary
update container at raw chip address zero. Do not deliberately corrupt boot to
test recovery. Startup-loader availability does not repair failed flash chips,
lost credentials/calibration or unproven board/cable paths; recovery is conditional.
