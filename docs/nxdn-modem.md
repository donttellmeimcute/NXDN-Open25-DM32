<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# NXDN48 modem: simulation and physical route

NXDN48 is the 6.25 kHz NXDN mode: 4800 bit/s, 2400 symbols/s 4-level FSK. The HR_C7000's native 4FSK mode runs
at a fixed 4800 symbols/s (the DMR and P25 rate), so NXDN48 needs another route. This page records the simulation
that chose it. Everything here is host simulation: nothing was measured on a radio. The hardware steps are listed
in the last section.

Run it:

```
python3 tools/test_nxdn.py                       # the checks (part of the NXDN host tests)
build/nxdn-host/test_nxdn_modem --sweep          # the tables below (build it with tools/test_nxdn.py first)
```

## Air interface

| Item | Value | Source |
|---|---|---|
| Symbol rate | 2400 symbols/s | NXDN Common Air Interface (TS 1-A); MMDVM `NXDNDefines.h` (10 samples a symbol at 24 kHz) |
| Deviation | +3 = +1050 Hz, +1 = +350 Hz, -1 = -350 Hz, -3 = -1050 Hz | TS 1-A; MMDVM `NXDNTX.cpp` levels 735 / 245 (3:1) |
| Dibit to symbol | 01 = +3, 00 = +1, 10 = -1, 11 = -3 | MMDVM `NXDNTX.cpp`, MMDVMHost |
| Frame | 80 ms = 192 symbols = 384 bits | MMDVM / MMDVMHost `NXDN_FRAME_LENGTH_BITS` |
| Frame layout | FSW 20 bits + LICH 16 + SACCH 60 + four 72-bit voice fields (288) | `src/nxdn/nxdn.h`, checked bit for bit against MMDVMHost (`tests/nxdn/test_nxdn_mmdvm.cc`) |
| FSW | 0xCDF59: -3 +1 -3 +3 -3 -3 +3 +3 -1 +3 | MMDVM `NXDN_FSW_SYMBOLS_VALUES` |
| Pulse | root raised cosine, roll-off 0.2, with a sin(x)/x shaping filter on transmit and x/sin(x) on receive | MMDVM `NXDNTX.cpp` (RRC 0.2 and SINC filters), `IO.cpp` (RRC 0.2 and ISINC) |

The 20 + 16 + 60 + 4 x 72 = 384 bits confirm the 80 ms frame at 2400 symbols/s.

## The modem (`src/nxdn/nxdn_modem.c`)

Integer only (the CK803S has no FPU), no allocation, no RTOS. Both directions run at 8 kHz, the rate of the radio's
FM audio paths; 8000 / 2400 = 10/3 samples a symbol.

| Part | Implementation | Cost |
|---|---|---|
| Transmit filter | RRC 0.2 x sin(x)/x, 9 symbols long, 10-phase polyphase table in Q14 (the sample-to-symbol phase repeats every 10 samples) | 9 multiply-adds a sample: 72 k/s |
| Receive filter | RRC 0.2 x x/sin(x), 31 taps at 64 fractional phases, evaluated only at the instants it needs | 2 x 31 multiply-adds a symbol: 149 k/s |
| Timing | Gardner on the symbol and mid-symbol outputs, normalized by the level, on the symbol pairs that cross the offset only; fast (1/16) for 32 symbols, then slow (1/256) | a few operations a symbol |
| Offset and level | Seeded from the first four symbols; during acquisition the offset follows the mean of the last four symbols (exact on either preamble below) and the outer level the symbols above half of it; then decision directed: the offset follows each symbol's distance from its ideal level and the outer level follows the +-3 symbols | a few operations a symbol |
| Finding a carrier | While the frame layer hunts, the demodulator acquires again whenever, after an acquisition, the symbols sit badly on their levels (mean distance over 1/7 of the outer level; noise gives 1/6, a signal at 6 dB C/N 1/9) | a few operations a symbol |
| Audio path compensation | optional inverses of a first-order high-pass (receive and transmit) and of a de-emphasis (receive), below | 2-4 operations a sample |

