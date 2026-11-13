/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef P25_LAB_H
#define P25_LAB_H
#include <stddef.h>
#include <stdint.h>
void p25_lab_start(void);
int p25_lab_busy(void);
size_t p25_lab_command(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap);
#if defined(CONFIG_P25_NATIVE) && CONFIG_P25_NATIVE
#include <stdbool.h>
/* The radio core's P25 channel (radio_dv.c). */
typedef struct {
    uint16_t nac_rx, nac_tx, talkgroup;
    uint32_t unit_id;
    bool hear_all;
} p25_lab_channel;
typedef struct {
    uint32_t last_frame_ms;  /* hal_millis() of the last valid frame, any NAC */
    uint32_t frames, foreign_frames;
    uint32_t source;
    uint16_t nac, talkgroup;
    bool ours;               /* the last frame had our NAC (or the channel hears any) */
    bool playing, encrypted, emergency, ended;
} p25_lab_rx_status;
void p25_lab_channel_config(const p25_lab_channel *);
void p25_lab_rx_status_get(p25_lab_rx_status *);
int p25_lab_live_begin(uint32_t session);
int p25_lab_channel_rx(uint32_t frequency_hz);
void p25_lab_live_up(void);
#endif
#endif
