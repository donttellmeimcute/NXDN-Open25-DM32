# Resources of the multi-mode image

One image carries every mode: FM, DMR, P25 conventional, NXDN48 and P25 trunking (control channel, FDMA voice).
The Phase 2 layer is built too. Built with `tools/build_radiofoundry.py`, RadioFoundry pinned at `dfbaf20c`, on
2026-11-13. Not measured on a radio: the figures are the linker's, the static stack check's, and host estimates
calibrated on one on-radio measurement.

## Flash and RAM

The CK803S has 880 KB of flash for the application and 211,456 bytes of RAM. The RAM figure includes the RTOS
heap: 40.5 KB with the P25 worker.

| Image | Switches | Flash | RAM | Free RAM |
|---|---|---:|---:|---:|
| **All modes, receive-only trunking (default)** | P25, NXDN, TRUNK, PHASE2 | 466,576 (51.8 %) | 194,432 | 17,024 |
| **All modes, trunked transmit** | + TRUNK_TX (`--trunk-tx`) | 468,004 (51.9 %) | 195,184 | 16,272 |
| All modes, lean (no bench replay or self-test) | `--lean --trunk-tx` | 466,268 | 192,768 | 18,688 |
| No trunking | `--no-trunk` | 459,588 | 193,176 | 18,280 |
| No trunking, no NXDN | `--no-trunk --no-nxdn` | 445,784 | 190,432 | 21,024 |
| P25 off | `--disable` | 409,064 | 163,352 | 48,104 |

Each mode's cost:

| Mode | Flash | RAM |
|---|---:|---:|
| NXDN48 | 13.8 KB | 2.7 KB |
| P25 trunking, receive | 7.0 KB | 1.3 KB |
| P25 trunking, transmit | 1.4 KB | 0.75 KB |
| Phase 2 | 0 | 0 |

Phase 2 costs nothing because nothing on the radio calls it (no physical layer), so the linker drops it.

Every switch builds on and off with zero warnings.

## Stack (static worst case, all modes with trunked transmit)

| Task | Stack | Deepest | Free |
|---|---:|---:|---:|
| p25-lab (P25 codec, frames, trunking tap) | 8,192 | 5,680 | 2,316 |
| audio (AMBE+2 for DMR and NXDN) | 6,144 | 4,324 | 1,624 |
| radio (radio core, NXDN modem, trunking logic) | 3,072 | 2,140 | 736 |
| ui | 2,016 | 1,548 | 272 |
| start | 2,560 | 1,632 | 732 |
| idle | 576 | 244 | 136 |
| native modem interrupts (separate 2 KB IRQ stack) | 2,048 | 184 | 1,668 |

Every task fits; the least margin is the idle task's 136 bytes, unchanged from the base firmware. The trunking tap's
path in the P25 worker is about 0.5 KB deep, below the encoder's path. The trunking logic runs in the radio task
below its deepest path.

## Real time

`tools/rt_budget.py` (`results/rt-budget.json`) runs each mode's work on the host for one deadline unit and counts
the instructions inside it (callgrind). It converts those to CK803S time at 168 MHz with the one CPU measurement
made on the radio for the same code: the IMBE encoder averaged 13.0 ms per 20 ms frame (0.843 CK803S cycles per
host instruction). As a cross-check, the IMBE decoder comes out at 11.5 ms on average; the radio measured a 12.2 ms
peak.

| Mode | Work per period | Estimate | CPU |
|---|---|---:|---:|
| P25 TX | 9 IMBE encodes + LDU coding per 180 ms | 117.5 ms | 65 % (on the radio: 13.3 ms average, 18.6 ms peak per 20 ms frame) |
| P25 RX | LDU framing and decoding + 9 IMBE decodes per 180 ms | 105.9 ms | 59 % |
| P25 trunk, control channel | a 3-block TSDU (3 Viterbi decodes) + framer + logic per 75 ms | 1.9 ms | 2.5 % |
| P25 trunk, following a call | P25 RX + the control channel tap + logic | 107.1 ms | 59.5 % |
| P25 trunk, talking | P25 TX + logic | 117.5 ms | 65 % |
| NXDN RX | demodulator with every compensation filter, call layer, per 80 ms frame | 0.8 ms | 1.0 % + the AMBE+2 decoder |
| NXDN TX | framing, modulation, the timer interrupt's pulls, per 80 ms frame | 0.6 ms | 0.7 % + the AMBE+2 encoder |
| Phase 2 receive (reference; no PHY) | one burst per 30 ms | 0.13 ms | 0.4 % |

Not covered:
- The stock AMBE+2 vocoder, which DMR and NXDN share. NXDN runs it at DMR's rate, which the firmware already
  sustains.
- Interrupt entry and the RTOS. The on-radio P25 encoder figures include interrupt preemption, because interrupt
  time is charged to the interrupted task.
- The NXDN timer interrupt runs every 2 ms and pulls 16 samples; its work is in the NXDN TX figure.
- The native modem's slot interrupts run every 30 ms in P25.

Modes never run at the same time: a channel is in one mode. The heaviest is P25 TX, measured on the radio at 93 % of
a frame at its peak, with buffering absorbing it. Trunking adds at most 2.5 % to any P25 mode. NXDN adds about 1 %
to DMR's vocoder load.

## Does it fit?

Yes:
- Flash: 52 % used.
- RAM: 16 KB free with every switch on.
- Every stack fits.
- Each mode's real-time load is below one CPU.

No switch needs to be turned off. If the margin has to grow, in this order:

| Order | Change | Gain | Loses |
|---:|---|---|---|
| 1 | `--lean` | 1.7 KB flash, 2.4 KB RAM | The on-radio replay and self-test |
| 2 | `--no-phase2` | No flash or RAM today | Nothing on the radio |
| 3 | Trunked transmit off (the default) | 1.4 KB flash, 0.75 KB RAM | Trunked transmit |
| 4 | `--no-nxdn` | 13.8 KB flash, 2.7 KB RAM | NXDN |

The figures to confirm on the radio are in [hil-checklist.md](hil-checklist.md) step 7.