Why the timing detector skips pairs that don't cross: between two symbols of the same sign the mid-symbol sample sits
near a peak, and on average its term cancels the crossings' terms. MMDVM's transmitter starts with 0x5F bytes
(+3 +3 -3 -3), a tone at a quarter of the symbol rate, on which plain Gardner has no timing information at all for
any phase; with the gate it locks as fast as on +3 / -3 dotting (the joining table below), and on random data the
gate removes self-noise.

The transmitter sends 36 symbols of +3 / -3 dotting and then 0x5775FD, the 24-bit preamble MMDVM sends before the
first frame (`nxdn_preamble_dibit`), then the header frame.

The frame layer (`src/nxdn/nxdn_call.c`) had two weaknesses the simulation found:

- **False sync in noise.** The receive audio is not squelched; a 20-bit word within two errors of the FSW turns up
  in about 5 % of 100 ms of noise. A sync the hunt finds is now tentative: it is dropped when its LICH is not one a
  frame can carry, or when a cleaner FSW appears before the next frame's FSW confirms it. Before, one call in 40 lost
  its header and the first superframe this way, even without noise.
- **Late entry.** The VCALL repeats unchanged in each SACCH superframe, so its four parts are now taken from any
  four frames, not one aligned superframe: a receiver that joins mid-call plays after four frames instead of up to
  seven.

**No squelch edge.** The radio's receive audio is unsquelched and the radio has no edge to restart the demodulator
on: between calls it runs on noise (or silence), and its levels and timing fit that. While the frame layer hunts for
sync (`nxdn_link` sets `onset_reacquire`), the demodulator measures how well the symbols sit on their four levels and
acquires again whenever, after an acquisition, they don't: a transmission that starts is met by an acquisition inside
its preamble. Two simpler triggers were tried and dropped: a periodic restart (every 40 symbols) lands late in a
preamble often enough to lose 6-9 % of the voice at call starts, and a level jump against the outer level fires at the
preamble-to-data step, where the mean level falls to 2/3 of the outer one. The table "Finding a call" below measures
the one kept. `nxdn_demod_reacquire` also restarts by hand (the host simulation's squelch).

Tables are generated by `tools/gen_nxdn_tables.py` (numerical inverse transform of the shaped spectra).

Modulator straight into demodulator decodes every frame of a call bit-exact (`test_nxdn_modem`).

### High-pass compensation

A high-pass in the audio path makes the 4FSK baseline wander: the low-frequency content of random data is not
small. Uncompensated, a 100 Hz first-order high-pass loses 85-100 % of the voice frames at any C/N (tables below).
`nxdn_hpf_inverse` undoes a first-order high-pass of known corner exactly, with a leak so a DC offset can't
integrate: receive leak 1.2 Hz (`NXDN_RX_LEAK`), transmit leak 10 Hz (`NXDN_TX_LEAK`). On transmit the inverse
boosts low frequencies before the radio's high-pass removes them again, so the samples need headroom: the peak is
1.7 x the +-3 level without compensation (the RRC overshoot), 4.6 x with a 100 Hz corner and 11.5 x with 300 Hz.
The corner must be measured on the radio: compensating a 100 Hz high-pass as 80 Hz costs about 2 dB at the knee.

`nxdn_deemph_inverse` undoes a receiver's de-emphasis (a one-pole low-pass, 750 us in FM voice): a two-tap filter
with the bilinear-matched pole. It lifts the noise above the corner too; the matched filter removes what lies beyond
the symbol band.

## Channel model (`tests/nxdn/nxdn_channel.c`)

| Stage | Model |
|---|---|
| Transmit audio path | the modulator's 8 kHz samples, optional first-order high-pass, ideal 3.6 kHz interpolation to 48 kHz (the chip's modulator) |
| FM | complex carrier at 48 kHz, deviation x `dev_scale`, carrier offset |
| Noise | complex white Gaussian, C/N in 12.5 kHz |
| Receiver | 12.5 kHz channel filter, ideal discriminator, 3.4 kHz audio low-pass, decimation to 8 kHz |
| Receive audio path | optional de-emphasis (one pole on the discriminator, before the audio low-pass), first-order high-pass, gain (8 LSB per Hz), clipping |
| Idle | 100 ms of noise without carrier before every call; the squelch opens 5 ms into the carrier and the demodulator reacquires |

