/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Included only in the separate derivative's board radio translation unit
 * (radio_dm32.c, at its end): the flat FM audio paths the NXDN48 modem uses
 * (hal_audio_tap.h AUDIO_TAP_FM_RX_FLAT / AUDIO_TAP_FM_TX_FLAT).
 *
 * Hardware-unverified. What each step relies on, and how to check it on the
 * radio, is in docs/nxdn-modem.md ("What the radio must confirm") and
 * docs/hil-checklist.md.
 *
 * Receive: the FD6818's AF output reaches the HR_C7000's AF input while the
 * speaker listens to the codec (speaker_source with data_listen); the
 * HR_C7000 copies it to RX RAM, 8 kHz (fm_rx_audio.c, modem-hrc7000.md 8.5).
 * The FD6818's AF filter word 0x2B = 0xE000 bypasses its receive high-pass,
 * low-pass and de-emphasis in the BK4819 layout (bk4819.h; the FD6818 is
 * BK4819-class, unverified; RadioFoundry's FM entry writes 0xC000, which
 * leaves the de-emphasis on), and the HR_C7000's FM band-pass is off
 * (FM_BANDWIDTH b6).
 *
 * Transmit: after the board's tx() in FM, the HR_C7000's FM voice source
 * becomes TX RAM (AUDIO_CONTROL b0; zeroed), so the microphone leaves the
 * modulator while its PCM still reaches the CPU for the vocoder (the mic
 * switch as in DMR). Pre-emphasis and band-pass are off (FM_BANDWIDTH b4,
 * b6). The samples ride the prompt FIFO into the modulator, the route APRS's
 * tones take (modem-hrc7000.md 8.4), topped up every 2 ms from the board's
 * timer. */
#if CONFIG_NXDN
#define FD_AF_FILTER		0x2Bu
#define FD_AF_FILTER_FM		0xC000u		/* fd6818.c's FM table */
#define FD_AF_FILTER_FLAT	0xE000u		/* receive high-pass, low-pass and de-emphasis bypassed */
#define C7_FM_BW_BANDPASS	(1u << 6)
#define C7_FM_BW_PREEMPH	(1u << 4)
#define C7_AUDIO_TXRAM_SOURCE	(1u << 0)	/* FM / DMR voice from TX RAM instead of the codec */
#define C7_TXRAM_FM		0x30u		/* TX RAM as the FM voice source: two 512-byte halves */
#define C7_TXRAM_FM_BYTES	1024u
#define FLAT_TICK_US		2000u
/* The APRS measurement on this board (afsk_board above): 2750 LSB through the prompt FIFO gave about 3 kHz peak at
 * 25 kHz. 60 % of a 12.5 kHz channel's peak, 1.5 kHz, is then about 1375 LSB (unmeasured at 12.5 kHz) */
#define FLAT_TX_NOMINAL_PEAK	1375

static audio_tap_pull_t flat_pull;
static int16_t flat_chunk[C7_PROMPT_CHUNK];
static bool flat_pending, flat_irq_set;

/* Timer interrupt: the FIFO topped up while it has room. A chunk the full FIFO refused waits for the next tick */
static void flat_tick(void)
{
	hrc_timer_ack(TIMER_BOARD);
	audio_tap_pull_t pull = flat_pull;
	if (pull == NULL) {
		return;
	}
	for (;;) {
		if (!flat_pending) {
			pull(flat_chunk, C7_PROMPT_CHUNK);
			flat_pending = true;
		}
		if (!hrc7000_prompt_push(&modem, flat_chunk)) {
			return;
		}
		flat_pending = false;
	}
}

static bool flat_tx_inject(audio_tap_pull_t pull, audio_tap_mode_t m)
{
	if (m != AUDIO_TAP_FLAT) {
		return false;
	}
	if (pull != NULL) {
		if (!transmitting || mode != RADIO_MODE_FM || data_tx) {
			return false;
		}
		for (uint32_t i = 0; i < C7_TXRAM_FM_BYTES; i++) {
			modem.buf[C7_TXRAM_FM + i] = 0;
		}
		uint32_t s = hrc_irq_save();	// the AUDIO_CONTROL shadow is shared with the audio task
		hrc7000_audio_bits(&modem, C7_AUDIO_TXRAM_SOURCE, 0);
		hrc_irq_restore(s);
		hrc7000_wr(&modem, C7_FM_BANDWIDTH, hrc7000_rd(&modem, C7_FM_BANDWIDTH) & ~(C7_FM_BW_BANDPASS | C7_FM_BW_PREEMPH));
		hrc_pin_write(PIN_MIC_PATH, false);	// the microphone to the codec, as in DMR (dm32_native_mic_route)
		hrc_audio_pcm_hold(true);		// speaker PCM stays off the FIFO meanwhile
		flat_pending = false;
		flat_pull = pull;
		data_tx = true;
		ring_play();
		if (!flat_irq_set) {
			hrc_irq_register(IRQ_TIMER5, flat_tick);
			hrc_irq_enable(IRQ_TIMER5, true);
			flat_irq_set = true;
		}
		hrc_timer_periodic(TIMER_BOARD, FLAT_TICK_US);
		return true;
	}
	uint32_t s = hrc_irq_save();
	hrc_timer_stop(TIMER_BOARD);
	flat_pull = NULL;
	hrc_irq_restore(s);
	if (data_tx) {
		data_tx = false;
		ring_play();
		hrc_audio_pcm_hold(false);
		s = hrc_irq_save();
		hrc7000_audio_bits(&modem, 0, C7_AUDIO_TXRAM_SOURCE);
		hrc_irq_restore(s);
	}
	return true;
}

static bool flat_rx(bool on)
{
	if (on) {
		if (mode != RADIO_MODE_FM || transmitting || !have_rx || data_listen) {
			return false;
		}
		data_listen = true;
		speaker_source();		// the FD6818's audio stays on the HR_C7000's input
		fd6818_write(&fd, FD_AF_FILTER, FD_AF_FILTER_FLAT);
		hrc7000_wr(&modem, C7_FM_BANDWIDTH, hrc7000_rd(&modem, C7_FM_BANDWIDTH) & ~C7_FM_BW_BANDPASS);
		if (hrc_fm_rx_start()) {
			return true;
		}
	}
	hrc_fm_rx_stop();
	if (data_listen) {
		data_listen = false;
		fd6818_write(&fd, FD_AF_FILTER, FD_AF_FILTER_FM);
		if (mode == RADIO_MODE_FM && !transmitting) {
			hrc7000_fm_rx(&modem, rx_bw == RADIO_BW_25K);	// the band-pass back
		}
		speaker_source();
	}
	return !on;
}

const hal_audio_tap_t dm32_audio_tap = {
	.caps = AUDIO_TAP_FM_TX_FLAT | AUDIO_TAP_FM_RX_FLAT,
	.fm_tx_inject = flat_tx_inject,
	.fm_tx_nominal_peak = FLAT_TX_NOMINAL_PEAK,
	.fm_rx_flat = flat_rx,
	.fm_rx_read = hrc_fm_rx_read,
	/* the receive and transmit shape of the flat paths: not measured yet (docs/nxdn-modem.md) */
	.flat_rx_hpf_hz = 0,
	.flat_tx_hpf_hz = 0,
	.flat_rx_deemph_us = 0,
};
#endif
