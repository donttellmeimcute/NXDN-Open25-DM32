# RadioFoundry and P25 feasibility reassessment

**Superseded implementation direction:** the user subsequently selected
RadioFoundry and a hardware-first modem route. See [conventional integration](conventional.md)
for the implemented derivative, tests and current limitations. This page is the
earlier assessment, not the current build status or modem preference.

2026-11-05. Scope: clear Phase 1 FDMA conventional subscriber RX/TX; assess
passive trunk tracking and active trunked subscriber roles separately. This is
a source/document review, not a hardware test or integrated firmware release.

## Conclusion

RadioFoundry supplies a much stronger implementation base than our October
binary-patching approach. It has public DM32 startup/RTOS, RF/calibration,
microphone/speaker, configuration, programming and diagnostic drivers. Its
authors report working FM/DMR and APRS on the radio. We have inspected source;
we have not independently repeated those hardware tests.

A concrete P25 engineering path now exists: build an embedded software C4FM
modem on its FM receive-sample and generated-modulation paths, connect the
existing Phase 1 CAI and full-rate IMBE components, and retain the board's RF,
PTT and protection infrastructure. The decisive unresolved issue is whether
the entire analog/baseband transfer path preserves P25 sufficiently well.
APRS at 1200 bit/s is useful evidence of sample access, not proof of P25 at
4800 symbols/s. No P25 radio operation has been demonstrated here.

## Pinned implementation evidence

RadioFoundry commit `dfbaf20c400c73e2ccf6edb80af7d1d255bee418`, clean local
clone `research/upstream/radiofoundry`. Original URLs, date, file hashes and
local TIA PDF identities are in `docs/radiofoundry-sources.json`.

| Component | Source | What it establishes |
|---|---|---|
| Microphone/speaker | `platform/hrc7000/audio_pcm.c` | 80-sample microphone IRQ halves combined into 160-sample blocks; speaker prompt FIFO feeding. Author reports the alternative CPU speaker buffer was choppy. |
| Continuous FM receive | `platform/hrc7000/fm_rx_audio.c`, `docs/spec/modem-hrc7000.md` 8.5 | Two 256-sample halves, reported 8 kHz/32 ms, unsquelched samples, SYS IRQ bit14, CPU ring already used by APRS. |
| Generated modulation | `platform/hrc7000/hrc_afsk.c`, modem spec 8.4 | 8 kHz generated samples through prompt FIFO, 2 ms replenishment, board routing/mic muting; authors report off-air APRS decoding. |
| RF/filter configuration | `drivers/modem/hrc7000/hrc7000.c` | Ordinary FM setup enables bandpass/pre-emphasis and a limiter. These settings must be reviewed/changed and the complete path measured for P25. |
| Codec | `platform/hrc7000/vocoder.c` | Stock AMBE+2 hosting, not an IMBE codec. Its DMR-specific bits interface cannot simply stand in for P25 voice. |
| Runtime ABI | `platform/hrc7000/platform.mk` | `-mcpu=ck803s -mlittle-endian -mno-high-registers -msoft-float`. High registers are not context-switched; our earlier objects require a matching rebuild before RTOS integration. |
| Memory | `boards/dm32/memory.ld` | Current application RAM region ends at0x43a00; a separate 83,456-byte stock-codec window is reserved. That reservation is not free RAM when retaining DMR. Our component sizes do not prove fit in a full RadioFoundry build. |
| Remote development | debug-link and stub-entry specs | Remote keys/PTT with bounded hold, screenshots, diagnostics, reboot and upload/jump into RAM update monitor from a running application. Failed boot/hard recovery remains separate. |

The preferred path avoids depending on the DMR deframer's bit permutation by
demodulating the FM samples in software. Raw physical-layer hardware streaming
remains an alternative, not a proven prerequisite or an established P25 mode.

## The remaining waveform and real-time gate

TIA-102.BAAA-B section8 (PDF60-61, printed46-47), Table20 and Figure36 specify
the Phase 1 modulation, symbol mapping and transmit filtering. The nominal
C4FM symbol levels are600/1800 Hz either side of center; the specified filtering
extends to2880 Hz. An 8 kHz sample interface is theoretically sufficient in
bandwidth, but only offers5/3 samples per symbol. Timing recovery, fractional
resampling and proper shaping are required. This is an inference from the
standard/sample rates, not a demonstrated demodulator.