The call: the 48-symbol preamble, the header frame (FACCH1 VCALL), 50 voice frames of random AMBE+2 parameters,
the release frame (FACCH1 TX_REL). The receiver is the full chain: demodulator, frame layer (`nxdn_rx`), AMBE+2
FEC decode.

## Results

Cells: pre-FEC bit error rate / voice frame error rate. A voice frame counts as lost unless all four of its AMBE+2
frames decode to exactly the parameters sent, after the AMBE FEC. 50 voice frames per cell. C/N in 12.5 kHz.

The second route sends NXDN through the chip's native 4FSK mode at 4800 symbols/s with each symbol twice: the chip
shapes the stream with its own DMR pulse (modeled as RRC 0.2 at 4800 symbols/s, 350 Hz a level unit). The chip's
transmit path is not in the audio chain, so the transmit high-pass rows don't apply (n/a).

### TX: software modulation at 8 kHz

| Impairment | 6 dB | 8 dB | 10 dB | 12 dB | 14 dB | 16 dB | 20 dB | 30 dB |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| clean audio path | 0.0148 / 0.28 | 0.0015 / 0.04 | 0.0000 / 0.00 | 0.0000 / 0.00 | 0.0000 / 0.00 | 0.0000 / 0.00 | 0.0000 / 0.00 | 0.0000 / 0.00 |
| offset +500 Hz | 0.0152 / 0.27 | 0.0012 / 0.01 | 0.0001 / 0.00 | 0.0000 / 0.00 | 0.0000 / 0.00 | 0.0000 / 0.00 | 0.0000 / 0.00 | 0.0000 / 0.00 |
| deviation +20% | 0.0117 / 0.21 | 0.0007 / 0.02 | 0.0001 / 0.00 | 0.0000 / 0.00 | 0.0000 / 0.00 | 0.0000 / 0.00 | 0.0000 / 0.00 | 0.0000 / 0.00 |
| deviation -20% | 0.0286 / 0.53 | 0.0062 / 0.16 | 0.0007 / 0.01 | 0.0001 / 0.01 | 0.0000 / 0.00 | 0.0000 / 0.00 | 0.0000 / 0.00 | 0.0000 / 0.00 |
| RX high-pass 20 Hz | 0.0292 / 0.55 | 0.0120 / 0.23 | 0.0027 / 0.07 | 0.0019 / 0.06 | 0.0014 / 0.05 | 0.0019 / 0.04 | 0.0011 / 0.01 | 0.0013 / 0.04 |
| RX high-pass 100 Hz | 0.1042 / 1.00 | 0.0864 / 0.91 | 0.0809 / 0.89 | 0.0727 / 0.82 | 0.0701 / 0.80 | 0.0728 / 0.82 | 0.0697 / 0.85 | 0.0674 / 0.75 |
| RX high-pass 300 Hz | 0.1922 / 1.00 | 0.1794 / 1.00 | 0.1767 / 1.00 | 0.1674 / 1.00 | 0.1707 / 1.00 | 0.1641 / 1.00 | 0.1672 / 1.00 | 0.1646 / 1.00 |
| TX high-pass 100 Hz | 0.1088 / 0.93 | 0.0917 / 0.92 | 0.0816 / 0.91 | 0.0754 / 0.86 | 0.0760 / 0.83 | 0.0674 / 0.85 | 0.0713 / 0.83 | 0.0691 / 0.82 |
| RX high-pass 100 Hz, compensated | 0.0154 / 0.24 | 0.0016 / 0.03 | 0.0000 / 0.00 | 0.0000 / 0.00 | 0.0000 / 0.00 | 0.0000 / 0.00 | 0.0000 / 0.00 | 0.0000 / 0.00 |
| RX high-pass 300 Hz, compensated | 0.0300 / 0.40 | 0.0023 / 0.06 | 0.0000 / 0.00 | 0.0000 / 0.00 | 0.0000 / 0.00 | 0.0000 / 0.00 | 0.0000 / 0.00 | 0.0000 / 0.00 |
| RX high-pass 100 Hz, compensated as 80 Hz | 0.0206 / 0.40 | 0.0029 / 0.06 | 0.0002 / 0.01 | 0.0000 / 0.00 | 0.0000 / 0.00 | 0.0000 / 0.00 | 0.0000 / 0.00 | 0.0000 / 0.00 |
| TX high-pass 100 Hz, pre-compensated | 0.0220 / 0.42 | 0.0033 / 0.09 | 0.0011 / 0.04 | 0.0002 / 0.01 | 0.0000 / 0.00 | 0.0000 / 0.00 | 0.0000 / 0.00 | 0.0000 / 0.00 |
| TX high-pass 300 Hz, pre-compensated | 0.0190 / 0.39 | 0.0045 / 0.10 | 0.0007 / 0.04 | 0.0001 / 0.00 | 0.0000 / 0.00 | 0.0001 / 0.00 | 0.0000 / 0.00 | 0.0000 / 0.00 |
| TX and RX high-pass 300 Hz, both compensated | 0.0251 / 0.46 | 0.0053 / 0.14 | 0.0008 / 0.01 | 0.0003 / 0.01 | 0.0003 / 0.01 | 0.0001 / 0.00 | 0.0000 / 0.00 | 0.0000 / 0.00 |
| RX clipped at 80% of +-1050 Hz | 0.0425 / 0.77 | 0.0170 / 0.43 | 0.0064 / 0.19 | 0.0029 / 0.10 | 0.0009 / 0.04 | 0.0005 / 0.01 | 0.0002 / 0.01 | 0.0000 / 0.00 |
| RX de-emphasis 750 us | 0.2321 / 1.00 | 0.2253 / 1.00 | 0.2252 / 1.00 | 0.2264 / 1.00 | 0.2265 / 1.00 | 0.2230 / 1.00 | 0.2234 / 1.00 | 0.2249 / 1.00 |
| RX de-emphasis 750 us, compensated | 0.0158 / 0.28 | 0.0011 / 0.04 | 0.0004 / 0.01 | 0.0000 / 0.00 | 0.0000 / 0.00 | 0.0000 / 0.00 | 0.0000 / 0.00 | 0.0000 / 0.00 |
| RX de-emphasis 750 us and high-pass 300 Hz, both compensated | 0.0185 / 0.32 | 0.0009 / 0.02 | 0.0000 / 0.00 | 0.0000 / 0.00 | 0.0000 / 0.00 | 0.0000 / 0.00 | 0.0000 / 0.00 | 0.0000 / 0.00 |
| all: +300 Hz, -10% dev, RX HP 50 Hz (comp.), clip 90% | 0.0815 / 0.81 | 0.0199 / 0.40 | 0.0069 / 0.15 | 0.0034 / 0.09 | 0.0023 / 0.06 | 0.0021 / 0.06 | 0.0018 / 0.06 | 0.0012 / 0.06 |

