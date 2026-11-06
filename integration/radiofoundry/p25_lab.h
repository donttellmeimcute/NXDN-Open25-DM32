/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef P25_LAB_H
#define P25_LAB_H
#include <stddef.h>
#include <stdint.h>
void p25_lab_start(void);
int p25_lab_busy(void);
size_t p25_lab_command(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap);
#if defined(CONFIG_DEV_TX) && CONFIG_DEV_TX
int p25_lab_live_begin(uint32_t session);
int p25_lab_channel_rx(uint32_t frequency_hz);
void p25_lab_live_up(void);
#endif
#endif
