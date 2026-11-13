# Hardware-in-the-loop checklist

What has to be checked on a DM-32UV before the multi-mode image counts as working: FM, DMR, P25 conventional,
NXDN48 and P25 trunking. Everything below is still open, because no radio was connected while the image was built.
The steps go in order of risk:
1. Backup.
2. Receive only.
3. Conventional transmit at the lowest power into a dummy load or an attenuated cable.
4. Trunked transmit, only on an authorized system or in a lab.

Stop at the first failure, record it, and do not go on to a later step on that image.

**Rules for every step**
- Use a programming cable you can name (`--port ACTUAL_PORT`). No tool picks a port by itself.
- Transmit only into a dummy load, or into an attenuated cable to the reference receiver.
- Transmit only on a frequency and power your license allows. Use the lowest power level unless a step says
  otherwise.
- Never on a trunked system you are not authorized on.
- Keep unit IDs, calibration data, system identities and recordings of other people's traffic out of public reports.

Record each step: the image's SHA-256, the date, the equipment, the result, and the files kept.

## 0. Backup and calibration (before anything is installed)

| # | Check | How | Pass |
|---|---|---|---|
| 0.1 | Identify the unit | Displayed firmware version, board revision, serial label (kept private) | Recorded |
| 0.2 | Full backup of both flash chips, twice | `tools/dm32_readonly_backup.py --port ACTUAL_PORT --installed-firmware VERSION --out backups/SESSION` ([recovery.md](recovery.md)) | Both passes complete; every page matches; the JEDEC IDs give the chip sizes; hashes recorded |
| 0.3 | Second copy off the PC | Copy `backups/SESSION` to separate storage; compare hashes | Hashes equal |
| 0.4 | Calibration and identity | Check the unit's calibration block in the backup ([unit048-compatibility.md](unit048-compatibility.md)); keep a separate copy | Calibration present and readable; never committed to any repository |
| 0.5 | Codeplug | Export it with the stock CPS or the codeplug editor | File kept with the backup |
| 0.6 | Restore path | Read [recovery.md](recovery.md) and know which path restores this unit | The operator can name the restore steps before going on |

## 1. Install and health

| # | Check | How | Pass |
|---|---|---|---|
| 1.1 | Build the image | `python3 tools/build_radiofoundry.py` (trunked transmit off). Record flash/RAM and the SHA-256 | Zero warnings; stack check passes (`--stack-check`) |
| 1.2 | Install | The project's installer and update path ([firstboot-installation.md](firstboot-installation.md)) | Readback hash equals the built image |
| 1.3 | Boot | Power on without the cable | Normal screen, version shown, no crash screen |
| 1.4 | Health receipt | `tools/firstboot_check.py --port ACTUAL_PORT` (first-boot image) or the debug link status | No fault; calibration in use is the unit's own |
| 1.5 | Battery | 30 min receive on battery | No reset; current within the stock firmware's figure ±10 % |

## 2. FM and DMR (regression)

| # | Check | Pass |
|---|---|---|
| 2.1 | FM RX: reference generator, 12.5 and 25 kHz, CTCSS/DCS | Squelch opens at the stock firmware's level ±2 dB; tone decode works |
| 2.2 | FM TX (low power, dummy load) | Deviation and frequency within limits; CTCSS present; audio clean on the reference receiver |
| 2.3 | DMR RX and TX against a reference DMR radio, both timeslots | Voice both ways; IDs and talkgroup shown; no AMBE errors in a 1 min call |
| 2.4 | Channel and mode switching: FM, DMR, P25, NXDN, P25T channels, each 10 times | No hang, no lost audio path, PTT works after each switch |

## 3. P25 conventional

| # | Check | Pass |
|---|---|---|
| 3.1 | RX from a reference P25 radio (NAC 293, TG 1), 1 min call, speaker | Intelligible; TG and source shown; no buffer underruns in the debug counters |
| 3.2 | RX with another NAC, another TG, an encrypted call | Not played; the encrypted call shows as encrypted and stays muted |
| 3.3 | TX at the lowest power into the reference radio and an SDR decoder (OP25 or dsd-fme) | Decoded with 0 to few FEC corrections; frequency error and deviation (±1.8 kHz outer) within TIA-102.CAAA limits |
| 3.4 | Long TX (2 min) | No queue underrun; encode time per 20 ms frame below 20 ms (`tools/cpu_bench.py`, measurement build) |
| 3.5 | Late entry, signal loss and recovery | Audio resumes within one LDU |

