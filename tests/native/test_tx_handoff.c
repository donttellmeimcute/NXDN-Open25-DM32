/* SPDX-License-Identifier: GPL-3.0-or-later */
/* TX RAM handoff against a timing model of the 2026-11-06 traced calls.
 *
 * The raw traces stay on the bench PC; PROGRESS.md (2026-11-06) records what
 * they showed, and this model is built from those numbers:
 *  - PHY_RDY is the first interrupt of every slot; the TX-slot vector follows
 *    0.43-0.45 ms later (both traced calls).
 *  - The modem takes the first three bytes of the coming slot's TX RAM before
 *    that TX-slot vector, and the rest after it: writing on the TX-slot vector
 *    sent the previous block's first three bytes and the current block's other
 *    33 (96.8% and 94.9% of head symbols matched the previous block on air).
 *  - SEND_DATA0 and SEND_DATA1 alternate (HR_C7000 guide Table 46).
 *  - The first RF-start interrupt follows the first handoff; the PA callback
 *    about 28.4 ms after it.
 * The model checks both the old write point (reproducing the failure, and the
 * substitution of the three intended bytes that recovered every block) and the
 * real driver and controller, which must put every block on air byte-exact. */
#include "native_test.h"
#include "hrc_native_chip.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { HEAD = 3 };

typedef struct {
	uint32_t phy_us;        /* PHY_RDY of the first slot */
	uint32_t slot_lag_us;   /* TX-slot vector after PHY_RDY */
	uint32_t head_fetch_us; /* head bytes taken after PHY_RDY */
	uint32_t tail_fetch_us; /* the other 33 bytes taken after PHY_RDY */
	uint32_t isr_us;        /* interrupt entry latency */
	int32_t jitter_us;      /* +/- per-slot PHY_RDY jitter */
	uint8_t alternate;      /* 1: SEND_DATA0/1 alternate, 0: always SEND_DATA0 */
} model_t;

static uint32_t regs[0x600 / 4];
static uint8_t buf[0x1000];
static hrc7000_t dev = { .regs = regs, .buf = buf };
static hn_chip chip;
static nt_ops hw;
static nt_test t;
static unsigned keys, pa_ons, offs;
static uint8_t intended[256][36];
static uint8_t air[256][36];
static unsigned air_count;

static int prepare(void *p, const nt_config *c) { (void)p; (void)c; return 1; }
static void fast_off(void *p) { (void)p; offs++; }
static void key_request(void *p) { (void)p; keys++; }
static void pa_on(void *p) { (void)p; pa_ons++; }
static int irq_on(void *p, nt_test *v) { (void)p; (void)v; return 1; }
static void noop(void *p) { (void)p; }
static int restore(void *p) { (void)p; return 1; }
static const hn_hooks hooks = { NULL, prepare, fast_off, key_request, pa_on, irq_on, noop, noop, restore, NULL, NULL };

static uint32_t rnd = 0x2611060u;
static uint32_t xorshift(void) { rnd ^= rnd << 13; rnd ^= rnd >> 17; rnd ^= rnd << 5; return rnd; }

static void make_blocks(unsigned n)
{
	for (unsigned i = 0; i < n; i++)
		for (unsigned j = 0; j < 36; j++) intended[i][j] = (uint8_t)xorshift();
}

/* One slot of the modem: optionally a write by the firmware at write_us
 * (relative to PHY_RDY), the head and tail fetches from the active half.
 * A slot whose send bit was not set before the tail fetch sends nothing. */
typedef void (*isr_fn)(uint32_t now);

static void fetch(const model_t *m, unsigned slot, uint8_t *dest, unsigned from, unsigned to)
{
	unsigned half = m->alternate ? (slot & 1u) : 0u;
	memcpy(dest + from, &buf[half * 36 + from], to - from);
}

/* The old driver: copy both halves and set the send bit on the TX-slot vector. */
static void legacy_write(unsigned block)
{
	for (unsigned i = 0; i < 36; i++) { buf[i] = intended[block][i]; buf[36 + i] = intended[block][i]; }
	regs[C7_LAYER2_TXRX_CTRL / 4] = 0x80;
}

