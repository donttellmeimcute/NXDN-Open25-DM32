# RadioFoundry conventional development derivative (November 5)

See [conventional.md](conventional.md) for scope and exact source decisions.
Run these commands from the repository root:

```sh
sh tools/bootstrap_newlib.sh
python3 tools/test_voice.py
python3 tools/test_native_modem.py
python3 tools/build_radiofoundry.py --stack-check --with-local-stock --verify-rebuild
python3 tools/build_radiofoundry.py --disable-native --stack-check
python3 tools/build_radiofoundry.py --disable
python3 tools/c4fm_reference.py generate artifacts/c4fm/calibration.csv
```

The build uses the pinned upstream commit and FreeRTOS submodule, our C-SKY
compiler, genuine pinned newlib, and codec objects rebuilt with
`-mno-high-registers`. `--with-local-stock` is optional and requires a recognized
local donor in ignored `research/originals`; it never fetches or commits a donor.
The generated directory includes original MIT notices and a GPL derivative
notice. Hash manifests accompany the ELF/BIN/map. A forced rebuild compares
application bytes. No command above opens a radio or flashes anything.

The feature definitions default off. This builder enables the offline P25
worker and the bounded native modem test interface; `--disable-native` omits
the native interface, and `--disable` omits both. Native tests include explicit
RF control and remain hardware-unverified; see [native-modem-tests.md](native-modem-tests.md).
A plain application lacks the donor AMBE material; the separate
`.codec.bin` contains locally merged stock material. Neither is a P25 RF release.
Keep all generated outputs internal and do not distribute the stock material.

When an appropriately backed-up radio is available and this development build
has separately passed its installation/recovery gates, the explicit diagnostic
command is:

```sh
python3 tools/p25_lab.py --port /dev/EXPLICIT_DEVICE --run
```

It has not been hardware tested. It runs no RF test and cannot establish P25
RX/TX. `p25.tx_block_max_us` covers encoding one 20 ms block including LDU coding
on every ninth block; `p25.rx_unit_max_us` covers a raw record including any
nine-frame decode burst. Both include preemption. `p25.stack_free` is the
measured worker high-water margin. Host reference PCM CRC is `4e158373` for the
pinned synthetic selftest. First compare the on-device software result; then
perform the independent RF/sample continuity gates in the implementation notes.

The remainder of this file documents the earlier binary-research tooling.

# Reproducible source and software builds

Actual build host: macOS 26.5.1, arm64, Apple command-line tools; Python 3,
Clang C/C++, Make. No remote compute or hardware processing is used in these
tests. No programming serial device was present.

## Inputs and toolchain

`sources.lock.json` records original URLs, repository commits/dates, SHA-256
of deterministic Git archives and direct binary/PDF downloads. `make sources`
fetches exact commits into pristine `research/` trees and refuses changed
checkouts or unexpected hashes. It extracts the loader helper from a Python
literal without importing downloaded code. `python3 tools/sources.py verify`
checks existing inputs. `record` is a maintainer operation to capture reviewed
pins, not something required to reproduce the build.

`make toolchain` verifies the pinned GNU binutils 2.45.1 and GCC 14.3.0 tarballs,
builds native `csky-elf` tools out of tree and installs only under `toolchain/`.
It builds C/C++ compiler stages and libgcc; it does not fabricate a target
operating system, libc or C++ standard library. It uses GCC's pinned prerequisite
script with SHA-512 verification. Additional retrieved archive SHA-256 values:

| Prerequisite | URL prefix `https://gcc.gnu.org/pub/gcc/infrastructure/` | SHA-256 |
|---|---|---|
| GMP 6.2.1 | `gmp-6.2.1.tar.bz2` | `eae9326beb4158c386e39a356818031bd28f3124cf915f8c5b1dc4c7a36b4d7c` |
| MPFR 4.1.0 | `mpfr-4.1.0.tar.bz2` | `feced2d430dd5a97805fa289fed3fc8ff2b094c02d05287fd6133e7f1f0ec926` |
| MPC 1.2.1 | `mpc-1.2.1.tar.gz` | `17503d2c395dfcf106b622dc142683c1199431d095367c6aacba6eec30340459` |
| gettext 0.22 | `gettext-0.22.tar.gz` | `49f089be11b490170bbf09ed2f51e5f5177f55be4cc66504a5861820e0fb06ab` |

