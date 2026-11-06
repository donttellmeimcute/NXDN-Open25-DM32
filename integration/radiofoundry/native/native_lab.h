/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NATIVE_LAB_H
#define NATIVE_LAB_H
#include "radio/radio_sm.h"
#include "native_test.h"
void native_lab_init(void);
void native_lab_service(radio_sm_t *, uint32_t now_us);
int native_lab_reserved(void);
/* Radio-task wait cap: 0 unreserved, 2 ms raw bench, 5 ms internal RX audio.
 * Hardware capture/lease interrupts remain independent of this task wait. */
uint32_t native_lab_wait_ms(void);
int native_lab_owns(void);
/* On-device P25 worker only. RX fixed bounds, no TX token or persistent writes.
 * The radio task still performs all prepare/restore operations. */
uint32_t native_lab_voice_start(uint32_t frequency_hz,uint32_t duration_ms);
int native_lab_voice_poll(uint32_t session,uint8_t *state,uint8_t *reason);
int native_lab_voice_read(uint32_t session,nt_record *record);
void native_lab_voice_stop(uint32_t session);
int native_lab_voice_release(uint32_t session);
/* Ordinary radio requests interrupt a test. Internal RX-audio consumes PTT
 * edges (including a shared programming-jack transition) without aborting;
 * no PTT request may turn an interrupted test into normal FM transmit. */
int native_lab_request(radio_sm_t *, const radio_req_t *);
size_t native_lab_command(const uint8_t *, uint16_t, uint8_t *, size_t);
/* Local mic/PTT owner. Cable ARM cannot select this audio mode. */
#define NATIVE_PHYSICAL_PTT_DOWN 3
#define NATIVE_PHYSICAL_PTT_UP 4
#if defined(CONFIG_DEV_TX) && CONFIG_DEV_TX
int native_lab_live_poll(uint32_t session, uint8_t *state, uint8_t *reason, uint8_t *queued);
int native_lab_live_block(uint32_t session, const uint8_t raw[36], int last);
int native_lab_live_go(uint32_t session);
void native_lab_live_summary(uint32_t out[5]);
#endif
#endif
