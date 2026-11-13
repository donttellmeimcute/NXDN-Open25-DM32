# Source and license record

New code in `src/`, `tools/`, `tests/` and `experiments/` is GPL-3.0-or-later
unless a file carries a different original notice. Preserve original notices
and the included full GPL license text. Public research originals remain
separate and are not relicensed by this project's LICENSE.

| Input | Pin/record | Use and terms observed |
|---|---|---|
| RadioFoundry | `dfbaf20c400c73e2ccf6edb80af7d1d255bee418`, generated manifests | MIT originals, notices preserved; separately modified GPL-compatible laboratory derivative, not an MIT-only upstream contribution |
| FreeRTOS-Kernel | `3a22924e0a9ddbbc8b0758881c33b3422a5cc20d` | MIT submodule used by the RadioFoundry laboratory image |
| newlib | 4.5.0.20241231, SHA256 in `tools/bootstrap_newlib.sh` | Genuine target libc/libgloss, per-file licenses in upstream COPYING.NEWLIB; not a fabricated sysroot |
| nostar/imbe_vocoder | `sources.lock.json`, `experiments/codec/results/sources.json` | Actual full-rate fixed-point codec; GPL-3.0-or-later per-file notices, upstream omitted COPYING; full license supplied locally |
| mbelib | same codec records | ISC, copyright notice in `LICENSES/mbelib-ISC.txt`; independent test oracle, not linked into embedded codec |
| OP25 | `docs/protocol-sources.json` | p25craft GPL-2.0-or-later adapted for CAI; selected GPL 3 distribution; GPL/LGPL helper/ezpwd headers are separate test oracles |
| MMDVM | protocol source record | GPL-2.0-or-later P25 and NXDN source; investigation only, no embedded dependency or target hardware substitution. The NXDN modem (`src/nxdn/nxdn_modem.c`) follows its filter structure (RRC 0.2 with SINC on transmit, ISINC on receive, `docs/nxdn-modem.md`); its coefficients are generated independently by `tools/gen_nxdn_tables.py` and no MMDVM code is copied. The transmitter sends the 24-bit preamble value MMDVM sends (`NXDNTX.cpp` NXDN_PREAMBLE, 0x5775FD). `tools/nxdn_validate.py` reads MMDVM's NXDN transmit filter coefficients from the local research copy at run time, on the host only, to shape a test waveform; nothing of it is built into the firmware or distributed |
| MMDVMHost | `6ffa6d354a2c185316201c27c831c766a25a5653`, `docs/protocol-sources.json` | GPL-2.0-or-later (G4KLX). Reference for the NXDN layer (LICH, SACCH, FACCH1, convolutional code, CRC, scrambler) and a host test oracle. Where `src/nxdn` follows its logic the file says so and keeps the notice; the combination is distributed under GPL-3.0-or-later |
| sdrtrunk | `9641d2ef59e40ba89b5453e4c4f052b6502124b5` (sparse: the P25 decoder), `docs/protocol-sources.json` | GPL-3.0-or-later (Dennis Sheirer). Reference for TSBK message field layouts (inbound requests, outbound responses and broadcasts, identifier updates) and Phase 2 MAC structures used by `src/p25t`; field positions are protocol facts, no source is copied or linked |
| dsd-fme | `34753963b8855c6c07b79250cb4de4f2c38a4613`, `docs/validation.md` | ISC (DSD) with GPL-2.0 files (dstar/descramble); built locally as an independent decoder for host tests only, never linked or distributed |
| RadioFoundry upstream changes | `integration/radiofoundry/patches/*.patch` | Patches to MIT files of the pinned upstream; the derivative is GPL-3.0-or-later as a whole and none of it is offered upstream |
| gzalo/dm32-uv | lock file | MIT repository; bootloader research lead |
| Roger Clark DM32 tools/helper | lock file | No repository license declared; fetched locally for audit and guarded binary patching; no original helper or script vendored into tracked source |
| baocoder | lock file | ISC main code with separate firmware-table provenance caveats; research reference only, no code/tables copied into this project's codec |
| Ghidra C-SKY candidate | lock file | No license file found; locally inspected only, unreliable startup decoding, no target dependency |
| Stock firmware/archive and board photograph | lock file/target record | No redistribution license asserted; originals remain ignored local research files |
| HR_C7000, Zetron and Tait manuals | source records | Copyrighted primary references, local copies only; no normative-standard redistribution claim |
| Historical ETSI TS 102361-1 v2.2.1 | User's S7 URL, web-read 2026-10-26 | Primary DMR modulation comparison; direct local download returned HTTP 403; not represented as the latest standard or a preserved local PDF |
| pyserial 3.5 | `requirements-hardware.txt`, PyPI version metadata | BSD; optional host serial diagnostics only, never an embedded or operational dependency |
| GNU binutils/GCC/libgcc and prerequisites | GNU source archives/build report | Retain upstream GPL/LGPL notices and applicable GCC Runtime Library Exception; no source terms replaced by this project's LICENSE |

The actual DM-32 OpenDM32 application source was not located publicly. An
experimental binary and parent OpenGD77 source for other processors do not
provide a buildable DM-32 source base or permission to redistribute a combined
stock/GPL application. Those distribution terms must be resolved before an
eventual combined firmware release. No firmware release was made here.

Locally generated system-voice audio remains in ignored development output.
Its origin and hashes are recorded; no redistribution permission for that voice
is asserted. Deterministic mathematical test signals and synthetic packet
fixtures are the portable source-controlled regression material.
