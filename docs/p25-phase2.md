# P25 Phase 2 (TDMA)

**Status:** built and validated above the physical layer (`src/p25p2`). There is no Phase 2 physical layer for the
DM-32, so the radio neither receives nor transmits Phase 2. A trunk channel ignores TDMA grants (it stays on the
control channel) and refuses them for its own calls (`P25T_SU_DENY_BAND`).

## Why there is no physical layer

Phase 2 needs these two waveforms:

| Direction | Modulation | Rate |
|---|---|---|
| Outbound (site to radio) | H-DQPSK | 6000 symbols/s |
| Inbound (radio to site) | H-CPM | 6000 symbols/s |

The HR_C7000 user guide (V2.7, local copy `research/modem/HR_C7000_Document_2.pdf`) was searched for every path the
CPU could use. Pages are the printed footers.

| Need | What the manual documents | Result |
|---|---|---|
| Demodulated or IF samples above 8 kHz | The baseband ADC feeds only the modem block (Fig. 2, p33). No register, RAM window or DMA exposes I/Q, IF or discriminator samples. The FM RX RAM runs at 8 kHz (p271). | No |
| CPU-written modulation above 8 kHz | DAC_CONTROL has no data field (p95). `rf_trans_mode` (p99, p281) selects the output format, not a source. The FM TX RAM runs at 8 kHz (p271). | No |
| A symbol rate other than 4800 | No baud or symbol-rate register exists. 4800 = `modem_clk` 9.8304 MHz / 2048 (derived from p56, p57, p63, p291). The only lever is the global APLL / `modem_clk_div_num`, which rescales the whole modem: FM clocks, 30 ms slot timers, `en_sample`. This is not a documented feature. | Not usable |
| H-DQPSK demodulation | The native demodulator is 4FSK with DMR burst framing around a 48-bit sync (p283–286, p296). It provides no raw symbol stream. | No |
| H-CPM pulse shaping on TX | No TX FIR or pulse-shape register exists. The DMR 4FSK pulse is fixed and undocumented. | No |

The only remaining routes need hardware changes (AK2401 serial I/Q pins, MCU ADC/DAC wired to the RF chain) or
untested operation outside the chip's documented clock plan. None fits the project's rules (an unmodified radio,
nothing transmitted untested). The full notes, with the register-level detail, are kept with the research files.

One route was considered and not built: H-CPM on the FM TX RAM path at 8 kHz, as NXDN uses.
- The H-CPM frequency pulse may fit under 4 kHz, but the FM chain's filtering of it is unknown.
- The outbound H-DQPSK cannot be received through the FM discriminator at all, because it is a phase modulation.
- A radio that can transmit but not receive Phase 2 cannot take part in a call.

## What is built (`src/p25p2`)

Coding, framing and the voice channel's MAC follow OP25 (`p25p2_tdma.cc`, `p25p2_vf.cc`, `apps/tdma/*.py`) and
dsd-fme (`p25p2_frame.c`, `p25p2_xcch.c`, `p25p2_vpdu.c`). The code is new.

| Part | Detail |
|---|---|
| Superframe | 12 bursts × 180 dibits (360 ms). Bursts 0-9 alternate logical channels 0 and 1; burst 10 carries the SACCH of channel 1 and burst 11 that of channel 0. |
| ISCH | 20 dibits before each burst. S-ISCH sync `0x575D57F7FF` before bursts 2, 3, 6, 7, 10, 11. I-ISCH is a (40,9) code with channel, location, free and count fields; its generator comes from OP25 `isch.py` (`tools/gen_p25p2_tables.py`). Decoding corrects up to 7 bit errors. |
| DUID | (8,4) code at payload dibits 0, 37, 122, 159. Never scrambled. Corrects 1 bit error. |
| Scrambling | 44-bit LFSR seeded with WACN (20 bits), System ID (12) and NAC (12), 4320 bits per superframe. Payload dibit p of burst b uses sequence dibit 180·b + 10 + p. |
| SACCH | RS(63,35) over GF(64) (x^6+x+1, roots α^1..α^28), shortened by 5 and punctured by 6: 52 hex bits carry a 168-bit MAC PDU and CRC-12. |
| FACCH | The same code shortened by 9 and punctured by 9: 45 hex bits carry a 144-bit PDU and CRC-12. |
| CRC-12 | x^12+x^11+x^7+x^4+x^2+x+1, inverted. |
| 4V / 2V | Four (two) 72-bit voice code words. ESS-B is spread over the four 4V bursts; the 2V carries ESS-A (RS(44,16,29)): algorithm ID, key ID, MI. |
| Voice code words | AMBE+2 3600x2450 frames in DMR's exact 72-bit air form (same Golay codes, whitening and interleave). The generator stops if OP25's interleave ever differs from DMR's. A Phase 2 radio could pass frames to the DMR vocoder unchanged. |
| MAC PDUs | MAC_PTT (MI, algorithm, key, source, group), MAC_END_PTT (color code, source, group), MAC_ACTIVE / HANGTIME / IDLE with Group Voice Channel User (abbreviated, opcode 0x01), Null fill. |
| Sender (`p2_tx`) | One logical channel: MAC_PTT ×2 on FACCH, then 4V 4V 4V 4V 2V (18 frames per 360 ms) with MAC_ACTIVE on SACCH, then MAC_END_PTT ×2, MAC_HANGTIME, MAC_IDLE. Underruns are padded with the AMBE+2 silence frame. |
| Receiver (`p2_rx`) | Finds S-ISCH (up to 4 bit errors), takes the burst number from the I-ISCH, then descrambles, decodes and hands out MAC PDUs and code words with the encrypted flag. |