static void run_legacy(const model_t *m, unsigned n)
{
	memset(buf, 0xcc, sizeof buf);
	air_count = 0;
	for (unsigned slot = 0; slot < n; slot++) {
		uint32_t phy = m->phy_us + slot * 30000u;
		uint8_t out[36];
		/* the modem takes the head before the TX-slot vector arrives */
		assert(m->head_fetch_us < m->slot_lag_us);
		fetch(m, slot, out, 0, HEAD);
		legacy_write(slot);
		assert(m->tail_fetch_us > m->slot_lag_us + m->isr_us);
		fetch(m, slot, out, HEAD, 36);
		(void)phy;
		memcpy(air[air_count++], out, 36);
	}
}

static void sys_irq(uint32_t now)
{
	regs[C7_SYS_INTERP_LIST / 4] = 1u << 12;
	hn_sys(&chip, now);
}

static unsigned run_driver(const model_t *m, unsigned n, unsigned lose_phy_at)
{
	memset(buf, 0xcc, sizeof buf);
	memset(regs, 0, sizeof regs);
	air_count = 0;
	keys = pa_ons = offs = 0;
	hn_init(&chip, &dev, &hooks, &hw);
	nt_init(&t, &hw);
	nt_config c = { 145500000, n, 2000 + n * 30, 250, NT_TRANSMIT, 1, 0, 0 };
	if (c.duration_ms > NT_MAX_MS) c.duration_ms = NT_MAX_MS;
	assert(nt_arm(&t, &c, 1));
	unsigned loaded = 0;
	while (loaded < n && loaded < NT_TX_CAP) assert(nt_load(&t, t.session, loaded, intended[loaded])), loaded++;
	uint32_t start = m->phy_us - 3330u;
	assert(nt_begin(&t, t.session, start));
	/* Separate TX start: no block and no send bit before the first PHY_RDY. */
	assert(buf[0] == 0xcc && buf[36] == 0xcc && !(regs[C7_LAYER2_TXRX_CTRL / 4] & 0x80u));
	unsigned sent_slots = 0;
	for (unsigned slot = 0; slot < n + 2 && t.state != NT_FAULT; slot++) {
		int32_t j = m->jitter_us ? (int32_t)(xorshift() % (2u * (uint32_t)m->jitter_us + 1u)) - m->jitter_us : 0;
		uint32_t phy = m->phy_us + slot * 30000u + (uint32_t)j;
		if (!nt_ping(&t, t.session, phy)) {
			assert(t.state == NT_STOPPED && t.reason == NT_TX_COMPLETE); /* drained */
			break;
		}
		regs[C7_LAYER2_TXRX_CTRL / 4] &= ~0x80u; /* the modem consumed last slot's send bit */
		uint8_t out[36];
		/* events in time order: the interrupt entry and the head fetch */
		if (m->isr_us < m->head_fetch_us) {
			if (slot != lose_phy_at) sys_irq(phy + m->isr_us);
			fetch(m, slot, out, 0, HEAD);
		} else {
			fetch(m, slot, out, 0, HEAD);
			if (slot != lose_phy_at) sys_irq(phy + m->isr_us);
		}
		hn_slot(&chip, NT_IRQ_TX_SLOT, phy + m->slot_lag_us);
		if (slot == 0 && t.state == NT_TX) {
			hn_rftx(&chip, phy + 3330u);          /* first RF start after the first handoff */
			nt_pa_ready(&t, phy + 3330u + 28400u); /* board timer callback */
		}
		int send = (regs[C7_LAYER2_TXRX_CTRL / 4] & 0x80u) != 0;
		fetch(m, slot, out, HEAD, 36);
		if (send && t.state != NT_FAULT) { memcpy(air[air_count++], out, 36); sent_slots++; }
		while (t.state == NT_TX && loaded < n && t.tx_count < NT_TX_CAP) assert(nt_load(&t, t.session, loaded, intended[loaded])), loaded++;
	}
	return sent_slots;
}

