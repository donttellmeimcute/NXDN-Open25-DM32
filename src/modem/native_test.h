/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NATIVE_TEST_H
#define NATIVE_TEST_H
#include <stdint.h>
#include <stddef.h>

/* Native 4800-symbol/s PHY experiment. No software modulation or codec here.
 * Task calls other than begin/cleanup must exclude modem/timer interrupts.
 * begin locks its claim/publication but runs slow board setup unlocked;
 * cleanup is serialized by the owner task. ISR calls are bounded.
 * start/restore are task-only, quiet/key/cancel/write are ISR-safe. A failed
 * start may have touched hardware: quiet then restore are mandatory. No backend
 * callback may synchronously reenter this controller, but start can be
 * preempted by STOP. See docs/native-modem-tests.md. */
enum { NT_BYTES=36, NT_RX_CAP=16, NT_TX_CAP=8, NT_MAX_BLOCKS=60,
       NT_MAX_MS=2000, NT_MIN_LEASE_MS=100, NT_MAX_LEASE_MS=500,
       NT_SLOT_US=30000, NT_START_US=100000, NT_STALL_US=60000,
       NT_DRAIN_US=45000, NT_STATUS_WORDS=37, NT_SNAPSHOT_WORDS=56,
       /* The TX-slot vector follows its slot's PHY_RDY by about 0.44 ms.
        * Later than this, that PHY_RDY (and its handoff) was lost. */
       NT_TX_SLOT_LAG_US=20000 };
#if defined(CONFIG_DEV_TX) && CONFIG_DEV_TX
enum { NT_TX_STORAGE_CAP=28, NT_TX_PRELOAD_MS=1000 };
#else
enum { NT_TX_STORAGE_CAP=NT_TX_CAP, NT_TX_PRELOAD_MS=1000 };
#endif
enum { NT_IDLE, NT_ARMED, NT_RX, NT_TX, NT_DRAIN, NT_STOPPED, NT_FAULT };
enum { NT_RECEIVE=1, NT_TRANSMIT=2 };
enum { NT_OK, NT_USER_STOP, NT_RX_LIMIT, NT_TX_COMPLETE, NT_RX_DURATION,
       NT_LEASE, NT_DEADLINE, NT_STALLED, NT_LATE, NT_EARLY, NT_UNDERRUN,
       NT_IO, NT_RX_OVERFLOW, NT_ABNORMAL, NT_DENIED, NT_INTERRUPTED,
       NT_RESTORE, NT_UNEXPECTED_RF };
/* Interrupt sources. Only NT_IRQ_PHY hands TX blocks to the modem; the slot
 * vectors are supervision (docs/native-modem-tests.md, TX handoff). */