Measure RX/TX frequency/phase response including low frequencies, DC behavior,
de-emphasis/pre-emphasis, bandpass, gain, clipping and sample continuity. The
FD6818 AF output, codec mixer and C7000 processing may alter the signal before
the CPU sees it or after it generates samples. Recover complete independently
known frames on RX and independently measure continuous TX before voice.
Then measure worst-case IMBE encode/decode plus modem/FEC with IRQs active.
Do not infer reliable CQPSK/LSM simulcast reception from FM sample access or
from a successful C4FM test.

## Trunked CAI

CAI is the air interface used by both conventional and trunked P25. Our current
library implements voice units, not a complete trunking controller/subscriber.
It lacks control-channel TSBK/PDU/MBT decoding, their necessary coding/checks,
channel-identifier/bandplan handling, site state, grant-following and retuning.

Passive tracking is the next sensible extension after conventional voice:
decode the outbound control channel, derive working frequencies from identifier
messages, select a talkgroup grant, retune the single receiver to the Phase 1
voice channel, then return to control when the call ends. A single receiver
cannot simultaneously monitor both. No registration/affiliation transmission
is needed merely to listen to broadcast grants and voice.

Active trunked subscriber operation is substantially more work: inbound
signaling, registration/affiliation, call request/grant state and timing,
channel access and optional authentication handling. This needs controlled
system interoperability tests beyond passive scanning and beyond voice CAI.
Native VHF/UHF tuning does not give this board700/800 MHz capability. Systems
assigning Phase 2 voice need a separate TDMA implementation; they are outside
this Phase 1 assessment.

## Standards now available locally

Originals remain in Downloads; hashes checked against the corpus catalog.
No PDFs or full standard text are copied into tracked/public project files.
Verified source clauses for this assessment:

- `TIA-102.BAAA-B`;8.1-8.4.1, Table20/Figure36; PDF60-61;
  printed46-47; `p25_d1460d9ae0713992`;
  `documents/tia-library/8.15_TIA-102.BAAA-B_Final Published Document_2017_06_22.pdf`.
- `TIA-102.AABC-E`;4.2.1.2/Figure4.2.1-2 and4.2.2;
  PDF59-60; printed40-41; `p25_89a3b878eee70a14`;
  `documents/tia-library/8.10_TIA-102-AABC-E_Final Published Document_2019_04_231.pdf`.
- `TIA-102.AABD-B`;6/Figure6-1 and6.1-6.2; PDF51-52;
  printed46-47; `p25_a48ebc16b125fb93`;
  `documents/tia-library/8 10 TIA 102 AABD B Final Published Document 2014 11 05.pdf`.

Relevant pages/figures were rendered for inspection. These are base editions,
not a claim to have audited every addendum. The collection also holds AABC-E-1,
E-3 and AABD-B addenda; read their applicability before implementing affected
behavior. Its saved gap audit identifies missing AABA-C (trunking overview),
among other documents. That gap does not erase the supplied base message and
procedure specifications, nor does an older overview fill the missing revision.

## Integration sequence and license boundary

1. Establish backups/recovery and a verified RadioFoundry DM32 baseline.
2. Use its diagnostics to capture native FM samples and inject controlled
   modulation. First gate: complete known C4FM frames, independently both ways.
3. Implement the embedded modem, bind IMBE/CAI to live mic/speaker/PTT, rebuild
   for its RTOS ABI, and measure timing/RAM/stack. Preserve analog/DMR mode paths.
4. Validate standalone conventional voice. Add passive Phase 1 trunk following
   next; active subscriber behavior is a separate later milestone.

RadioFoundry is MIT with an explicit clean-room/permissive-only contribution
policy. Our existing IMBE/CAI work is GPL-3.0-or-later. Dropping it into an
MIT-only upstream contribution is not an acceptable integration assumption.
A separate GPL-compatible derivative or an appropriately licensed implementation
needs an explicit source/license plan. This review changes no upstream source,
claims no clean-room contribution, and produces no new flash image.