static unsigned head_matches(unsigned block, unsigned other)
{
	unsigned same = 0;
	for (unsigned i = 0; i < HEAD; i++)
		for (unsigned b = 0; b < 8; b += 2)
			same += ((air[block][i] >> b) & 3u) == ((intended[other][i] >> b) & 3u);
	return same; /* of 12 symbols */
}

static void reproduce_trace(const model_t *m)
{
	unsigned n = 192; /* the trace window: 192 handoffs, 5.76 s */
	make_blocks(n);
	run_legacy(m, n);
	unsigned prev_symbols = 0, cur_symbols = 0, total = 0, tails = 0, recovered = 0;
	for (unsigned k = 1; k < n; k++) {
		prev_symbols += head_matches(k, k - 1);
		cur_symbols += head_matches(k, k);
		total += 12;
		tails += !memcmp(air[k] + HEAD, intended[k] + HEAD, 36 - HEAD);
		uint8_t fixed[36];
		memcpy(fixed, air[k], 36);
		memcpy(fixed, intended[k], HEAD); /* the trace analysis' substitution */
		recovered += !memcmp(fixed, intended[k], 36);
	}
	/* Every head symbol tracks the previous block (the radio's own traces,
	 * through demodulation and bit errors: 96.8% and 94.9%); a random block
	 * agrees with the intended head about a quarter of the time (24.5%, 23.2%). */
	assert(prev_symbols == total);
	assert(cur_symbols * 100u > total * 15u && cur_symbols * 100u < total * 35u);
	assert(tails == n - 1 && recovered == n - 1);
}

static void driver_exact(const model_t *m)
{
	unsigned n = 60;
	make_blocks(n);
	unsigned sent = run_driver(m, n, ~0u);
	assert(sent == n && air_count == n);
	for (unsigned k = 0; k < n; k++) assert(!memcmp(air[k], intended[k], 36));
	assert(t.stats.tx_sent == n && keys == 1 && pa_ons == 1);
	assert(t.state == NT_DRAIN || t.state == NT_STOPPED);
}

int main(void)
{
	/* Timing seen on the radio: TX-slot 0.43-0.45 ms after PHY_RDY. Head and
	 * tail fetch times bracket that vector; ISR latency is tens of us. */
	const model_t traced[] = {
		{ 34006, 430, 200, 900, 12, 0, 1 },
		{ 34006, 450, 400, 2500, 40, 0, 1 },
		{ 34006, 440, 60, 1200, 5, 0, 0 },
	};
	for (unsigned i = 0; i < sizeof traced / sizeof *traced; i++) reproduce_trace(&traced[i]);
	puts("PASS old TX-slot write point reproduces the 2026-11-06 head-of-block lag; three-byte substitution recovers every block");

	for (unsigned i = 0; i < sizeof traced / sizeof *traced; i++) driver_exact(&traced[i]);
	/* PHY_RDY jitter of +/-300 us and both half policies */
	model_t jitter = { 34006, 440, 200, 1500, 20, 300, 1 };
	driver_exact(&jitter);
	jitter.alternate = 0;
	driver_exact(&jitter);
	puts("PASS PHY_RDY handoff: every block byte-exact on air with alternating or fixed halves and +/-300 us jitter");

	/* A lost PHY_RDY stops the run at that slot's TX vector: no mixed block
	 * reaches the air, and the PA is commanded off. */
	make_blocks(30);
	model_t m = traced[0];
	run_driver(&m, 30, 10);
	assert(t.state == NT_FAULT && t.reason == NT_LATE && t.stats.tx_sent == 10 && offs == 1);
	for (unsigned k = 0; k < air_count; k++) assert(!memcmp(air[k], intended[k], 36));
	/* Sensitivity: an interrupt entered after the head fetch is visible here. */
	m.isr_us = 300; m.head_fetch_us = 100;
	make_blocks(20);
	run_driver(&m, 20, ~0u);
	unsigned bad = 0;
	for (unsigned k = 1; k < air_count; k++) bad += memcmp(air[k], intended[k], 36) != 0;
	assert(bad == air_count - 1);
	puts("PASS lost PHY_RDY faults before a mixed block; a late ISR is detected by the model");
	return 0;
}