enum { NT_IRQ_PHY, NT_IRQ_TX_SLOT, NT_IRQ_RX_SLOT };
typedef struct nt_config {
    uint32_t frequency_hz;
    uint32_t blocks;
    uint32_t duration_ms;
    uint16_t lease_ms;
    uint8_t mode, p25_sync, audio;
    uint8_t power; /* board power level for TX */
} nt_config;
static inline int nt_tx_preloaded(const nt_config *c)
{
#if defined(CONFIG_DEV_TX) && CONFIG_DEV_TX
    return c->mode==NT_TRANSMIT &&
           c->duration_ms==NT_TX_PRELOAD_MS && c->lease_ms==NT_TX_PRELOAD_MS &&
           c->blocks<=NT_TX_STORAGE_CAP;
#else
    (void)c;
    return 0;
#endif
}
static inline unsigned nt_tx_required(const nt_config *c)
{
    if (nt_tx_preloaded(c)) return c->blocks;
    return c->blocks==1 ? 1 : 2;
}
static inline int nt_tx_live(const nt_config *c)
{
    return c->mode==NT_TRANSMIT && c->audio;
}
typedef struct {
    uint32_t sequence, time_us, sys_flags, rx_info;
    uint8_t raw[NT_BYTES];
} nt_record;
typedef struct { uint32_t index; uint8_t raw[NT_BYTES]; } nt_block;
typedef struct {
    uint32_t rx_seen, rx_saved, rx_dropped, rx_read, tx_loaded, tx_sent;
    uint32_t phy_irqs, tx_slot_irqs, rx_slot_irqs, rf_irqs, abnormal_irqs;
    uint32_t duplicate_irqs, ignored_irqs, pa_keys, quiets, faults, rejected;
    uint32_t max_gap_us, last_gap_us, restore_failures;
} nt_stats;
struct nt_test;
typedef struct nt_ops {
    void *ctx;
    int (*start)(void *, struct nt_test *);
    int (*write)(void *, const uint8_t[NT_BYTES]);
    void (*cancel_send)(void *);
    void (*request_key)(void *);
    void (*pa_on)(void *);
    void (*quiet)(void *);
    int (*restore)(void *);
    uint32_t (*lock)(void *); /* paired IRQ/scheduler exclusion; optional in host mocks */
    void (*unlock)(void *, uint32_t);
    size_t (*snapshot)(void *,uint32_t *,size_t); /* fixed read-only PHY diagnostics */
} nt_ops;
typedef struct nt_test {
    const nt_ops *ops;
    nt_config config;
    nt_stats stats;
    uint32_t session, generation, started_us, ping_us, boundary_us, drain_us;
    nt_record rx[NT_RX_CAP];
    nt_block tx[NT_TX_STORAGE_CAP];
    uint8_t state, reason, owned, boundary_seen, key_requested, keyed, canceled, boundary_source;
    uint8_t rx_head, rx_tail, rx_count, tx_head, tx_tail, tx_count;
} nt_test;
void nt_init(nt_test *, const nt_ops *);
int nt_arm(nt_test *, const nt_config *, int allow_rf);
/* Internal receive-only audio session: up to 300 s, 10000 records; same lease,
 * ring overflow and fault handling. Not exposed through ordinary ARM. */
int nt_arm_voice(nt_test *, uint32_t frequency_hz, uint32_t duration_ms);
/* The radio core's channel: open-ended TX (sealed when the call ends) and
 * RX sessions with a 250/500 ms lease the worker renews. */
int nt_arm_live(nt_test *, uint32_t frequency_hz, uint8_t power);
int nt_arm_live_rx(nt_test *, uint32_t frequency_hz);
int nt_live_seal(nt_test *, uint32_t session);
#if defined(CONFIG_DEV_TX) && CONFIG_DEV_TX
enum { NT_TRACE_BLOCKS=192, NT_TRACE_WORDS=26, NT_TRACE_BLOCK_BYTES=44 };
/* Completed physical PTT call, retained across RX re-arm. Caller excludes IRQs. */
void nt_trace_summary(uint32_t out[NT_TRACE_WORDS]);
size_t nt_trace_read(uint32_t session, unsigned first, uint8_t *out, size_t cap);
void nt_trace_irq_setup(const nt_test *, uint32_t mask, uint32_t before, uint32_t after);
void nt_trace_irq_quiet(const nt_test *, uint32_t seen);
void nt_trace_stage(const nt_test *, uint32_t now_us);
#endif
int nt_load(nt_test *, uint32_t session, uint32_t index, const uint8_t[NT_BYTES]);
int nt_begin(nt_test *, uint32_t session, uint32_t now_us);
int nt_ping(nt_test *, uint32_t session, uint32_t now_us);
int nt_stop(nt_test *, uint32_t session, uint8_t reason);
void nt_deny(nt_test *);
void nt_fault(nt_test *, uint8_t reason);
int nt_cleanup(nt_test *); /* task only, returns 1 when ownership released */
int nt_reset(nt_test *);   /* fault acknowledgment; refuses ownership/unread capture */
int nt_pop(nt_test *, uint32_t session, nt_record *);
void nt_tick(nt_test *, uint32_t now_us);
void nt_boundary(nt_test *, uint8_t source, uint32_t now_us);
void nt_rx(nt_test *, uint32_t now_us, uint32_t flags, uint32_t info, const uint8_t[NT_BYTES]);
void nt_abnormal(nt_test *);
void nt_rf_tx(nt_test *, uint32_t now_us);
void nt_pa_ready(nt_test *, uint32_t now_us);
void nt_status(const nt_test *, uint32_t out[NT_STATUS_WORDS]);
#endif