### TX: native 4800-symbol/s physical mode, each symbol twice

| Impairment | 6 dB | 8 dB | 10 dB | 12 dB | 14 dB | 16 dB | 20 dB | 30 dB |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| clean audio path | 0.0352 / 0.64 | 0.0144 / 0.26 | 0.0080 / 0.13 | 0.0054 / 0.08 | 0.0039 / 0.03 | 0.0030 / 0.04 | 0.0037 / 0.04 | 0.0031 / 0.01 |
| offset +500 Hz | 0.0337 / 0.58 | 0.0132 / 0.26 | 0.0076 / 0.17 | 0.0046 / 0.07 | 0.0039 / 0.06 | 0.0032 / 0.02 | 0.0035 / 0.04 | 0.0031 / 0.02 |
| deviation +20% | 0.0227 / 0.44 | 0.0099 / 0.20 | 0.0065 / 0.12 | 0.0037 / 0.05 | 0.0040 / 0.05 | 0.0034 / 0.03 | 0.0031 / 0.01 | 0.0029 / 0.01 |
| deviation -20% | 0.0451 / 0.72 | 0.0241 / 0.49 | 0.0124 / 0.26 | 0.0079 / 0.14 | 0.0050 / 0.08 | 0.0039 / 0.06 | 0.0038 / 0.06 | 0.0031 / 0.03 |
| RX high-pass 20 Hz | 0.0530 / 0.80 | 0.0340 / 0.56 | 0.0204 / 0.40 | 0.0205 / 0.40 | 0.0168 / 0.27 | 0.0157 / 0.27 | 0.0148 / 0.29 | 0.0142 / 0.21 |
| RX high-pass 100 Hz | 0.1218 / 1.00 | 0.1106 / 0.95 | 0.1098 / 1.00 | 0.0986 / 0.94 | 0.1012 / 1.00 | 0.0983 / 1.00 | 0.0981 / 1.00 | 0.0950 / 1.00 |
| RX high-pass 300 Hz | 0.2112 / 1.00 | 0.1964 / 1.00 | 0.1969 / 1.00 | 0.1919 / 1.00 | 0.1946 / 1.00 | 0.1920 / 1.00 | 0.1936 / 1.00 | 0.1966 / 1.00 |
| TX high-pass 100 Hz | n/a | n/a | n/a | n/a | n/a | n/a | n/a | n/a |
| RX high-pass 100 Hz, compensated | 0.0355 / 0.63 | 0.0131 / 0.30 | 0.0081 / 0.16 | 0.0054 / 0.07 | 0.0043 / 0.06 | 0.0036 / 0.06 | 0.0033 / 0.03 | 0.0031 / 0.01 |
| RX high-pass 300 Hz, compensated | 0.0400 / 0.66 | 0.0151 / 0.33 | 0.0093 / 0.17 | 0.0056 / 0.13 | 0.0048 / 0.09 | 0.0039 / 0.06 | 0.0034 / 0.03 | 0.0037 / 0.03 |
| RX high-pass 100 Hz, compensated as 80 Hz | 0.0415 / 0.68 | 0.0210 / 0.40 | 0.0128 / 0.30 | 0.0106 / 0.19 | 0.0097 / 0.19 | 0.0072 / 0.15 | 0.0061 / 0.11 | 0.0049 / 0.07 |
| TX high-pass 100 Hz, pre-compensated | n/a | n/a | n/a | n/a | n/a | n/a | n/a | n/a |
| TX high-pass 300 Hz, pre-compensated | n/a | n/a | n/a | n/a | n/a | n/a | n/a | n/a |
| TX and RX high-pass 300 Hz, both compensated | n/a | n/a | n/a | n/a | n/a | n/a | n/a | n/a |
| RX clipped at 80% of +-1050 Hz | 0.0633 / 0.88 | 0.0344 / 0.68 | 0.0207 / 0.52 | 0.0132 / 0.37 | 0.0086 / 0.26 | 0.0063 / 0.16 | 0.0037 / 0.11 | 0.0022 / 0.07 |
| RX de-emphasis 750 us | 0.2216 / 1.00 | 0.2104 / 1.00 | 0.2131 / 1.00 | 0.2088 / 1.00 | 0.2100 / 1.00 | 0.2096 / 1.00 | 0.2099 / 1.00 | 0.2096 / 1.00 |
| RX de-emphasis 750 us, compensated | 0.0287 / 0.53 | 0.0111 / 0.21 | 0.0055 / 0.09 | 0.0040 / 0.07 | 0.0026 / 0.03 | 0.0024 / 0.01 | 0.0024 / 0.01 | 0.0029 / 0.02 |
| RX de-emphasis 750 us and high-pass 300 Hz, both compensated | 0.0308 / 0.59 | 0.0103 / 0.23 | 0.0062 / 0.10 | 0.0036 / 0.04 | 0.0031 / 0.02 | 0.0026 / 0.02 | 0.0024 / 0.00 | 0.0031 / 0.00 |
| all: +300 Hz, -10% dev, RX HP 50 Hz (comp.), clip 90% | 0.0953 / 0.90 | 0.0488 / 0.71 | 0.0254 / 0.46 | 0.0170 / 0.37 | 0.0116 / 0.26 | 0.0088 / 0.23 | 0.0066 / 0.17 | 0.0048 / 0.15 |

