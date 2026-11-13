# User guide: FM, DMR, P25, NXDN and P25 trunk channels

This firmware is experimental and **not verified on a radio** ([hil-checklist.md](hil-checklist.md)). Read this
before you install it:

- Keep a full backup of your radio, including its calibration ([recovery.md](recovery.md)).
- Transmit only where your license allows.
- Transmit on a trunked system only if its administrator authorized you and assigned your unit ID.
- Encrypted calls in any mode are muted. Nothing is ever decrypted.

## Channel modes

| Mode | Screen | What it is |
|---|---|---|
| FM | `FM` | Analog, as the stock radio |
| DMR | `DMR` | Tier II, both timeslots, as RadioFoundry |
| P25 | `P25` | P25 Phase 1 conventional: one frequency (or a repeater pair), a NAC, a talkgroup |
| NXDN | `NXDN` | NXDN48 conventional (6.25 kHz): a RAN and a group |
| P25 trunk | `P25T` | One P25 Phase 1 trunked system. The channel's frequency is the system's control channel; the radio moves to the voice channels by itself |

The radio covers VHF and UHF. 800 MHz trunked systems are out of its band.

## Setting up channels

Use the codeplug editor or a CSV file. The radio's menu also edits NAC, RAN, talkgroup and unit ID.

| Field (CSV column) | P25 | NXDN | P25 trunk |
|---|---|---|---|
| RX / TX frequency | the channel (TX: the repeater input) | the channel | RX: the control channel. TX: its inbound frequency |
| RX NAC / TX NAC | 000-FFF; RX `any` hears every NAC | - | RX NAC: the control channel's, or `any` |
| RX RAN / TX RAN | - | 0-63; RX 0 hears every RAN | - |
| Talkgroup | 1-65535: where PTT talks, and what plays | 1-65519 | 1-65535: affiliated to, PTT talks to it |
| Hear All Groups | play every talkgroup | play every group | follow every group call on the system |
| WACN | - | - | 5 hex digits, from the system's administrator |
| System ID | - | - | 3 hex digits, from the system's administrator |
| Unit ID | the channel's ID, else the radio's DMR ID | the same; NXDN needs 1-65519 | the same: **the ID the administrator assigned you** |

## Using a P25 or NXDN channel

The screen shows:
- The PTT line with the talkgroup.
- The NAC or RAN.
- The caller's talkgroup and unit ID during a call.

Calls on another talkgroup, NAC or RAN, or encrypted calls, show but do not play.

PTT sends a call to the channel's talkgroup with your unit ID. The time-out timer applies. Busy channel lockout
refuses PTT while another NAC (RAN) is heard, or any traffic, as the setting says.

## Using a P25 trunk channel

| PTT line | Meaning |
|---|---|
| Searching for the site | No control channel with the channel's WACN and System ID is heard yet |
| Listen only | On the system; this firmware does not transmit on trunked systems (the default build) |
| Not registered | On the system; transmit is built in, but the system has not registered the radio (yet, or it refused) |
| TG n | Registered: PTT asks for a channel to talk to talkgroup n |
| Requesting a channel | PTT is down and the request is out |
| Queued by the system | The system will give a channel when one is free (at most 10 s) |

Group calls on the channel's talkgroup play, and so do all calls with Hear All Groups. The radio goes to the voice
channel and comes back to the control channel after the call.

PTT on a trunk channel (only with a firmware built with trunked transmit) works as follows:
- Hold PTT. The radio asks the system for a channel and talks when the system grants it.
- If the system denies the call, or doesn't answer, the screen shows the refusal until you release PTT.
- The radio sends nothing without PTT, except its registration and its affiliation to the talkgroup. It sends each
  at most every 30 s, and only to the system the channel names.

Limits:

| What | Behavior |
|---|---|
| Phase 2 (TDMA) calls | Not heard: the radio cannot receive or send Phase 2 |
| Control channels | One per channel. For another site, use another channel |
| Call types | No emergency, private calls, data or supplementary services |
| Leaving the channel | De-registration is not sent; the system ages the registration out |
