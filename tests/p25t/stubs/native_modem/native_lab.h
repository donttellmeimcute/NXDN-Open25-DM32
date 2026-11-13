/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host stand-in for integration/radiofoundry/native/native_lab.h: the channel calls radio_p25t.c makes */
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    uint32_t rx_hz, tx_hz;
    uint8_t power;
} native_channel;
typedef struct {
    bool enabled, tx_pending, transmitting, keyed, receiving;
    uint8_t reason;
    uint32_t calls, blocks, faults;
    bool inbound;
    uint32_t inbound_sent;
} native_channel_state;
int native_lab_channel(const native_channel *);
int native_lab_channel_ptt(bool down);
void native_lab_channel_state(native_channel_state *);
int native_lab_channel_inbound(void);