### Joining a call (software route; voice frames lost at the start, mean of 40 calls)

| Start | 8 dB | 10 dB | 14 dB | 20 dB |
|---|---:|---:|---:|---:|
| our preamble (+3 / -3 dotting, 0x5775FD) | 0.68 | 0.01 | 0.00 | 0.00 |
| MMDVM's preamble (0x5F bytes, 0x5775FD) | 0.66 | 0.01 | 0.00 | 0.00 |
| squelch opens 2.5 frames into the call | 4.64 | 4.26 | 4.17 | 4.12 |

### Finding a call in the receiver's unsquelched audio (nxdn_link; 40 calls of 10 voice frames each, +150 Hz)

| Before the carrier | Calls joined at their header | Voice frames lost per call |
|---|---:|---:|
| silence | 40 / 40 | 0.00 |
| noise, +-2500 Hz peak | 39 / 40 | 0.07 |
| noise, +-1200 Hz peak | 40 / 40 | 0.00 |
| noise, +-400 Hz peak | 40 / 40 | 0.00 |
| noise +-1200 Hz, joining 2.5 frames into the call | n/a | 2.15 (after the frames before it) |

### Transmit spectrum (no noise, 60 voice frames)

| TX route | 99% occupied bandwidth | power outside +-3.125 kHz | each adjacent 6.25 kHz channel |
|---|---:|---:|---:|
| software modulation at 8 kHz | 3803 Hz | 0.01% | -43.7 dB |
| native 4800-symbol/s mode, each symbol twice | 4354 Hz | 0.08% | -34.2 dB |