## 4. NXDN48

The measurements [nxdn-modem.md](nxdn-modem.md) lists come first, because the modem's compensation needs them:
- The low-frequency response of the flat RX and TX paths (fill `flat_rx_hpf_hz`, `flat_tx_hpf_hz`,
  `flat_rx_deemph_us`).
- The deviation scale (`fm_tx_nominal_peak`).
- The FD6818 AF filter word.

| # | Check | Pass |
|---|---|---|
| 4.1 | Flat paths: a known tone and a sweep through the FM path; SDR measures deviation | Measured values written into the board's audio tap; deviation ±1050/±350 Hz within 5 % |
| 4.2 | RX from a reference NXDN radio or an MMDVM hotspot (RAN 12, group 200) | Voice plays; RAN and group filter as set; late entry works |
| 4.3 | TX at the lowest power: decoded by the reference radio and by dsd-fme from an SDR recording | Header, voice and TX_REL decoded; adjacent channel power within limits |
| 4.4 | Encrypted NXDN call | Muted, shown as encrypted |

## 5. P25 trunking, receive (default image: trunked transmit off)

A real Phase 1 system you may listen to works for this, as does a lab site. For a lab site:
- Replay the simulated site's control channel (`tests/p25t/site.c`) through a signal generator with an arbitrary
  waveform, at a low level into a cable.
- Do not radiate.

| # | Check | Pass |
|---|---|---|
| 5.1 | Trunk channel with the system's control channel, WACN, System ID | "Listen only" shown within 5 s; OP25 on an SDR shows the same system |
| 5.2 | Wrong WACN or System ID | Stays on "Searching for the site"; nothing transmitted (SDR on the inbound frequency) |
| 5.3 | Group call on the talkgroup | Followed within 0.5 s of the grant; audio plays; back to the control channel after the call |
| 5.4 | Hear all; another talkgroup | Followed only with hear all |
| 5.5 | Control channel lost (generator off) | "Searching for the site" within 3 s; back when it returns |
| 5.6 | Retune time control to voice channel | Measured and recorded (the logic allows 1.5 s of hang) |

## 6. P25 trunking, transmit (`--trunk-tx` image, authorized system or lab only)

Before this step:
- The system's administrator has assigned the unit ID and authorized the test.
- The inbound access timing in [p25-trunking.md](p25-trunking.md) has been checked against TIA-102.AABD and
  adjusted if needed.

| # | Check | Pass |
|---|---|---|
| 6.1 | Registration and affiliation on power-up | The system shows the unit registered and affiliated; at most one attempt every 30 s; "TG n" shown |
| 6.2 | Inbound packet timing: SDR on the inbound frequency, triggered on the outbound status symbols | The packet starts in an idle inbound slot; the lead (90 ms) and its PA ramp recorded |
| 6.3 | Group call: PTT, grant, voice on the granted channel, release | The system grants and other radios hear the call; source shown is the configured unit ID |
| 6.4 | Denied and queued calls (system configured to deny or busy) | "Requesting a channel" / "Queued by the system"; refusal shown; nothing transmitted on the voice channel |
| 6.5 | No PTT for 10 min | Nothing transmitted except registration and affiliation renewals |
| 6.6 | Unit ID missing from the channel | PTT refused (`NO_ID`); nothing transmitted |

## 7. Resources and real time

| # | Check | Pass |
|---|---|---|
| 7.1 | `tools/cpu_bench.py` (measurement build with `--cpu-profile`) during P25 TX, P25 RX, NXDN RX/TX, a trunk channel on the control channel and following a call | Busy below 80 %; no task's stack below its static margin; heap minimum above 4 KB |
| 7.2 | Compare with the estimates in [resources.md](resources.md) | Recorded; differences above 2× explained |

## 8. Restore

| # | Check | Pass |
|---|---|---|
| 8.1 | Restore the stock firmware from step 0 | Stock firmware boots; calibration and codeplug as before; FM and DMR work |
| 8.2 | Reinstall the image | As step 1 |
