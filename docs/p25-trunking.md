# P25 trunking

A trunk channel (`P25T`) puts the radio on one P25 Phase 1 trunked system.
- **Listens (always):** finds the system's control channel, follows group calls to FDMA voice channels and back.
- **Transmits (only in a `CONFIG_P25_TRUNK_TX` build):** registers, affiliates to its talkgroup and asks for a voice
  channel on PTT.

Phase 2 (TDMA) voice channels are not followed: see [p25-phase2.md](p25-phase2.md).

## Code

| Layer | Files | Runs in |
|---|---|---|
| TSBK coding: CRC-CCITT, rate 1/2 trellis (Viterbi), interleave, TSDU frames with status symbols; receiver | `src/p25t/tsbk.c` | P25 worker (receive tap) |
| Messages: OSP and ISP layouts, band plan (IDEN_UP, IDEN_UP_VU, IDEN_UP_TDMA) to frequencies | `src/p25t/tmsg.c` | both |
| Subscriber logic: hunt, band plan, registration, affiliation, grants, PTT requests, random access | `src/p25t/su.c` | radio task |
| Radio channel layer: control channel tap, events to the radio task, tuning, inbound packets, voice | `integration/radiofoundry/p25t/radio_p25t.c` | radio task + tap |
| Radio core: trunk mode, TX rules, no busy lockout; screen state | patches `0007`, `0008` | radio task, UI |

The protocol code has no RTOS and no hardware access. Host tests run it unchanged.

## Channel settings (codeplug, editor, CSV)

| Field | Where | Use |
|---|---|---|
| RX frequency | channel | The control channel |
| TX frequency | channel | Its inbound frequency, until the system's band plan gives it |
| WACN, System ID | channel (P25T) | The system. Transmit needs both, and the control channel must broadcast both (NET_STS_BCST and RFSS_STS_BCST) before anything is sent |
| Unit ID | channel override, else the radio's DMR ID | Assigned by the system's administrator. Every inbound packet carries it |
| Talkgroup | channel | Affiliated to; PTT talks to it |
| RX NAC | channel | The control channel's NAC, or any (F7E) |
| Hear all | channel | Follow every group grant, not only the talkgroup |

The logic also follows a priority list of up to 8 talkgroups (`p25t_su_cfg.priority`). The channel record has no
field for it yet, so on the radio the list is empty.

The radio's menu shows and edits NAC, talkgroup and unit ID as for P25. WACN and System ID are set in the codeplug
editor (or CSV), which validates them as 20 and 12 bits.

The radio covers VHF and UHF only. 800 MHz systems are out of band and their channels are refused by the radio core
(`TXDENY_BAND`).

## Behavior

| State | Screen (PTT line) | What happens |
|---|---|---|
| Hunting | "Searching for the site" | Waits for the configured WACN/System ID in NET_STS/RFSS_STS. A channel without either for 3 s means the next listed control channel. A trunk channel lists one; the logic takes up to 4. |
| On the site | "TG n", "Listen only" or "Not registered" | Stores the band plan. With transmit on, registers (U_REG_REQ), then affiliates (GRP_AFF_REQ), each at most every 30 s. After a refused registration it waits 60 s. |
| Following a call | the call (TG, source) | A GRP_V_CH_GRANT or _UPDT for the talkgroup, a priority list entry or (hear all) any group. Tunes to the voice channel and plays it as a P25 channel; encrypted calls are muted. Returns after a terminator or 1.5 s without frames. |
| Requesting | "Requesting a channel" | PTT sends GRP_V_REQ. ACK_RSP_FNE waits for the answer; QUE_RSP queues (10 s at most); DENY_RSP refuses. A grant for our group and unit ID transmits on the voice channel's inbound frequency. |
| Queued | "Queued by the system" | |
| Talking | as a P25 call | The FDMA call (HDU, LDU1/LDU2 with our unit ID, TDU), then back to the call's hang time. |

A refusal reaches the radio core like a refused transmission:

