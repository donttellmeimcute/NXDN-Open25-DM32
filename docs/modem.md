# Native modem and audio investigation

Status: software and document investigation, 2026-10-26. No DM-32 was connected,
no register was read from a radio, no RF was emitted, and no image was flashed.
The physical RX/TX feasibility gates remain open. This is not a claim that the
chip cannot implement P25.

## Evidence and applicability

The chip reference is the [HR_C7000 user guide](https://www.connectsystems.com/products/top/radios/CS120D/HR_C7000%20Document%202.pdf),
revision history through V2.7 (2017-07-19), 412 pages. Local pristine copy:
`research/modem/HR_C7000_Document_2.pdf`, SHA-256
`d60f652e114d5103b2252be604089528775dffa263c703545d7cfed9fe490253`.
Page numbers here are the printed one-based numbers, matching PDF page index +1.
Pages 269, 295, 296 and 340 were rendered and visually inspected. Extraction
alone gave a misleading field order and reversed the Figure 70 type labels;
the mapping below follows the rendered figure.

The actual code map is for the public `DM32.01.01.049.bin`, size 869,688 bytes,
SHA-256 `03107b6f600e7fa95105af8324cff349c0f17fe30db249c14d11e79baa8da97a`.
This is an analysis target, not identification of the user's radio. Every
entry in `docs/modem-map.csv` records its input hash, file offset, runtime
address, original first 16 bytes, full range hash, evidence and confidence.
`experiments/modem/map_known_paths.py` regenerates that CSV and assembly slices
using the root analysis map. It checks the complete image hash before doing so.
These named functions are our annotations, not recovered symbols.

The [first-generation board photograph](https://github.com/yo3hjv/Baofeng-DM32/blob/b6c9703934eaccfacaacc10b64944c3721c18734/DM32%20Component%20side%20adnotated.jpg)
is preserved as `research/modem/DM32_component_side_annotated.jpg`, hash
`65907f88a4eedde604707f0e40141e41ff5e5e52a90c143a61d0caaf4f9a398f`.
Its author's annotations identify W25Q128JV, GD25Q80, FD6818/CS3417xa, C7000,
RDA5807, TDA2822 and GNSS. It establishes a studied early board, not the chip
capacities or routing of a currently connected unit. In particular, GD25Q80
is 1 MiB; a later Q16 program flash is 2 MiB. The chip manual's BK4815/RDA1846S
reference drawings must not be promoted into DM-32 wiring facts.

`research/modem/baocoder` is pinned at
`cef8125c7b2f3fffeecb8f2162a90d2bb0520ade` (2026-08-30). Its scope is AMBE+2,
not P25 IMBE. The main code license is ISC with separate provenance caveats
for firmware-extracted tables in its LICENSE. No baocoder code or tables are
copied into this project's modem tooling. Its original firmware-derived
addresses are not accepted as target functions merely because names match.

## What the selected stock image actually exposes

`docs/modem-map.csv` and `research/modem/code-evidence/*.asm` are the audit trail.
The following are direct GNU C-SKY ABIv2 instruction observations, interpreted
against the chip guide. Runtime behavior is still untested.

| Role | Runtime address | Concrete observation |
|---|---:|---|
| Physical TX test selection | `0x03021588` | Selectors 1/3 fill a 36-byte source with `0x5f`, selector 2 with `0x55`; writes offsets 0 and 36 through TX RAM copier, sets WORK_MODE=2, LAYER2_CONTROL=0x82, TXRX_CTRL=0x81 |
| TX RAM copy | `0x0300d3ec` | Byte copy from r1 to `0x16000000+r0`, uint16 byte count r2 |
| RX RAM copy | `0x0300d41c` | Byte copy from `0x160004b0+r0` to r1, uint16 byte count r2 |
| FM PTT setter | `0x0300c304` | Writes zero/one to `0x11000560`; this alone does not identify the PA key line |
| Modulation amplitude | `0x03021748` | Replaces the low two bytes of `0x11000114` with repeated uint8 r0, preserving the high half |
| PCM pointer setup | `0x00014164` | Sets global `0x33c4c` to `0x180000a0` and `0x33c50` to `0x18000000` |
| IRQ registration | `0x0301d040` | Vector 60 -> `0x2c4a4` -> capture `0x2e1d4`; vector 59 -> `0x2c49c` -> playback `0x2e238`; vector 61 -> system `0x2c454` |
| Capture IRQ | `0x0002e1d4` | Copies 80 halfwords from the capture pointer into a 160-sample ring, advances by 80 modulo 160 and signals a worker |
| Playback IRQ | `0x0002e238` | Writes 80 halfwords to the playback pointer from its queue or fallback buffer |
| Existing capture/play hooks | `0x03021a64`, `0x03021a68` | Each contains RTS/BKPT; called with PCM buffer r0 and sample count 80 in r1 |
| System IRQ collection | `0x0002c454` | Collects/clears SYS status, snapshots RX_TYPE_INFO at 0x390 and RX_INFO at 0x394, then signals a worker |
| Common IRQ entry | `0x0001064a` | Saves 68 bytes, switches IRQ stack to `0x4b7b0`, dispatches table `0x45514[(PSR-vector)-32]`, restores and executes RTE |

The stock raw-TX test is stronger evidence than a manual feature list, but it
does not establish alternating ownership, uninterrupted arbitrary data, P25
shaping, or on-air deviation. Its source is normally the same pattern in both
halves, which can hide repeated buffers and lost handoffs. Do not call it from
a startup RAM helper: it assumes initialized stock radio state and may key RF.
The routine at `0x0302150c` is a separate state/event setter, not that test's
entry point.

The PCM hooks are unusually useful future patch sites. A bounded hook could
copy samples and enqueue work while returning promptly with the caller's link
register preserved. IMBE must not be run synchronously inside these 10 ms
interrupt callbacks. Queue ownership, ISR nesting, watchdog policy and worker
integration still require an on-device trace before a production patch.

## Native UHF RF switching and calibration

The selected stock code exposes a complete *software* TX/RX switching chain.
It does not yet identify the physical PA net or prove a fail-safe release.
All function addresses below are included in the hash-guarded 51-entry code map.

`0x0300da88` accepts TX and RX tuning values in r0/r1. It compares each with
20,000,000, sets TX band bit4 and RX band bit3 at `0x3e318+3` when above that
threshold, and stores the tuning words at `0x38468` and `0x3846c`. The threshold
and native band limits are consistent with 10 Hz frequency units. That unit
interpretation remains an inference until the loaded channel and RF frequency
are observed together. The RFIC writer itself receives these words unchanged:
`0x2b56c` writes low16 to IC register 0x38 and high16 to register 0x39.

| Software control | Runtime address | Band bit=1 (native UHF interpretation) | Band bit=0 |
|---|---:|---|---|
| TX band enable | `0x2c838` | GPIO B23 low, B3 high | B3 low, B23 high |
| RX band enable | `0x2c878` | GPIO B2 low, B4 high | B4 low, B2 high |

Passing zero disables both pins in the corresponding pair. These are actual
stock GPIO writes, using GPIOB at `0x14100000`; they are distinct from the
baseband FM_PTT register. The names “TX/RX band enable” describe the proved
call context. Whether either pin directly powers a PA, switches an RF path,
or drives an intervening transistor requires a schematic or measurement.

`0x2b440` switches to TX: disable RX band, wait according to channel settings,
tune the TX word through `0x2b674`, enable the TX band, ramp the calibrated
level through `0x2b364`, and set bit0 of `0x3e1c0`. `0x2b4a0` restores RX:
if that flag is set, wait, ramp level to zero, disable TX band, clear the flag,
wait, tune RX through `0x2b6a0`, then enable RX band. The waits are observed
arguments to `0x2df80`; their physical durations have not been measured.
TX tune selects IC register 0x30=0 then 0xc1fe; RX tune selects 0 then 0xbff1,
with additional stock mode and interrupt handling. `0x0300e918` writes
IC register 0x30=0; that operation alone does not clear the external GPIO/DAC.

These switching routines execute in a worker, rather than directly in the RF
interrupt. Registered vector 64 (`0x2c400`) acknowledges RF_TX and writes
`0x38464=1`; vector 65 (`0x2c3d4`) acknowledges RF_RX and writes `0x38465=2`.
Both signal `0x38590` unless disabled by the stock state flag. The worker at
`0x0300dc94` waits on that object, checks channel-mode high nibble 0x10, and
dispatches the TX/RX functions. A separate worker at `0x0300dce4` waits on
event mask 0x200 and calls RX restore while the TX flag is active. This is an
observed restoration mechanism; attributing its event to battery, temperature
or transmit timeout would require tracing the event producers.

Power level has a recoverable calibration path. `0x0300db48` loads TX frequency
from channel bytes 4..7 and the power selector from byte 8 bits 2:1, calls
`0x0301b33c`, and stores its returned byte at `0x38498`. That lookup clamps
the frequency to configured band bounds, selects a frequency interval, reads
two adjacent calibration bytes using storage bank 2 via `0x0300f780`, and
interpolates. For band bit4=1, its frequency table starts at offset 0x314 and
its three level arrays at 0x40/0x45/0x4a; for bit4=0, those are 0x33c and
0xd0/0xd5/0xda. These are logical offsets in the stock storage API, not yet
proved absolute data-flash offsets. The ramp's byte output passes through
`0x2c3b4`, which writes `(uint8_t)level << 4` to DAC channel B,
`0x140f000c`. This dataflow strongly suggests PA power control, but no volts,
watts, safe maximum or physical PA connection is established by the binary.
Unique calibration must be backed up and retained.

For a future physical-mode experiment, a bounded RAM log of these stock worker
transitions, band state, tuning values and DAC target gives a concrete native
UHF hook context. Preserve the stock RF switching/calibration path while first
observing its timing. A direct call to FM_PTT, physical test selection, or the
state/event setter `0x0302150c` cannot substitute for the complete PTT release,
protection and RX restoration audit. No RF driver has been implemented from
these annotations and none of the routines has been called on a radio here.

A dedicated button candidate is also traceable. `0x03012f48` reads indexed
GPIO 43 (B11) and returns one when the input is low. `0x03012f58` requires four
differing polls before accepting a changed state, sets event bits 1/2 in
`0x3e1c0`, then invokes paired callbacks. The image's initial SAHB data maps
`0x18000838` to `0x03014e4c` and `0x180008a4` to `0x03014e00` (file offsets
0x660b8 and 0x66124). The former's normal path calls a substantial admission
routine at `0x030149fc(0)` before posting request 0x04; the latter normally
posts request 0x08. This pairing and its TX-state use strongly suggest physical
PTT, but the button/pin association and current callback-table contents still
need a runtime trace. The admission routine contains policy and mode branches
whose individual meanings have not been assigned. The press callback also has
service/calibration branches that write storage. Calling these routines from
a diagnostic is inappropriate; a bounded observer may log their existing
execution after backup. A future replacement must preserve their relevant
admission checks and release behavior, rather than bypassing them.

## Startup clock model

The copied IRAM startup calls `0x2c9b4`, which preserves BPLL register 0x10's
upper byte and sets its low24 bits to 0x040712. Manual pp. 61-64 decode that as
reference divider 1, feedback divider 28, and postdividers 2 and 2. With the
guide's 24 MHz reference, integer mode and fractional part zero, this gives
672 MHz VCO and 168 MHz BPLL output. The code sets register 0x24 low4 bits to 2:
CPU:AHB=1:1 and AHB:APB=2:1, yielding the nominal model 168/168/84 MHz.
It explicitly stores 168,000,000 at `0x3a610` and `0x3a608`, 84,000,000 at
`0x3a60c`, and 24,000,000 at `0x3a614`, consistent with that interpretation.

The function triggers both PLL reconfiguration bits, waits for lock, switches
baseband and SoC clock sources to PLL, waits for ready, and enables clock gates.
It does not overwrite the BPLL upper control bits or fractional register.
Thus the model depends on inherited/reset state; these values are static
configuration and bookkeeping, not measurements. Other stock power states
can change clocks later. Do not equate the helper's 84 MHz UART reference
variable with a measured 84 MHz CPU. Read the clock registers and compare a
hardware-timed GPIO/sample interval before deriving real-time IMBE headroom.

## Physical RX: documented permutation, unresolved continuity

Guide pp. 294-297 describes physical mode with channel decoding bypassed, but
the receive interface remains a DMR deframer. `WORK_MODE` is `0x11000100`;
`0x02` selects continuous physical mode and `0x22` slotted physical mode.
The documented RX setup also uses LAYER2_CONTROL `0x11000400=0x40` and
TXRX_CTRL `0x11000408=0x41`. PHY receive completion is SYS status bit 11
(`0x800`); readiness is bit 12 and abnormal slot boundaries bit 16.
SYS list/mask/clear are at `0x11000398/39c/3a0`; mask bits set to one enable.

RX RAM is based at `0x160004b0`. Continuous-mode bytes at offsets `0x30..0x53`
contain 288 bits: CACH 24 plus a DMR-sized 264. Slotted mode provides 264 bits
at offsets `0x30..0x50`. Retaining the first three continuous bytes is essential
when testing whether this arrangement can represent every P25 bit.

Rendered Figure 70 gives these continuous RAM field sequences:

| RX_TYPE_INFO[1:0] | RAM bit sequence | Reconstruction hypothesis for on-air order |
|---|---|---|
| 0/1 | CACH24, sync/embedded48, info108, info108 | CACH24, info108, sync/embedded48, info108 |
| 2 | CACH24, slot-type10, slot-type10, sync48, info98, info98 | CACH24, info98, slot-type10, sync48, slot-type10, info98 |
| 3 | RC, not defined by this figure | Reject until separately mapped |

`experiments/modem/stream_probe.py rx-air` implements these exact permutations,
preserves all 288 bits, requires consecutive sequence numbers and refuses
33-byte continuous records or unknown RC layout. This is a manual-derived
reconstruction hypothesis. It must first be checked using an independent
known DMR bitstream and captured RAM before it can establish P25 continuity.

The four 48-bit programmable data-sync pairs start at `0x1100013c/140` (MS),
`144/148` (BS), `14c/150` (TDMA1) and `154/158` (TDMA2). High-register bit 31
enables the associated matcher outside layer 3. The guide does not promise
arbitrary unsynchronized bit streaming, arbitrary sync periodicity, or free
choice of voice/data interpretation. P25 sync acquisition alone will not pass
the gate. Capture an entire independent HDU/LDU1/LDU2/terminator stream,
including units crossing 36-byte boundaries; recover valid BCH NID/NAC and
complete independently comparable CAI units without insertion/deletion.

The manual RX example's RF_MODE `0xa0000060` selects an IF-oriented reference
route. It must not overwrite a board's actual AF/IQ routing. The selected
image's initialization near `0x03023654` explicitly enables RF_MODE bits 25
and 24 (AF averaging and AF input), with a later delay value 14000 at 0x1b4.
That differs from the guide's AF example delay 2000. Preserve the board's
observed route and calibration; vary only the intended physical-test controls
after confirming their masks. The exact signal on the board remains unmeasured.

## Physical TX: plausible stream route, waveform still a gate

Guide pp. 294-295 gives TX RAM halves `0x16000000..23` and `0x16000024..47`.
36 bytes represent 144 symbols and nominally 30 ms at 4800 symbols/s. The
documented mapping is 00 -> +1, 01 -> +3, 10 -> -1, 11 -> -3. Guide p. 273
shows nominal digital deviation +/-648 and +/-1944 Hz, not the P25 values
+/-600 and +/-1800. RF_MODE bit 7 reverses RF polarity; the selected image
has explicit code that clears it for a platform selection. DAC sign constants
in another modem implementation are not proof of RF polarity on this board.

Changing gain by the nominal ratio 25/27 is only an initial hypothesis.
Measure both modulation paths and their crossover; retain unique calibration
and verify the actual +/-600 and +/-1800 levels, symbol rate, pulse response
and occupied spectrum with an independent receiver. The guide exposes receive
FIR coefficients, but this investigation did not establish a programmable
digital TX shaping coefficient array. Changing deviation does not change
DMR pulse shaping into compliant P25 automatically.

Use distinct successive 36-byte chunks, with a known long stream crossing
every boundary phase, and timestamp both IRQ and ownership transitions. Look
for CACH, sync substitution, padding, repeated chunks, gaps and underruns.
After that, send the independently validated CAI test stream from
`tests/protocol/vectors/clear-call.bin`. Repeatable PTT release must stop both
baseband transmission and PA keying, cancel queued sends, and restore RX.
The FM PTT setter and physical bit7 enable are not a complete PA fail-safe.

## Native sample fallback and microphone/speaker path

The guide's CPU voice interface (pp. 269-270) and selected code agree:
80 signed 16-bit samples per 10 ms at 8 kHz. Microphone buffer is
`0x18000000..009f`; playback starts at `0x180000a0` and contains exactly
160 bytes, ending at `0x1800013f`. The guide also prints an endpoint of 0x140;
that would be one byte too many if interpreted as inclusive. Earliest sample
occupies the low halfword of a 32-bit transfer. Capture IRQ28/vector60 clears
INTERP_CLEAR bit4; playback IRQ27/vector59 clears bit5. Audio control is at
`0x11000080`, with capture/play AHB enables bits4/5.

FM sample access is a separate potential demodulation/modulation route:

| Path | Low/high half | Representation |
|---|---|---|
| FM TX RAM | `0x16000030..22f` / `0x16000230..42f` | 256 samples per half; high PCM byte at lower address |
| FM RX RAM | `0x160004e0..6df` / `0x160006e0..8df` | Same, 512 bytes per half |

The guide describes 32 ms of 8 kHz PCM per half (pp. 271, 340). Its endian
layout differs from the CPU PCM interface. `FM_ADDR_SW` at `0x1100056c` reports
which half CPU should access (1 low, 2 high); SYS bits15/14 signal FM TX/RX.
`AUDIO_CONTROL[0]=1` selects TX RAM as FM source. `FM_BANDWIDTH` at
`0x11000500` has bandpass enable bit6 and TX pre-emphasis bit4, both documented
as disableable (pp. 332-333). This makes an all-native software fallback worth
testing, but does not prove that the RX buffer is a suitable discriminator
signal or that TX RAM reaches the modulator without undesired filtering.

Required fallback measurements are an RX transfer function and known C4FM
capture with bandpass disabled, including DC behavior, clipping, de-emphasis,
sample order and actual sample rate. For TX, inject known PCM at 8 kHz with
pre-emphasis/bandpass disabled and measure interpolation response and deviation.
The sample-rate conversion and complete waveform must run on the C-SKY.
Passing these tests could justify embedded software C4FM; normal speech PCM
availability alone cannot. No unmodified-board 38.4 kHz IF/IQ memory tap was
established. Chip SPI/IF/IQ pin options are not a usable fallback if reaching
them would require wires or board changes.

## Smallest bench sequence and evidence format

1. Complete the read-only flash identification, two stable reads, complete
   program/data backups and codeplug exercise first. Confirm loader handshake
   and range limits while stock RX/TX still work. This step needs the actual
   radio and cable; another unit's dump is not a substitute.
2. Load the smallest non-RF RAM probe through the demonstrated startup loader.
   Keep clocks/pinmux inherited from ROM initially; emit a fixed marker through
   the already used UART0, with bounded polling. This validates custom compiler
   output and return/boot behavior without application or flash mutations.
3. After a proved logging path, collect baseline RX register snapshots in FM
   and DMR. `experiments/modem/register-snapshot.json` distinguishes ordinary
   reads from clear/ack/data-pop registers. Never dump UART RBR, timer EOI or
   W1C interrupts as if they were passive RAM. Codec and modem RAM use byte
   access; ordinary registers use aligned words. Capture to RAM first and
   export later: 115200-baud serial output itself can overrun 10 ms deadlines.
4. Validate bounded microphone/speaker hooks before P25. Copy two 10 ms chunks
   into each 20 ms IMBE input; play known decoded speech via both 10 ms speaker
   halves. Measure encode/decode worst-case separately, IRQ occupancy, queue
   depth, drops, stack high-water and full-pipeline deadlines.
5. RX physical experiment: retain stock RF route, log every raw 36-byte record
   with monotonic sequence, hardware timer, IRQ status, RX_TYPE_INFO, PHY state,
   overrun counters and buffer-copy completion time. Independently replay known
   DMR, then P25, then polarity changes and signal loss/reacquisition. Compare
   full streams and frame coding, not only correlator hits.
6. TX physical experiment: after identifying inactive-buffer ownership and PA
   stop path, use a bounded finite pattern with distinct chunks. Follow with
   known CAI. Observe RF independently, including PTT-release/reset/error stop.
   If digital path fails, execute the FM sample fallback measurements above.

Raw RX JSONL records consumed by the offline reconstruction tool use:

```json
{"sequence":0,"tick":12345,"rx_type_info":"0x1","raw_hex":"<exactly 72 hex digits>"}
```

Keep all additional timing/error fields; the tool does not fabricate them.
Then run, from the repository root:

```sh
python3 experiments/modem/stream_probe.py plan tests/protocol/vectors/clear-call.bin artifacts/modem/clear-call
python3 experiments/modem/stream_probe.py rx-air captures/native-rx.jsonl captures/native-rx.air
python3 experiments/modem/stream_probe.py compare artifacts/modem/clear-call.rawbits captures/native-rx.air --output artifacts/modem/comparison.json
python3 -m unittest discover -s experiments/modem -p 'test_*.py' -v
```

The planner's explicit zero tail only completes the last 36-byte buffer; it is
not CAI termination and its report says so. The comparator reports exact
equality and unique 64-bit anchors every 288 bits, including changes in stream
offset. It does not silently remove CACH or correct a bit slip. For repeated
or error-damaged anchors it reports nonunique/missing evidence. Explicit
`--mapping` hypotheses include dibit sign inversion (XOR 0xaa), dibit bit swap
and bit-order reversals. Choosing a transform is not an interoperability test.
Ten host tests passed, including every one-bit basis through all three
supported layouts, inserted 24-bit CACH, lost dibit, duplicate chunk and
source-preserving segmentation. These are tooling tests, not modem tests.

## RAM diagnostic UART evidence

The community helper extracted from Roger Clark's reader has SHA-256
`59de726d5a50cf17573d35e98d183a184d3cc5da81ca30e198fb061cd3ee0f46`,
3840 bytes, loaded at `0x10000`, execution command entry `0x10974`.
`research/modem/loader-helper.asm` is an ABIv2 disassembly wrapper, not changed
helper bytes. It directly accesses UART0 `0x14030000`:

- `0x10870`: transmit bytes, polling USR+0x7c bit1, writing THR+0x0.
- `0x1089c`: receive bytes, polling LSR+0x14 bit0, reading RBR+0x0.
- `0x10834`: baud initialization via LCR+0x0c DLAB, DLL+0x0, DLH+0x4;
  clears IER, selects 8-bit words and writes FCR 0x67.

The helper configures a clock variable to 84 MHz before selecting 115200 baud.
That is static helper configuration, not a measured stock CPU clock. Its full
startup also changes PLL, pinmux and watchdog state. A new minimal probe should
not transplant that entire setup or inherit unbounded polling. ROM UART state
remaining valid after its jump command is a testable assumption, still
unverified here. No sample, modem, codec or RF test has run on a real DM-32.

### Reader response checksum and address audit

The live reader must distinguish the ROM's fixed `02 05 00 03` acknowledgements
from the RAM helper's read-page response. A successful flash read returns
`02 05 SS 03` and 256 bytes, where SS is the sum of the payload modulo 256.
Requiring SS=0 is wrong and does not validate the payload. Direct code evidence:
program read `0x10d6e..0x10d7e` sums bytes, `0x10d80` loads the ACK template,
and `0x10d88` substitutes its third byte. Data read `0x10dc4..0x10dd4` uses the
same scheme. A known page containing bytes 0..255 requires SS=0x80. This issue
was found during independent review before any serial execution and corrected
in the root backup tool.

The program-read handler (`0x17`) parses a 32-bit big-endian host address at
`0x10c08..0x10c0a` and emits SPI READ opcode 0x03 followed by address bits
23..16, 15..8 and 7..0 at `0x10d46..0x10d66`. Data read (`0x15`) does the same
at `0x10d9a..0x10dbc`. Thus this binary's read-address construction has 24 bits,
not a 1 MiB mask. The host script's original 1 MiB loop limit is a separate
limitation. Addresses 0x100000..0x1fffff are representable by the helper, but
that is not proof of a 2 MiB chip, correct pin/chip select on every revision,
or absence of read aliasing. The 16 MiB data limit is exclusive: address
0x1000000 would wrap in the helper and must be rejected by the host.

### RAM-only removal of flash-write command reachability

The pristine helper includes host write commands 0x18 (program) and 0x16 (data).
Its command byte is not included in the request payload checksum. Keeping
reachable write handlers in the first backup helper introduces an unnecessary
failure path even when the host intends to send only reads.
`experiments/modem/make_readonly_helper.py` makes exactly two guarded changes to
the RAM helper, without changing any flash or pristine file:

| Helper offset/runtime | Expected -> replacement | Effect |
|---|---|---|
| `0xb4c / 0x10b4c` | `45 08` -> `45 04` | BT becomes unconditional BR to 0x10bd6, bypassing program-write entry 0x10b4e |
| `0xc5a / 0x10c5a` | `37 08` -> `37 04` | BT becomes unconditional BR to 0x10cc8, bypassing data-write entry 0x10c5c |

GNU assembler independently reproduces both original BT and replacement BR
encodings; GNU objdump confirms both destinations. A bounded interpreter of
the actual dispatcher instructions and literal pool traces all 256 commands:
0x17 reaches program-read, 0x15 reaches data-read, all others reach NACK.
Negative control with each original conditional restored reaches its original
write entry. Another 131,070 malformed prefix/suffix combinations cannot reach
a retained read handler. Tests check input mutation/truncation and exact byte
differences. This is a narrowly scoped dispatcher proof, not full CPU emulation
or formal verification of the entire helper, its startup, peripheral code or
hardware. The write routines remain as unreachable code under this entry path.

Build with `python3 experiments/modem/make_readonly_helper.py`. Output is
`artifacts/modem/readonly-helper/helper.bin`, 3840 bytes, SHA-256
`8d316eff7b2be89745162da739cecc8334e2befcd278fa6748bff44cc6bfde42`, still
loaded at 0x10000 with entry 0x10974. Its manifest records original bytes,
source/output hashes and all proof limits. This affects the helper after it
starts; the ROM loader's separate 0x18 RAM-download operation is unchanged.
Seventeen modem-tooling/helper tests passed; there has been no hardware run.

## Remaining uncertainties

Unresolved: target board revision and actual flash IDs/capacities; exact PA/PTT
and battery/thermal control integration; runtime CPU/bus/sample clocks and
available RAM; physical RX synchronization persistence and reconstruction;
physical TX buffer ownership, padding and shaping; native FM sample fidelity;
IMBE real-time deadlines with interrupts; RF and independent voice tests.
There is no demonstrated need for a soldered path, and no demonstrated proof
that a qualifying native path works. The next decisive item is a recovered,
backed-up actual target with the programming cable and an independent known
P25 source/observer. Software file results alone cannot close these gates.
