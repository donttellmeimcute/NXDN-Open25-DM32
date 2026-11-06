# Exact-image startup and integration map

Historical integration assessment below is from2026-10-26. The2026-11-05
RadioFoundry source review supersedes the binary-only/source-availability and
preferred integration-route discussion: see `radiofoundry-assessment.md`.
The exact-image static observations remain evidence for their stated input hash.

Target: `DM32.01.01.049.bin`, 869,688 bytes, SHA-256
`03107b6f600e7fa95105af8324cff349c0f17fe30db249c14d11e79baa8da97a`.
Original M7OCM branch commit `8efe0e504392aefbfbd84169df6f2da1d32cdd3c`
and Roger mirror commit `4b2185944a85a578ae33626922b763db6256533e` match
byte-for-byte. Supported **analysis** family: this exact `DM32.01.*` image;
no physical unit or deployable P25 hardware family has been validated.

## Container and boot

The first nine bytes are `BFUV32-V2`. Bytes 9..252 are `0xff`; bytes 253..255
are `END`. The community stock loader skips this 256-byte header. The remaining
869,432-byte payload SHA-256 is
`97106fc968c848d552a5ee906cc81173e49fd7069fec21d85af631e1cb2e70e5`.
No other container fields are inferred from this one image. Host source does
not prove absence of device checks, relocation or protected erase regions.

The payload's runtime base `0x0300c000` is supported by PC-relative literal
references, startup copy sources within that image and RAM vectors pointing
back to its exception stubs. The physical program-flash placement under the
updater is not thereby established. Do not equate file offset, runtime virtual
address and raw flash address.

`tools/analyze_image.py` checks the complete image hash and startup literals,
then emits these analysis segments and disassemblies:

| Region | Original container offset | Runtime range (end exclusive) | Bytes | Evidence |
|---|---:|---:|---:|---|
| Payload/XIP candidate | `0x100` | `0x0300c000..0x030e0438` | 869,432 | Internal references and startup entry |
| Initialized IRAM | `0x0b3ca0` | `0x10000..0x2f098` | 127,128 | Startup copy loop `0x0300c046..05a` |
| Initialized separate SRAM | `0x066080` | `0x18000800..0x18004bfc` | 17,404 | Startup copy loop `0x0300c030..044` |
| Zeroed data/BSS | no file data | `0x2f098..0x4a7b0` | 112,408 | Startup zero loop `0x0300c05c..072` |

Startup sets SP to `0x50000`, VBR to `0x10000`, configures core-local control
registers and calls `0x2e158`. The vector table at `0x10000` includes reset
entry `0x0300c000` and stock exception stubs. Common IRQ entry `0x1064a`
saves 68 bytes and uses a separate stack at `0x4b7b0`; see the modem map.
These observations identify initialization and dispatch, not a measured stack
budget or a proven free code/data allocation.

AMBE addresses from another research image are not imported automatically.
For example, baocoder's cited SRAM table file mapping starts at `0x636c0`,
which differs from this image's directly observed initialization sources.
Image hashes and relocation must agree before a function/table label is reused.

## ISA and decoder validation

The manual identifies CK803S. Mixed 16/32-bit instructions, little-endian
halfwords, register calls/returns and compiler-produced ELF attributes agree
with C-SKY ABIv2. The built toolchain targets `-mcpu=ck803s -mlittle-endian
-msoft-float`; no FPU or ARM machine code is used. Argument registers are
r0..r3, return r0, stack r14 and link r15, consistent with byte-copy callsites
and compiler output. Stock interrupt conventions require the actual wrappers.

The candidate leommxj Ghidra module compiles in Ghidra 12.1.4 but lacks MTCR/MFCR
and other operations, includes NOP semantics and an always-false unsigned
comparison warning, and stops at startup `0x0300c002`. Its plausible output is
not trusted. `tools/DM32Initial.java` preserves that failed experiment.

GNU binutils 2.45.1 provides the working disassembler. A subtle raw-input issue
was reproduced: `objdump -D -b binary -m csky:ck803` alone leaves ABI flags unset
and splits valid 32-bit instructions into misleading 16-bit output. The analysis
tool wraps extracted bytes in ELF32 and sets **wrapper-only** flags `0x20000009`
(ABIv2 + CK803). Generated code naturally carries `0x21000009` and CK803S
attributes. No firmware bytes are changed by this wrapper. The stock word
`01 c0 20 64` then decodes to `mtcr r1, cr<0,0>`, and independently assembled
branches reproduce the guarded helper patch encodings exactly.

## Native paths and integration choice

The detailed, hash-pinned function ledger is `docs/modem-map.csv`. It identifies
native modem memory copies, uncoded TX test configuration, audio pointer setup,
80-sample capture/playback IRQs, system events and available PCM hooks. RF tuning,
PTT and PA findings with confidence levels are maintained in `docs/modem.md`.
Configuration/calibration/identity/ALPU ranges and the complete protection path
remain unresolved. No patch may overwrite these based on an assumed layout.

The chosen first integration route is **stock-assisted, RAM-only experiments**,
using this exact map to establish native RX/TX and audio behavior. A publicly
buildable DM-32-specific OpenDM32 tree was not found; the experimental binary
does not supply source or a verified P25 driver. The codec and CAI components
are independently buildable GPL source. A dedicated P25 application is the
prospective first radio milestone, while selectable stock-mode integration is
deferred until runtime resources and source/distribution terms are resolved.

The linked codec/CAI software has 34,072 bytes of code/constants and 12 bytes
static writable data in this build. Caller-owned codec contexts, queues,
stacks and the radio supervisor are additional. Fit in a nominal 288 KiB RAM
does not establish free space in the running stock application, or completion
within the 20 ms audio deadline. There is no verified insertion address, no
patched stock entry, and no flashable application artifact.