Encryption: MAC_PTT's algorithm ID, the ESS, or the encrypted bit of the voice channel user message marks a call
encrypted. The receiver reports it and the radio would mute it. Nothing is decrypted, and there is no key input.

### Inbound bursts (radio to site): an assumption

OP25, dsd-fme and sdrtrunk decode outbound only, and TIA-102.BBAC is not available here. The subscriber's inbound
bursts are therefore built with:
- The outbound logical structure: DUID, 4V/2V/FACCH/SACCH payload, scrambling.
- 20 zero dibits in place of the inbound physical framing (guard, ramp, sync).
- Timing in the slot's positions of the received superframe, with no offset.

The site simulator decodes them at those positions. Check the inbound burst structure and timing against
TIA-102.BBAC before any Phase 2 PHY uses this code.

## Validation (`tools/p25p2_validate.py`, results in `results/p25p2-validation.json`)

`tests/p25p2/p2_air.c` writes a voice channel's outbound stream as dibits:
- WACN BEE00, System ID 3A5, NAC 293.
- Three idle superframes.
- A call on each logical channel: TG 1234 from 0xABCDE, and TG 4321 from 777.
- 60 voice frames per call, built from known parameter bits with the radio's DMR FEC encoder.
- The ends of the calls, then idle superframes.

| Decoder | Checked | Result |
|---|---|---|
| dsd-fme `-f2 -X BEE003A5293` | Sync, ISCH, DUID, descrambling, RS + CRC-12 of SACCH/FACCH | Every PDU accepted |
| | MAC_PTT ×2 per channel with TG/SRC | Matches |
| | MAC_ACTIVE with Group Voice Channel User on the right VCH | Matches |
| | MAC_END_PTT ×2 per channel (CRC; dsd-fme prints its stored call, which matches) | Matches |
| | ESS-B algorithm 0x80 | Matches |
| | 62 AMBE frames per channel (60 + 2 silence pads) | 62/62 per channel identical to what went in, 0 Golay corrections |
| OP25 `apps/tdma` (Python) | Its own LFSR, ISCH and DUID tables, and burst types | 72 S-ISCH, 72 I-ISCH, 0 unknown; 4V/2V/SACCH/FACCH all typed |
| | Voice code words (deinterleave, Golay) | 62/62 per channel identical |

The OP25 Python modules are Python 2. The validator replaces the one incompatible function at run time
(`rs.gly23127GetSyn`, `/` to `//`); nothing else is changed.

Host tests:
- `tests/p25p2/test_p2.c`:
  - RS with errors and erasures up to the bound.
  - SACCH/FACCH with hex bit errors.
  - ESS with 14 symbol errors.
  - ISCH with 7 bit errors and DUID with 1.
  - Scrambled 4V/2V round trips.
  - A call on each channel through the stream receiver, starting in noise.
- `tests/p25t/test_su.c`:
  - Phase 2 voice channels in the simulated site.
  - Units follow a TDMA grant and make a group call in their slot, and the other unit hears it.
  - A unit without a Phase 2 transmitter refuses the grant.

## Build

`CONFIG_P25_PHASE2` (default on, needs `CONFIG_P25_TRUNK`) adds `core/p25p2` to the image. Nothing on the radio
calls it, because there is no physical layer, so the linker drops it. The image is the same size with the switch on
or off (466,576 / 466,588 bytes, alignment only).