### Reading the tables

- The software route decodes every voice frame from 10 dB C/N up, with +500 Hz offset or +20 % deviation, and from
  12 dB with -20 % deviation. 6 dB is the knee (a third of the frames lost).
- The native route has an error floor: 0.3 % BER and a few percent of frames lost even at 30 dB. A DMR pulse at
  4800 symbols/s with each symbol repeated is not NXDN's 2400-symbol/s pulse; the receiver's matched filter sees
  intersymbol interference it can't remove. It also spreads 10 dB more power into the adjacent 6.25 kHz channels.
- Any high-pass in the audio path must be compensated: uncompensated, even 20 Hz leaves a frame loss floor, and
  100 Hz breaks reception. Compensated, 100 Hz and 300 Hz cost nothing measurable, in either direction.
- An FM voice de-emphasis (750 us) on the receive audio breaks reception (26 % BER); compensated, it costs nothing
  measurable, with a 300 Hz high-pass on top as well.
- Clipping the receive audio near the +-3 level costs 2-4 dB. The receive gain must leave headroom above
  1050 Hz plus the worst carrier offset: the last row, which clips at 90 % with a 300 Hz offset, keeps losing a few
  frames at any C/N.
- A call is joined from its first frame with either preamble from 10 dB up. Joining mid-call costs the frames until
  four SACCH parts are in (about four frames, 320 ms).
