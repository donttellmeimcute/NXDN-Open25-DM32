/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The firmware's NXDN48 receive chain on the host, for tools/nxdn_validate.py:
 * another transmitter's waveform -> the FM channel model -> the 8 kHz
 * software demodulator -> the call layer -> the AMBE FEC.
 *
 *   nxdn_rx_air IN.f64 OUT.ambe CNR_DB OFFSET_HZ RAN GROUP
 *
 * IN.f64: the deviation in Hz at 48 kHz (doubles). OUT.ambe: the 49-bit
 * parameters of every voice frame played, as nxdn_tx_air writes them.
 * stdout: JSON with what the receiver heard. */
#include "nxdn_call.h"
#include "nxdn_modem.h"
#include "nxdn_channel.h"
#include "dmr/ambe/ambeFec.h"
#include <stdio.h>
#include <stdlib.h>

enum { MAX48 = 48000 * 30 };

static double dev48[MAX48];
static int16_t audio[MAX48 / 6 + 8000];
static uint8_t dibits[MAX48 / 6];
static FILE *ambe;
static unsigned played;

static void sink(void *ctx, const uint8_t slot[9])
{
    (void)ctx;
    uint8_t params[49];
    ambe_fec_result_t r;
    ambeFecDecode(slot, params, &r);
    unsigned long long v = 0;
    for (unsigned i = 0; i < 49; i++) v = v << 1 | (params[i] & 1u);
    fprintf(ambe, "%014llX\n", v << 7);
    played++;
}

int main(int argc, char **argv)
{
    if (argc != 7) {
        fprintf(stderr, "usage: %s IN.f64 OUT.ambe CNR_DB OFFSET_HZ RAN GROUP\n", argv[0]);
        return 2;
    }
    FILE *in = fopen(argv[1], "rb");
    ambe = fopen(argv[2], "w");
    if (!in || !ambe) return 1;
    size_t n48 = fread(dev48, sizeof dev48[0], MAX48, in);
    fclose(in);
    nxdn_channel_cfg c = { 0 };
    c.cnr_db = atof(argv[3]);
    c.offset_hz = atof(argv[4]);
    c.dev_scale = 1.0;
    c.rx_gain = 8.0;
    c.lead_s = 0.1;
    c.seed = 7;
    size_t n8 = nxdn_channel_run48(&c, dev48, n48, audio);
    nxdn_demod d;
    nxdn_demod_init(&d);
    /* the receiver idles on noise; the squelch opens 5 ms into the carrier */
    size_t open = (size_t)(c.lead_s * 8000) + 40;
    size_t nd = nxdn_demod_samples(&d, audio, open, dibits, sizeof dibits);
    nxdn_demod_reacquire(&d);
    nd += nxdn_demod_samples(&d, audio + open, n8 - open, dibits + nd, sizeof dibits - nd);
    nxdn_rx rx;
    nxdn_rx_init(&rx, (uint8_t)atoi(argv[5]), (uint16_t)atoi(argv[6]), 0, sink, NULL);
    for (size_t i = 0; i < nd; i++) nxdn_rx_push_dibit(&rx, dibits[i]);
    fclose(ambe);
    printf("{\"frames\": %u, \"calls\": %u, \"releases\": %u, \"late_entries\": %u, \"ran\": %u, \"source\": %u, "
           "\"destination\": %u, \"group_call\": %s, \"encrypted\": %s, \"bad_lich\": %u, \"bad_sacch\": %u, "
           "\"false_syncs\": %u, \"sync_losses\": %u, \"slots_played\": %u}\n",
           rx.frames, rx.calls, rx.releases, rx.late_entries, rx.last_ran, rx.msg.source, rx.msg.destination,
           rx.msg.call_type == NXDN_CALL_GROUP ? "true" : "false", rx.msg.cipher_type ? "true" : "false", rx.bad_lich,
           rx.bad_sacch, rx.false_syncs, rx.sync_losses, played);
    return 0;
}
