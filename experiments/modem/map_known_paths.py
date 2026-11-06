#!/usr/bin/env python3
"""Reproduce exact-image modem/audio code anchors without modifying firmware."""
import csv
import hashlib
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
EXPECTED = "03107b6f600e7fa95105af8324cff349c0f17fe30db249c14d11e79baa8da97a"

# Name, runtime start, exclusive end, confidence, concrete evidence.
PATHS = [
    ("fm_play_enable", 0x0300c2dc, 0x0300c302, "direct instructions + manual p96",
     "RMW AUDIO_CONTROL bit6 at 0x11000080 from bool r0"),
    ("fm_ptt_set", 0x0300c304, 0x0300c326, "direct instructions + manual p338",
     "Writes bool r0 to FM_PTT 0x11000560; does not establish PA control"),
    ("txram_copy", 0x0300d3ec, 0x0300d41c, "direct instructions",
     "Byte copy r1 source to 0x16000000+r0; r2 truncated uint16 byte count"),
    ("rxram_copy", 0x0300d41c, 0x0300d444, "direct instructions",
     "Byte copy 0x160004b0+r0 to r1; r2 truncated uint16 byte count"),
    ("vocoder_mailbox_copy", 0x0300d444, 0x0300d46c, "direct instructions",
     "Byte copy 0x16000960+r0 to r1; r2 truncated uint16 byte count"),
    ("irq_callback_registration", 0x0301d040, 0x0301d07c, "direct instructions + manual p106",
     "Registers PCM capture vector60->0x2c4a4 and playback vector59->0x2c49c; system61->0x2c454"),
    ("physical_test_select", 0x03021588, 0x030216b4, "direct instructions + manual p295",
     "Selectors1/3 fill0x5f selector2 fill0x55; writes36bytes to each TX half, WORK_MODE2 L2_CONTROL82 TXRX81; PA context unverified"),
    ("modulation_amplitude_set", 0x03021748, 0x03021764, "direct instructions + manual p282",
     "Replaces low16 RF_MOD_BIAS_CTRL at0x11000114 with repeated uint8(r0), preserves high16"),
    ("channel_band_and_tuning_set", 0x0300da88, 0x0300db1c, "direct instructions",
     "Compares TXr0 and RXr1 with20000000; sets0x3e31b bits4/3; stores TX/RX words0x38468/6c for supported mode branches"),
    ("channel_calibration_load", 0x0300db48, 0x0300dc80, "direct instructions + traced consumers",
     "TX channel bytes4..7 and powerselector byte8 bits2:1 feed0x301b33c; stores DAC ramp target at0x38498; other calibration consumers remain separate"),
    ("rf_switch_worker", 0x0300dc94, 0x0300dce4, "direct instructions + IRQ producer trace",
     "Waits on0x38590; for channel mode0x10 dispatches flags0x38464/65 to TX0x2b440 or RX0x2b4a0"),
    ("rf_restore_worker", 0x0300dce4, 0x0300dd1c, "direct instructions; event meaning unverified",
     "Waits on0x3fd90 mask0x200 and ifTXflag0x3e1c0 bit0 set calls RXrestore0x2b4a0; no proven thermal/battery attribution"),
    ("rf_rx_setup", 0x0300df08, 0x0300df46, "direct instructions",
     "Tunes RX word0x3846c through0x2b6a0 then enables RXband0x2c878; retains mode dependent stock processing"),
    ("rf_tx_setup", 0x0300e40c, 0x0300e49c, "direct instructions",
     "Disables RXband, enables TXband, applies stock subtone/mode controls, tunesTX0x2b674 and ramps calibrated DACB target0x38498"),
    ("rfic_idle", 0x0300e918, 0x0300e924, "direct instructions",
     "Writes RFIC register0x30=0 through0x2c2f4; does not itself prove external PA disabled"),
    ("tx_level_calibration_lookup", 0x0301b33c, 0x0301b470, "direct instructions + channel/ramp dataflow",
     "Clamps frequency byTXband; reads bank2 UHF levels0x40/45/4a or VHF0xd0/d5/da plus interval index; interpolates and returns uint8 target"),
    ("candidate_ptt_input_read", 0x03012f48, 0x03012f58, "direct instructions; physical button identity inferred",
     "Reads indexedGPIO43=B11 through0x2c588 and inverts result; active wheninputlow"),
    ("candidate_ptt_input_debounce", 0x03012f58, 0x03013016, "direct instructions + initial callback table",
     "Debounces candidatePTT change afterfour differing polls; sets0x3e1c0 eventbits1/2 andinvokes callbacks initially0x3014e4c/0x3014e00"),
    ("candidate_ptt_admission", 0x030149fc, 0x03014c4c, "direct instructions; individual policy checks not named",
     "Normal candidatePTTpress invokes withr0=0; substantial stock state/mode gating andsideeffects; nonzero return permits postingpress request"),
    ("candidate_ptt_release_callback", 0x03014e00, 0x03014e4c, "direct instructions + initial callback table",
     "Checks stockstate, normallyposts0x08 through0x3014dec; callbacksource initially0x180008a4"),
    ("candidate_ptt_press_callback", 0x03014e4c, 0x03014f46, "direct instructions + initial callback table",
     "Normal path calls0x30149fc(0), checks admission, sets statebit7 andposts0x04; servicecalibration branches maywrite storage"),
    ("modem_state_event_set", 0x0302150c, 0x03021548, "direct instructions; downstream worker not fully audited",
     "Sets uint8 state0x3bd3e and flags, posts event0x20 via0x300d0a4; not the physical test selector and not a direct PA instruction"),
    ("common_irq_entry", 0x1064a, 0x106b4, "direct instructions",
     "Saves68bytes, switchesIRQstack0x4b7b0, dispatches table0x45514[(PSRvector-32)], restores and rte"),
    ("pcm_pointer_init", 0x14164, 0x1417e, "direct instructions + manual pp269-270",
     "After0x16bdc stores0x180000a0 at0x33c4c and0x18000000 at0x33c50"),
    ("tx_level_ramp", 0x2b364, 0x2b42e, "direct instructions + traced DAC consumer",
     "Mutex0x3a4c4; ramp/direct uint8 targetr2 through0x2c3b4; r0 controls ramp andr1 selects up/down"),
    ("rf_tx_switch", 0x2b440, 0x2b49e, "direct instructions",
     "Disables RXband, waits, tunes TXword0x38468, enables TXband, ramps calibrated DACB up, sets TXflag0x3e1c0 bit0"),
    ("rf_rx_restore", 0x2b4a0, 0x2b4fc, "direct instructions",
     "IfTXflag set waits, ramps DACB tozero, disables TXband, clears flag, tunesRXword0x3846c and enables RXband"),
    ("rfic_tuning_word_write", 0x2b56c, 0x2b584, "direct instructions",
     "Writes low16 r1 toRFICreg0x38 and high16 r0 toreg0x39 via3wirewriter"),
    ("rfic_tx_mode", 0x2b5b8, 0x2b5e4, "direct instructions + TX caller",
     "Writes RFIC37=1d0f 52=028f then30=0,c1fe"),
    ("rfic_rx_mode", 0x2b5e4, 0x2b620, "direct instructions + RX caller",
     "Writes RFIC37=1f1f or1f0f according to mode then30=0,bff1"),
    ("rfic_tx_tune", 0x2b674, 0x2b69e, "direct instructions + mode writer trace",
     "Writes32bit tuning wordr0 through0x2b56c then selectsRFIC TXmode; GPIO interrupt andmode state sideeffects"),
    ("rfic_rx_tune", 0x2b6a0, 0x2b7de, "direct instructions + mode writer trace",
     "Writes32bit tuning wordr0 through0x2b56c then configures mode dependent RX/interrupt/subtone andRFIC RXmode"),
    ("rfic_3wire_write", 0x2c2f4, 0x2c33c, "direct instructions; chip/net identity inference",
     "Mutex0x3a49c; GPIOA24CS A7clock A8bidirectionaldata; emits8bit registerr0 and16bit valuer1 MSBfirst"),
    ("rfic_3wire_read", 0x2c33c, 0x2c384, "direct instructions; not sideeffect free",
     "AssertsGPIOCS, sends uint8registerr0|0x80 and receives16bits; changes GPIO direction andmutex state"),
    ("tx_level_dac_write", 0x2c3b4, 0x2c3c4, "direct instructions + manual p243",
     "Writes uint8r0<<4 through0x2c388 channel2 toDAC_DATA_B at0x140f000c; physical PA connection not traced"),
    ("rf_rx_irq", 0x2c3d4, 0x2c400, "direct instructions + vector65 registration",
     "ACKs RFRXbit3 at0x110003b0; unlessdisabled sets0x38465=2 andsignals0x38590"),
    ("rf_tx_irq", 0x2c400, 0x2c42c, "direct instructions + vector64 registration",
     "ACKs RFTXbit2 at0x110003b0; unlessdisabled sets0x38464=1 andsignals0x38590"),
    ("gpio_output_high", 0x2c500, 0x2c544, "direct instructions + manual pp218-219",
     "RMW GPIO data register; pin0..31 A14020000,32..63 B14100000,64.. C14110000"),
    ("gpio_output_low", 0x2c544, 0x2c588, "direct instructions + manual pp218-219",
     "RMW GPIO data register clearing indexed bit withsame pinbank convention"),
    ("rf_tx_band_enable", 0x2c838, 0x2c878, "direct instructions + bandselector trace",
     "Enable forTXbandbit4=1: B23low B3high; bit4=0: B3low B23high; disable clearsboth"),
    ("rf_rx_band_enable", 0x2c878, 0x2c8b8, "direct instructions + bandselector trace",
     "Enable forRXbandbit3=1: B2low B4high; bit3=0: B4low B2high; disable clearsboth"),
    ("startup_clock_config", 0x2c9b4, 0x2ca56, "direct instructions + manual pp61-64; not measured",
     "BPLL low24=040712 busdivlow4=2; softwareclockglobals168M168M84M24M; upper PLL flags preserved andfraction notwritten"),
    ("capture_pcm_enable", 0x16088, 0x160ae, "direct instructions + manual p96",
     "Setscaptureactiveflag, clearsPCM_WR IRQ bit4 at0x110003b0 and enablesAUDIO_CONTROL bit4"),
    ("play_pcm_enable", 0x160c0, 0x160e6, "direct instructions + manual p96",
     "Setsplayactiveflag, clearsPCM_RD IRQ bit5 at0x110003b0 and enablesAUDIO_CONTROL bit5"),
    ("system_irq_collect", 0x2c454, 0x2c49a, "direct instructions",
     "AccumulatesSYS_INTERP_LIST0x11000398, clears0x110003a0, snapshotsRX_TYPE_INFO390 andRX_INFO394 then signalsworker"),
    ("capture_pcm_irq_wrapper", 0x2c4a4, 0x2c4ac, "direct instructions + registration",
     "Vector60 wrapper calls0x2e1d4"),
    ("play_pcm_irq_wrapper", 0x2c49c, 0x2c4a4, "direct instructions + registration",
     "Vector59 wrapper calls0x2e238"),
    ("capture_pcm_irq", 0x2e1d4, 0x2e236, "direct instructions + manual p269",
     "AcknowledgesPCM_WR, copies80 halfwords frompointer0x33c50 into160-sample ring, advances index80 modulo160, signalsworker"),
    ("play_pcm_irq", 0x2e238, 0x2e2f6, "direct instructions + manual p270",
     "AcknowledgesPCM_RD; writes80 halfwords to pointer0x33c4c fromaudioqueue or fallbackbuffer; underflow behavior requires runtimecheck"),
    ("capture_pcm_ack", 0x2e35c, 0x2e36c, "direct instructions + manual p105",
     "RMWbit4 thenwrites0x110003b0"),
    ("play_pcm_ack", 0x2e36c, 0x2e37c, "direct instructions + manual p105",
     "RMWbit5 thenwrites0x110003b0"),
]


