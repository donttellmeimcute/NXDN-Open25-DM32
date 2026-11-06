<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# First-boot installation record

This records the initial installation. Subsequent receive-test images and normal
remote restart results are in [live receive tests](live-receive-tests.md). The
installer now pins the latest reviewed image and its saved verified predecessor.

November 5, 2026. The owner authorized installation of the reviewed restricted
first-boot image and a boot/cable check. **Installation and boot/cable checks
passed.** The entire installed application matches the expected bytes on the
PC and independently on the Mac. Two health receipts show increasing uptime,
the expected first-boot version and `.048` codec, all restriction flags set,
zero denied attempts and no storage writes/claim. A stable cable screenshot
shows the idle FM home screen. No RF transmit test or P25 modem/codec test ran.
See [live results](../results/firstboot-installation.json).

## Installer and write limits

For this initial installation, `tools/install_firstboot.py` pinned the raw candidate SHA-256 to
`81e9242d7a7bcb0a7e48d4c08031ac29353623325c9c9fca03045babcf5c1a25`.
It validates the saved two-pass backups and private staged package before
opening COM5. It uses the same hash-pinned universal RAM monitor as the backup
reader, at 115200 baud with RTS/DTR inactive. Both chip IDs and the echoed DM32
monitor configuration must match.

The host guard permits only a precomputed sequence of write-enable, 4 KiB
sector erase and exact 256-byte page payloads on the **program** flash. It
refuses chip/block erase, arbitrary writes, writes below offset `0xC000`, writes
to the data chip, changed/reordered/repeated page requests, arbitrary calls and
unrelated register access. Only sectors differing from the saved original are
included: this image has 234 changed application sectors and 2,024 nonblank
pages. The first 48 KiB bootloader is outside every mutation range.

Each changed sector is read back immediately after writing. A mismatch or
transport failure stops the process; mutation requests are not automatically
retried. After all planned writes, the complete installed application is read
back and compared byte for byte, including erased padding up to offset 1 MiB.
Reset is refused unless that comparison passes. Only then can the exact seven
reviewed RAM-register writes restore the loader clocks/XIP state and restart
the CPU. These are reset/control-register writes, not additional flash writes.

The pinned monitor binary contains other operations and its C7000 source is
absent upstream; the restriction is enforced on every outgoing host request.
Host enforcement and successful readback are not a formal audit of the monitor
or proof against hardware faults.

## Owner-requested omission of original-firmware reads

The initial run finished its additional original-program read and was midway
through data-chip CRC checks when the owner said, “don't read existing
firmware.” The installer process was stopped before its write phase. The log
had no installation/write progress; that code path sends no flash mutations
before completing the checks.

The revised run uses `--use-saved-backup --attach-known-monitor`: it reuses the
known pinned helper still running in RAM and the verified saved backups, with
no additional physical power cycle. **No pre-install firmware or data-array
reads are permitted in this mode.** Chip identification, RAM configuration and
flash busy/status reads remain necessary to perform the installation.

The guard also refuses bootloader and data-array reads in this mode. During
writing, only already-written application sectors may be read back. The final
application readback is allowed only after every mutation in the plan was sent.
There is no post-install bootloader readback or full data-chip CRC claim for
this run; those ranges are excluded from outgoing writes by construction.
The existing full backups remain preserved.

## Tests and receipts

All 64 host Python tests pass. Eight installer tests also pass on Windows,
using the actual upstream monitor protocol simulator. They exercise exact
application writes, preservation of boot/data memory, rejection before
authorization, range/content/order limits, readback corruption, reset refusal
on bad verification, and the owner's no-original-read mode. The last mode
explicitly rejects pre-install reads and later boot/data reads while retaining
new-application verification.

The Windows starter checks all four Python tool hashes and the explicit CH340
COM5 identity. It keeps its SSH parent alive until the child exits, caches the
process handle for a reliable exit code, and records a unique session path and
stdout/stderr in `tools/install-active.json`. Unit images, readbacks, operation
counts and live receipts remain in ignored internal artifacts/reports.

After a verified restart, the query-only health collector checks restriction
flags, cable echo, version and radio/task/storage views. It does not request TX,
change settings or start a modem/codec experiment. A passing receipt establishes
application execution and cable communication, not P25 RF or audio operation.
