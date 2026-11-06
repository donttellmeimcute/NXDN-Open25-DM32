# C7000 RAM diagnostic (hardware-unverified)

This is an executable CK803S ABIv2 test of code loading, the C toolchain,
endianness, a CRC calculation, UART output, and a read of WORK_MODE. It does not
implement or test P25 RF. It is **not an ordinary Baofeng update image**.

Build with `python3 tools/build_probe.py`. It links at `0x10000`, uses a stack
at `0x50000` (matching the selected stock startup), disables interrupts, and
uses UART0's inherited loader configuration. Its only MMIO writes are UART0
transmit data. It does not configure PLL, GPIO, RF, PA, audio, or flash.
Every transmit poll has a fixed bound; if output fails, it stops in an idle
loop. Power cycling restores the normal boot path. UART inheritance and this
recovery behavior require testing on the actual target.

First complete the backup/recovery exercise in `docs/recovery.md`. Then use the
startup loader's `0x18` RAM download and `0x1b` execute commands to load the
exact built payload at `0x10000` and execute `0x10000`. The observed download
size field is `0x40` for a 256-byte block in the inspected upstream transfer,
consistent with a 32-bit word count; full ROM parser semantics are unverified.
Pad the final RAM block with zeros. Confirm `probe.manifest.json` and all
packet checksums before sending anything. `python3 tools/run_ram_probe.py`
validates the built payload and shows an offline transfer plan. After a
completed two-pass backup with matching file hashes, the specific live runner is:

```sh
python3 tools/run_ram_probe.py --port ACTUAL_PORT --backup backups/UNIT \
  --out backups/UNIT/ram-probe-result.json
```

The live runner requires the backup to identify `DM32.01.01.049`; it does not
silently accept another version/family. It performs no erase or flash-write
request. Its `0x18` packets are sent only after the power-on startup-loader
handshake and have addresses inside `0x10000..0x11000`. That command has a
different meaning once the community flash helper is running; never conflate
the two protocols.

Expected serial output at inherited 115200 8N1:

```
DM32-RAM-PROBE/1
crc16=000031c3
byte0=00000078
work_mode=<8 hex digits>
DONE; power-cycle to restore normal boot
```

The ROM reply ACK alone proves only that execution was requested. Require the
CRC/endian output before claiming successful custom code execution. Save the
actual log with unit identity and binary hash. No such hardware run has occurred.