def main():
    image = (ROOT / "research/originals/DM32.01.01.049.bin").read_bytes()
    if hashlib.sha256(image).hexdigest() != EXPECTED:
        raise SystemExit("Refuse mapping: expected complete pinned image hash")
    out = ROOT / "research/modem/code-evidence"
    out.mkdir(parents=True, exist_ok=True)
    lines = {}
    for name in ("flash", "iram"):
        lines[name] = (ROOT / f"artifacts/map-049/{name}.asm").read_text().splitlines()
    with (ROOT / "docs/modem-map.csv").open("w", newline="") as dest:
        writer = csv.DictWriter(dest, lineterminator="\n", fieldnames=["image_sha256", "name", "file_offset",
                                 "runtime_start", "runtime_end_exclusive", "length",
                                 "first16_hex", "range_sha256", "confidence", "evidence"])
        writer.writeheader()
        for name, start, end, confidence, evidence in PATHS:
            segment = "iram" if start < 0x3000000 else "flash"
            offset = 0xb3ca0 + start - 0x10000 if segment == "iram" else 0x100 + start - 0x0300c000
            data = image[offset:offset + end - start]
            if len(data) != end - start:
                raise SystemExit("Anchor extends beyond pinned image")
            writer.writerow(dict(image_sha256=EXPECTED, name=name, file_offset=hex(offset),
                                 runtime_start=hex(start), runtime_end_exclusive=hex(end),
                                 length=len(data), first16_hex=data[:16].hex(),
                                 range_sha256=hashlib.sha256(data).hexdigest(),
                                 confidence=confidence, evidence=evidence))
            kept = []
            for line in lines[segment]:
                m = re.match(r"\s*([0-9a-f]+):", line)
                if m and start <= int(m[1], 16) < end:
                    kept.append(line.split("\t//")[0])
            (out / (name + ".asm")).write_text("\n".join(kept) + "\n")
    print(f"Mapped {len(PATHS)} exact-image anchors; hardware execution unverified")


if __name__ == "__main__":
    main()