- Without a squelch edge (as on the radio) a call is still found at its header after silence and after noise; after
  noise twice as loud as the signal's outer level about 1 call in 40 misses its header and joins at its SACCH.

## The route chosen

| Direction | Route | Why |
|---|---|---|
| Transmit | software modulation at 8 kHz (`nxdn_mod`), samples through the prompt FIFO into the FM modulator during an FM transmission, the route APRS uses (RadioFoundry `modem-hrc7000.md` 8.4, `hrc_afsk.c`) | no error floor, 10 dB less adjacent-channel power than the native route, the pulse is NXDN's own; the native route would also need its framer to send a non-DMR stream |
| Receive | software demodulation at 8 kHz (`nxdn_demod`) of the modem's copy of its FM receive audio in RX RAM (RadioFoundry `modem-hrc7000.md` 8.5, `fm_rx_audio.c`) | the only path from the receiver to the CPU below the native 4800-symbol/s framer; unsquelched, so the demodulator works below the squelch |

In NXDN the radio is in FM mode (WORK_MODE 0x80) in both directions, with the 12.5 kHz receive filter (the
narrowest the chip has; the simulation uses 12.5 kHz). The FM voice processing must be off for the data: the band-pass
(FM_BANDWIDTH b6) and pre-emphasis (b4), and the speaker gets the decoded voice, not the discriminator.

On the DM-32 the FM receiver is the FD6818, a BK4819-class chip; the HR_C7000's AF input, and so its RX RAM copy, is
the FD6818's AF output (RadioFoundry `hardware-dm32.md`). RadioFoundry's FM entry writes the FD6818's AF filter
register 0x2B = 0xC000: in the BK4819's layout (RadioFoundry `bk4819.h`: RX bits 15 high-pass, 14 low-pass,
13 de-emphasis, set = bypassed) that bypasses the receive high-pass and low-pass and leaves the de-emphasis on. The
NXDN receiver writes 0xE000, the de-emphasis bypassed too. The FD6818's bit layout is unverified, so the
demodulator's de-emphasis compensation stays available: the tables show both outcomes.

The sample rate gives the modem room: 3.33 samples a symbol at 8 kHz. The cost is about 220 k multiply-adds a second
for both filters, which is small next to the AMBE+2 vocoder.

## What the radio must confirm

Measured on the hardware before NXDN is called working (`docs/hil-checklist.md`):

1. **Transmit scale.** The prompt FIFO's LSB per Hz of deviation in NXDN's FM settings. APRS measured about
   2750 LSB for 3 kHz peak at 25 kHz (`radio_dm32.c`); the NXDN worker starts from that and the value is calibrated
   with a service monitor on the 1200 Hz preamble (+-1050 Hz peak).
2. **Transmit low-frequency response.** Whether the prompt FIFO path has a high-pass or pre-emphasis with the voice
   processing off: send the +3 / -3 preamble and a long run of +3 symbols, measure the deviation's droop. The
   worker then compensates the measured corner, or nothing.
3. **Receive frequency response** of the RX RAM copy with the band-pass off and FD6818 0x2B = 0xE000: 50 Hz,
   300 Hz, 1 kHz and 2 kHz tones at the same deviation from a signal generator, their levels in the RX RAM samples.
   Flat: no compensation. A first-order roll-off above about 200 Hz: the de-emphasis is still on (750 us
   compensation). A droop at 50 Hz: a high-pass, compensate its corner. Then the receive gain against clipping.
4. **The prompt FIFO reaches the modulator on this board.** The RadioFoundry spec says ring play reaches the
   modulator on some boards and needs the codec mixer route on others; the mixer route passes the transmit limiter.
5. **Decoding by another receiver**: a commercial NXDN radio or dsd-fme from an SDR on the radio's transmission,
   and the radio receiving a commercial NXDN radio or an MMDVM modem.
