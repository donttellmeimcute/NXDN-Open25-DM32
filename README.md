# Open25-DM32

Open25-DM32 is an independent firmware research project to bring **APCO P25 Phase 1 digital voice** to the **Baofeng DM-32UV** using the radio's existing hardware. The first goal is reliable two-way conventional voice. The longer-term goal is a radio that can participate in an authorized Phase 1 trunked system, if the hardware and interoperability tests support it.

[![Donate via GitHub Sponsors](https://img.shields.io/badge/Donate-GitHub%20Sponsors-EA4AAA?style=for-the-badge&logo=githubsponsors&logoColor=white)](https://github.com/sponsors/tylerwatt12?metadata_project=open25-dm32)
[![Donate via Ko-fi](https://img.shields.io/badge/Donate-Ko--fi-FF6433?style=for-the-badge&logo=kofi&logoColor=white)](https://ko-fi.com/kd8zmm)

## Where it stands

The DM-32UV has decoded live conventional P25 calls and played intelligible audio through its built-in speaker. A reference radio has also heard intelligible speech from the DM-32UV's microphone in short bench calls. Longer transmissions still show RF decode errors. The immediate work is to verify modem timing and measure the transmitted signal independently.

This repository contains experimental source code, tests, and bench notes. Firmware downloads are unavailable. The development setup has been exercised on a specific radio and firmware revision. See the [progress log](PROGRESS.md) for test results and open problems.

## Roadmap

| Milestone | What needs to work |
| --- | --- |
| 1. Stabilize conventional voice | Repeat clear receive and transmit calls using the built-in speaker and microphone. Check transmit buffer timing, deviation, frequency accuracy, and spectrum on a controlled bench with a reference radio and independent capture. |
| 2. Build usable conventional channels | Add channel settings for frequencies, NAC, and group calls. Validate PTT, call setup and teardown, and recovery. Measure flash, RAM, and real-time headroom before deciding which analog FM and DMR features can coexist with P25. |
| 3. Follow trunked calls in receive-only mode | Decode a Phase 1 control channel, resolve channel grants, tune to voice traffic, and return to the control channel. Compare results with an independent receiver. |
| 4. Evaluate active trunking | Once conventional transmit is reliable, qualify an authorized lab with working control and traffic paths. Use it to test registration, affiliation, and grant-controlled group calls before considering roaming or supplementary services. |
| 5. Qualify a release | Test extended calls, supported hardware revisions, settings migration, and recovery. Document supported features and confirm redistribution terms before distributing firmware. |

The order and scope can change as measurements come in. Replay tests, hardware acceptance checks, and preservation of factory calibration run through every milestone.

## Support development

Support development through [Ko-fi](https://ko-fi.com/kd8zmm) or [GitHub Sponsors](https://github.com/sponsors/tylerwatt12?metadata_project=open25-dm32). The GitHub Sponsors link tags your sponsorship for Open25-DM32. Donations help cover development and testing expenses. Sponsorship is optional and carries no delivery commitments.

You can also help by reviewing the modem or P25 code, contributing reproducible test cases, or sharing measurements from an authorized bench setup. Open a [GitHub issue](https://github.com/tylerwatt12/Open25-DM32/issues) with the radio and firmware revision, test setup, and what you observed. Please keep radio IDs, calibration data, and private traffic out of public reports.

## Developer resources

- [Build and test guide](docs/build.md)
- [P25 conventional channels](docs/p25-channels.md)
- [Independent validation](docs/validation.md)
- [P25 framing, error correction, and synthetic test vectors](docs/protocol.md)
- [Modem research and RF test criteria](docs/modem.md)
- [Hardware validation and recovery](docs/hardware-tests.md)
- [Source provenance and license notices](docs/licenses.md)

Project code is distributed under [GPL-3.0-or-later](LICENSE) unless a file carries a different notice.
