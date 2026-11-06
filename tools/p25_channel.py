#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Select the development P25 channel or inspect its last local PTT call."""
import argparse
import json
from pathlib import Path
import struct
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools import native_modem, normal_reboot


def tx_trace(request):
    fields = (
        'version session complete start_us count reason sent pa_keys rf_irqs '
        'first_rf_us first_rf_state first_rf_sent request_us first_pa_us pa_callbacks '
        'boundary_mask first_phy_us first_tx_slot_us first_rx_slot_us '
        'boundary_source pic_mask_1 pic_pending_1'
    ).split()
    _, header = request(0x21, b'\x0b')
    words = struct.unpack(f'<{len(header)//4}I', header)
    if words and words[0] == 2:
        fields += ['pic_after_clear_1', 'pic_at_quiet_1', 'first_stage_us', 'staged']
    if len(words) != len(fields) or words[0] not in (1, 2):
        raise RuntimeError('Unexpected TX trace header')
    summary = dict(zip(fields, words))
    if summary['count'] > 192:
        raise RuntimeError('Unexpected TX trace header')
    blocks = []
    if summary['complete']:
        while len(blocks) < summary['count']:
            payload = struct.pack('<BIH', 11, summary['session'], len(blocks))
            _, page = request(0x21, payload)
            if not page or len(page) % 44 or len(blocks) + len(page)//44 > summary['count']:
                raise RuntimeError('Unexpected TX trace page')
            for timestamp, queued, raw in struct.iter_unpack('<II36s', page):
                blocks.append({'us': timestamp, 'queued': queued, 'raw_hex': raw.hex()})
        if request(0x21, b'\x0b')[1] != header:
            raise RuntimeError('A different call replaced the TX trace')
    return {'summary': summary, 'blocks': blocks}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('action', choices=('enable', 'disable', 'status', 'benchmark', 'trace'))
    args = parser.parse_args()
    view = ['core', 'link', 'tasks', 'crash', 'watchdog', 'reset']
    allowed = {9: {bytes([i]) for i in (0, 6, 20, 21, 22, 23)},
               0x20: {b'\x00', b'\x01', b'\x0d'},
               0x21: {b'\x00', b'\x0a\x00', b'\x0a\x01'}}
    with normal_reboot.open_link(args.port) as link:
        request = type(link).__mro__[1].request

        def checked(cmd, payload=b'', **_):
            trace_read = (args.action == 'trace' and cmd == 0x21 and
                          len(payload) in (1, 7) and payload[0] == 11)
            if payload not in allowed.get(cmd, ()) and not trace_read:
                raise ValueError('Request outside P25 channel operations')
            status, data = request(link, cmd, payload, busy_wait=0)
            if status:
                raise RuntimeError(f'Command {cmd:#x} {payload.hex()} refused ({status})')
            return status, data

        link.request = checked

        def metrics(op):
            _, data = checked(0x20, bytes([op]))
            return dict(line.split('=', 1) for line in data.decode('ascii').splitlines())

        if args.action == 'benchmark':
            checked(0x20, b'\x01')
            end = time.monotonic() + 12
            result = metrics(0)
            while result['p25.state'] == '1' and time.monotonic() < end:
                time.sleep(.25)
                result = metrics(0)
            print(json.dumps(result, indent=2))
            if result['p25.state'] != '2' or result['p25.pcm_crc'] != '4e158373':
                raise RuntimeError('Encoder self-test failed or changed output')
        elif args.action in ('enable', 'disable'):
            checked(0x21, bytes([10, args.action == 'enable']))
        elif args.action == 'trace':
            print(json.dumps(tx_trace(checked), indent=2))
        else:
            native = native_modem.status(link)
            native['state_name'] = native_modem.STATES[native['state']]
            native['reason_name'] = native_modem.REASONS[native['reason']]
            print(json.dumps({'native': native, 'live': metrics(13), 'runtime': link.view(view)}, indent=2))


if __name__ == '__main__':
    main()