The successful binutils configuration uses `--with-system-zlib`; the initial
bundled-zlib attempt failed on this Mac SDK. The successful GCC build obtains
GMP/MPFR/MPC locally through the verified prerequisite archives. No global
Homebrew repair or toolchain replacement was performed.

## Static analysis

`make analyze` verifies the entire selected stock image and exact startup
literal bytes before extracting relocated segments. It emits GNU ABIv2 assembly
and `artifacts/map-049/memory-map.json`, then regenerates the modem function CSV.
Analysis ELF wrappers have explicit ABI flags; plain binary objdump decoding
was shown to split 32-bit instructions incorrectly without them. See the
firmware map for this failed experiment and the Ghidra module limitation.

For the independently verified read-only helper patch:

```sh
python3 experiments/modem/make_readonly_helper.py
```

It writes only generated local files and verifies both changed branch encodings
with GNU assembler/objdump. It never opens a serial port.

## Tests

`make test` runs the host image/backup/probe guards, independent OP25 CAI
comparisons, sanitizer fuzz tests, IMBE against mbelib/OP25 vectors, a complete
synthetic PCM/CAI pipeline, modem permutation/continuity analysis and the small
freestanding runtime against host libc. It requires no radio or purchased
equipment. Deterministic synthetic audio is the portable primary regression
input. Optional locally generated speech and its provenance are documented in
the codec report; no perceptual quality or on-radio claim follows from it.

Results are recorded in `results/validation.json`, `tests/protocol/results.json`
and `experiments/codec/results/`. Full local logs are under `logs/`.
Timing values from host tests, particularly instrumented builds, must not be
used as CK803S deadline measurements.

## Target components and diagnostic

`make target` builds the full codec and CAI as actual ELF32 C-SKY ABIv2 objects,
with explicit `-mcpu=ck803s -mlittle-endian -msoft-float -ffreestanding`.
It links them relocatably with `src/runtime/memory.c` and the built libgcc and
fails on any unresolved symbol. The codec's small compile-only header set
declares standard memory routines and placement new; it supplies no fake runtime.

The recorded linked component object is `artifacts/p25-codec-ck803s.relocatable.o`:
34,072 code/constant bytes, zero initialized writable data, 12 BSS bytes.
Its caller-owned contexts and stacks are additional. It is not executable radio
firmware. The probe links independently at `0x10000`, is 435 bytes, uses no
BSS/runtime and has its reviewed SHA pinned in `tools/run_ram_probe.py`:
`d3e9f7f0c58a91c701ff8d36d24cf2ef02e946f338169ec61bc753806ed4201a`.
Changing the probe requires a new source/disassembly review and pin update.

Codec debug/source paths are mapped to stable prefixes and GNU archives use
deterministic mode. Builds in two separate generated directories produced
identical codec object/archive hashes; `experiments/codec/results/reproducibility.json`
records them. This establishes reproducibility for those components, not a whole
radio image across every host/compiler. The GNU compiler binary itself has not
been established bit-identical across hosts.

## Update packaging remains gated

`tools/dm32_image.py` inspects/extracts the exact original container and can apply
a same-length patch manifest only with exact input hash, target ID, original
bytes, non-overlapping payload ranges and expected output hash. It preserves
the original header and refuses output overwrite. No actual stock/P25 patch
manifest has been released.

There is no documented verified application erase/load range, readback-capable
writer, retained-settings location or fully integrated P25 application yet.
Consequently, no script packages the components as a flashable-looking update,
and there is no installation/release-candidate hash to advertise.
