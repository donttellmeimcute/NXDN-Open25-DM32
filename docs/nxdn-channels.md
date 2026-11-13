<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# NXDN48 conventional channels

An NXDN channel is a channel mode like FM, DMR and P25. It is stored in the codeplug, edited in the web Codeplug
Editor, in `channels.csv` and on the radio, and the radio core applies the same transmit rules to it. NXDN48 is the
6.25 kHz mode: 2400-symbol/s 4FSK, AMBE+2 voice (the DMR vocoder's 49-bit frames with the same FEC). The HR_C7000's
native 4FSK mode runs at 4800 symbols/s only, so the radio receives and transmits NXDN through its FM paths with a
software modem at 8 kHz (`docs/nxdn-modem.md` has the simulation that chose this route).

Build switch: `CONFIG_NXDN=y` (on in `tools/build_radiofoundry.py`'s presets; `--no-nxdn` turns it off). It needs
`CONFIG_DMR=y` for the vocoder and the voice bridge.

## Channel fields

| Field | Values | Codeplug (open-firmware-cps.md 7.3.3.1) | Editor / CSV column |
|---|---|---|---|
| Mode | NXDN | mode byte 3 | Mode `NXDN` |
| RX / TX frequency | the radio's bands | 0x10 / 0x14 | RX / TX Frequency (MHz) |
| RX RAN | 0-63; 0 hears every RAN | 0x20 | RX RAN |
| TX RAN | 0-63 | 0x22 | TX RAN |
| Group | 1-65535 (65535 the all call): where PTT talks, and the group RX plays | 0x2E | Talkgroup |
| Hear all groups | play every group (else ours and the all call) | 0x31 bit 6 | Hear All Groups |
| Unit ID | 1-65519; blank uses the radio's DMR ID, which must then be 65519 or less | 0x27-0x29 | DMR ID (alias Unit ID) |
| Power, time-out, RX only, scan skip | as FM/DMR | as FM/DMR | as FM/DMR |

On the radio the channel editor cycles the mode FM, DMR, P25, NXDN (each where the build has it). For NXDN the RAN
row is typed (0-63, RX and TX alike), the group and the unit ID as P25's talkgroup and ID. Separate RX and TX RANs
come from the editor or CSV.

## Receive

The receiver stays in FM on the channel's frequency with the 12.5 kHz filter, the FM band-pass and the FD6818's
de-emphasis off. The HR_C7000's copy of the received audio (8 kHz) goes to the software demodulator in the radio
task every tick; the frame layer finds the frame sync, checks the RAN, and admits a call by its VCALL (from the
header, or from the SACCH superframe when it joins late). A call plays when its group is ours, the all call, or the
channel hears all groups, or when it is a private call to our unit ID. Encrypted calls (a cipher type other than 0)
are muted and shown as encrypted; nothing is decrypted. Voice goes to the speaker through the same vocoder, PCM stream
and FEC as DMR.

There is no squelch: the demodulator runs on the receiver's noise between calls and acquires again when a carrier
appears (`docs/nxdn-modem.md`, "No squelch edge").

The home screen shows the group and the caller's unit ID while a call plays, another RAN's traffic as "Busy: RAN n",
and the last caller between calls.

## Transmit rules

PTT on an NXDN channel goes through the radio core's `tx_check` like FM, DMR and P25:

| Rule | Behavior |
|---|---|
| TX inhibit, RX only, no TX frequency | Refused, with the reason on screen |
| Band limits, power, battery | The board's FM transmit entry: band limits, the calibrated power level |
| Unit ID, group | Refused without a unit ID or group, or with a unit ID above 65519 (set the channel's own ID) |
| Busy channel lockout | Settings BCL: `Carrier` refuses while NXDN frames of any RAN are heard; `Wrong tone` (the RAN lockout) while frames of another RAN are heard. Our own RAN's call: talk back |
| Time-out | The channel's TOT, else the radio's. At the limit the call ends with its release frame, then "TX time-out reached" until PTT is released |
| PTT release | The current frame is padded with voice silence, the TX_REL frame and a short tail go out, then the carrier drops and receive resumes |
| A board refusal | Shown until PTT is released; receive resumes at once |

On air: the board keys FM on the TX frequency (12.5 kHz, no sub-audio tone), the modulator's 8 kHz samples go
through the prompt FIFO into the HR_C7000's FM modulator with pre-emphasis and band-pass off, and the microphone
reaches the vocoder, not the modulator. A transmission is a 48-symbol preamble (dotting and MMDVM's 0x5775FD), the
header frame (VCALL), voice frames whose SACCH repeats the VCALL, and the release frame.

## Scan

On an NXDN channel a zone scan stops on frames with our RAN (or any RAN on a channel with RX RAN 0) and holds while a
call plays. Another RAN's frames count as a carrier only.

## Not verified in hardware

Host tests cover the radio core (`radioCoreTestNXDN`, `radioCoreTestP25`), the channel's work in the radio task with
RadioFoundry's voice bridge and a fake board (`test_radio_nxdn`), the link, modem and frame layers, the UI
(`uiFlowTest`, the layout check) and the codeplug, editor and CSV (`codeplugTest`, the site tests). The bitstream is
checked by dsd-fme and by MMDVMHost's classes, and the receiver against MMDVM's transmit shaping
(`docs/validation.md`).

Unverified on the radio: that the prompt FIFO reaches the modulator in FM transmit on this board with the voice
source switched to TX RAM, that the microphone then still reaches the CPU and not the air, the transmit deviation
scale (from the APRS measurement at 25 kHz), the low-frequency response of both paths, the FD6818's AF filter word,
the RX RAM audio level against clipping, speaker playback of decoded voice in FM mode, RX/TX switching times, the
UI on the display, and interoperation with other NXDN radios. `docs/nxdn-modem.md` ("What the radio must confirm")
and `docs/hil-checklist.md` list the steps.
