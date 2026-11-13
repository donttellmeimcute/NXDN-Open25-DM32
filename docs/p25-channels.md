<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# P25 conventional channels

A P25 channel is a channel mode like FM and DMR. It is stored in the codeplug, edited in the web Codeplug Editor,
in `channels.csv` and on the radio, and the radio core applies the same transmit rules to it. The modem's native
physical layer receives and transmits the 4800-symbol/s C4FM stream; the CPU runs IMBE and the CAI framing.

Build switches: `CONFIG_P25=y CONFIG_P25_NATIVE=y` (the `native` preset of `tools/build_radiofoundry.py`).
`CONFIG_DEV_TX` is the laboratory gate described below.

## Channel fields

| Field | Values | Codeplug (open-firmware-cps.md 7.3.3.1) | Editor / CSV column |
|---|---|---|---|
| Mode | P25 | mode byte 2 | Mode `P25` |
| RX / TX frequency | the radio's bands | 0x10 / 0x14 | RX / TX Frequency (MHz) |
| RX NAC | 000-FFF hex; `any` (F7E) hears every NAC | 0x20 | RX NAC |
| TX NAC | 000-F7D hex | 0x22 | TX NAC |
| Talkgroup | 1-65535: where PTT talks, and the group RX plays | 0x2E | Talkgroup |
| Hear all groups | play every talkgroup (else ours and All Call FFFF) | 0x31 bit 6 | Hear All Groups |
| Unit ID | 1-16776415; blank uses the radio's DMR ID | 0x27-0x29 | DMR ID (alias Unit ID) |
| Power, time-out, RX only, scan skip | as FM/DMR | as FM/DMR | as FM/DMR |

A blank tone word from an FM CPS reads as RX NAC `any` and TX NAC 293. qdmr and the open-firmware CPS keep the bytes
of records they don't edit, but show mode 2 as an unknown mode.

On the radio the channel editor cycles the mode FM, DMR, P25, edits the talkgroup and the unit ID (Own ID) and
switches RX between our NAC and every NAC. The NAC values themselves are set in the editor or CSV (hex needs
letters the keypad doesn't have).

## Receive

The native receiver hears every NAC. Frames with another NAC are counted for the busy lockout and never reach the
speaker. A call plays when its header or LDUs carry clear voice (ALGID 0x80 and a valid encryption sync), a
talkgroup and a source, and the talkgroup is ours, All Call, or the channel hears all groups. Encrypted voice is
muted and shown as "Encrypted TG n"; nothing is decrypted. Late entry waits for the LDU1 link control and the LDU2
encryption sync.

The home screen shows the talkgroup and the caller's unit ID while a call plays, the emergency flag in the warning
color, and the last caller's ID and age between calls.

## Transmit rules

PTT on a P25 channel goes through the radio core's `tx_check` like FM and DMR:

| Rule | Behavior |
|---|---|
| TX inhibit, RX only, no TX frequency | Refused, with the reason on screen |
| Band limits | `board_in_band`, and the board's own TX entry refuses an out-of-band frequency |
| Power | The channel's power level (or the radio's), through the board's calibrated power ramp. TX needs valid factory calibration |
| Battery | Refused when critical; the native layer re-checks every 25 ms on air |
| Unit ID, talkgroup | Refused without a unit ID (channel or radio) or a talkgroup |
| Busy channel lockout | Settings BCL: `Carrier` refuses while P25 frames of any NAC are heard; `Wrong tone` (the NAC lockout) while frames of another NAC are heard. Our own NAC's call: talk back |
| Time-out | The channel's TOT, else the radio's. At the limit the call ends with its terminator, then "TX time-out reached" until PTT is released |
| PTT release | The current LDU is finished with encoded silence, the TDU goes out, the PA drops. At most one LDU (180 ms) plus the queued blocks (up to 210 ms) |
| Any fault | A modem, timing, lease or battery fault drops the PA at once (`nt_fault` quiets the modem and the PA before anything else) and is shown until PTT is released |

## Scan

On a P25 channel a zone scan stops on frames with our NAC (or any NAC on a channel that hears every NAC) and holds
while a call plays. Another NAC's frames count as a carrier only. The probe and sync windows are DMR's.

## Laboratory gate

`CONFIG_DEV_TX=y` (with `CONFIG_FIRSTBOOT=y CONFIG_DEV_UPDATE=y`, `--first-boot --dev-update --tx-bench`) keeps the
bench restrictions as a build: TX also needs `python tools/p25_channel.py --port PORT enable` this boot (RAM only),
145.500 MHz and the lowest power, and the first-boot guards allow the PA only for that native session. The normal
build has none of these.

## Not verified in hardware

Everything above is checked by host tests (`radioCoreTestP25`, the native tests, `codeplugTest`, the site tests)
and the transmit chain's bitstream by dsd-fme (docs/validation.md). Unverified on the radio: the PHY_RDY handoff
fix, the RX/TX switching time, the waveform and deviation, the calibrated power on P25, busy lockout timing, scan
stops and the UI on the display. `docs/hil-checklist.md` lists the steps.
