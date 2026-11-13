<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Independent validation

Our protocol code is checked against decoders that share no code with it. They run on the host only, as test
oracles; nothing from them is linked into the firmware. No radio, serial port or RF is involved.

## Decoders

| Decoder | Pin | Used for | License |
|---|---|---|---|
| dsd-fme | `34753963b8855c6c07b79250cb4de4f2c38a4613` (lwvmobile/dsd-fme) | P25 Phase 1, NXDN48 and P25 Phase 2 decoding of our streams | ISC (DSD) with GPL-2.0 files; not distributed |
| mbelib | `9a04ed5c78176a9965f3d43f7aa1b1f5330e771f` (szechyjs/mbelib) | dsd-fme's vocoder library | ISC |
| OP25 | `3f44d43eb449bb4753247ce848bb42a403823dea` (boatbod/op25) | CAI frame oracle (`tests/protocol`), trunking message reference | GPL-2.0-or-later / GPL-3.0-or-later |
| MMDVMHost | `6ffa6d354a2c185316201c27c831c766a25a5653` (g4klx/MMDVMHost) | NXDN layer reference and oracle | GPL-2.0-or-later |

Build them (Ubuntu 24.04 needs `libsndfile1-dev libncurses-dev libpulse-dev cmake`):

```sh
sh tools/build_validators.sh
```

## P25 Phase 1 transmit chain

`tests/validation/p25_tx_air.c` runs the firmware's own transmit chain on the host: synthetic speech, the
fixed-point IMBE encoder, CAI framing (HDU, LDU1/LDU2, TDU) and `p25_phy_tx_block`, which packs the 36-byte blocks
the native modem is handed. `tools/p25_validate.py` turns those dibits into a 48 kHz discriminator recording and
decodes it with dsd-fme. It compares every frame type, the NAC, talkgroup and source, and every IMBE frame dsd-fme
recovers (with its own deinterleave and FEC) against the parameters our encoder produced.

```sh
python3 tools/p25_validate.py --check --seconds 6
python3 tools/p25_validate.py --waveform c4fm --snr-db 20
python3 -m unittest tests.test_p25_validation -v
```

Results on 2026-11-13 (6 s call, NAC 293, talkgroup 10101, source 1234567; `results/p25-validation.json`):

| Waveform | Frames heard / sent | IMBE frames equal to ours | Frames with FEC corrections |
|---|---:|---:|---:|
| Held symbol levels (bit-exact) | 36 / 36 | 300 / 300 | 0 |
| Held symbol levels, 12 dB SNR | 36 / 36 | 300 / 300 | 0 |
| Raised-cosine pulse | 36 / 36 | 296 / 300 | 17 |
| TIA H(f)P(f) C4FM filter | 35 / 36 | 287 / 300 | 75 |
| TIA C4FM filter, 20 dB SNR | 36 / 36 | 273 / 300 | 112 |

The held-level runs establish the bitstream: dsd-fme hears every frame, our identifiers and every vocoder bit, with
no corrections. dsd-fme samples symbol centers, so on the shaped synthetic waveforms its FEC has work to do even
without noise; that characterizes its demodulator, not our frames. The radio's on-air waveform is made by the
HR_C7000's modulator and is verified only with an RF capture (`docs/hil-checklist.md`).

Two details of the comparison:

- IMBE bit 87 (the low bit of u7) carries the alternating pattern `src/p25/p25.c` writes; the 87 vocoder bits are
  compared and the pattern is checked on its own.
- dsd-fme counts one "header error" when the first frame's NAC differs from its initial NAC 0. It is bookkeeping,
  not a bit error.

## Receive side

The receive path below the modem is the HR_C7000's demodulator and DMR deframer (docs/modem.md). On the host, the
receiver's framing is checked against frames built by OP25's `p25craft` (`tests/protocol`, `tests/test_voice.c`),
and live captures from the radio were decoded by OP25 (PROGRESS.md, 2026-11-05).