| Cause | Radio core shows |
|---|---|
| The system denied the call, or the radio is not registered or not on the system | `TXDENY_REJECTED` |
| No answer after 4 tries, or queued too long | `TXDENY_TIMEOUT` |
| Transmit off | `TXDENY_MODE` |
| Unit ID, talkgroup, WACN or System ID missing | `TXDENY_NO_ID` |

### Inbound random access

The outbound status symbols mark the inbound channel idle or busy every 7.5 ms microslot. A pending packet waits
for an idle microslot and then a random number of slots (6 microslots each) in a window of 4. The window doubles on
each retry. With no answer in 1.5 s it retries, 4 tries in all.

These timing values are this implementation's own, not TIA-102.AABD's, which is not available here. Check them
against the standard before transmitting on a real system.

On the radio, a packet goes out as a short native transmission: three lead blocks (90 ms) while the PA ramps, the
TSDU (about 33 ms), and a tail block. The radio is deaf meanwhile. The packet therefore starts about 100 ms after
the idle slot it waited for. That pipeline latency is not measured on the radio.

## Safeguards

| Rule | How |
|---|---|
| Transmit off by default | `CONFIG_P25_TRUNK_TX` is off unless built with `--trunk-tx`. Without it the radio core refuses PTT on a trunk channel (`TXDENY_MODE`), the channel layer has no inbound packet path compiled in, and the logic's `tx_enabled` is false. |
| Only on the configured system | Transmit needs a user-entered WACN (20 bits), System ID (12 bits), unit ID (24 bits) and talkgroup (`p25t_su_init` clears `tx_enabled` otherwise). The control channel must broadcast that WACN and System ID first, and another system's broadcasts send the logic back to hunting. |
| Our unit ID only | Every inbound packet and our voice carry `cfg.unit_id`. IDs heard on the air are shown, never stored for transmit. No function reads, copies or spoofs other units' IDs. |
| No transmission without PTT | Except registration, affiliation and de-registration on the configured system, each rate-limited. Leaving the channel sends nothing, because the board is back in FM first. |
| Out of scope | Emergency, supplementary services, individual calls, data, and encryption. Encrypted calls are muted and never decrypted. |

Test trunked transmit only on a system you are authorized on, or in a lab.

## Tests

| Test | What it covers |
|---|---|
| `tests/p25t/test_tsbk.c` | CRC, trellis Viterbi (0-2 bit errors always, 3 in 99% or more), TSDUs of 1-3 blocks, polarity, status symbols, NAC |
| `tools/p25t_validate.py` | OP25's `tk_p25` parses 10 OSP types as written; dsd-fme decodes 18 TSDUs of a control channel stream with every value |
| `tests/p25t/test_su.c` + `site.c` | A simulated site (control channel, FDMA and TDMA voice channels, collisions) with half-duplex units: hunt, registration, affiliation, following, PTT calls (grant, ACK, queue, deny, no answer), 3 units contending, lost control channel, de-registration, other systems, transmit off, incomplete identity, Phase 2 calls |
| `tests/p25t/test_radio_p25t.c` | `radio_p25t.c` unchanged, on host stand-ins for the native modem and P25 worker: registration through the tap, a followed call, a granted FDMA call heard by the site, a refusal |
| `radioCoreTestP25T`, `radioCoreTestP25TX` | Radio core: trunk mode selection, receive-only PTT refusal, refusals and timeouts, missing identity, no busy lockout |

Results in the simulated site:
- Control channel found 3.1 s after start, after the empty first frequency.
- Registered 0.28 s later, affiliated 0.23 s after that.
- Grant followed in 75 ms.
- PTT to grant 250 ms.
- 3 units registered and affiliated in 5.3 s through the random access.

## Not verified on the radio

All of it. In particular:
- Decoding TSDUs from the native receive stream (the same path as P25 conventional, itself unverified).
- Retune time between control and voice channels.
- The inbound packet pipeline's latency and the PA ramp against the system's access timing.
- Interoperation with a real P25 trunked system.

`docs/hil-checklist.md` (Phase 4) gives the steps.
