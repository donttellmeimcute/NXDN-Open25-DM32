#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Enter the approved RAM update helper over the running application's cable.

No firmware/data-array reads, flash writes, RF transmission or arbitrary RAM
payloads. The installer applies its separate exact flash-write plan afterward.
"""
import struct
import time
import zlib
from tools import dm32_readonly_backup as backup
from tools import firstboot_check as check
from tools import native_modem as native
from tools import normal_reboot
MAGIC=0x42533743
BASE=0x44000

def enter(port,serial_port,log=print):
    module=backup.load_upstream();image=module.stub_univ_image()
    # load_upstream verifies the public host source and exact helper SHA-256.
    link=normal_reboot.open_link(port,serial_port)
    parent=type(link).__mro__[1].request
    expected=[]
    def request(cmd,payload=b'',**kw):
        if cmd==0x1a:
            if not expected or payload!=expected.pop(0):raise ValueError('Unapproved RAM-helper transfer operation')
        elif not (cmd in (1,2,9,0x22) or cmd in (0x20,0x21) and payload==b'\x00'):
            raise ValueError('Warm update permits health and approved helper upload only')
        return parent(link,cmd,payload,**kw)
    link.request=request
    restrictions=check.restrictions(link)
    if restrictions['flags'] not in (15,47):raise ValueError('This image has no approved remote-update entry')
    n=native.status(link)
    _,lab=link.request(0x20,b'\x00',busy_wait=0)
    lab=dict(line.split('=',1) for line in lab.decode().splitlines() if '=' in line)
    view=link.view(['core'])
    if n['owned'] or n['state'] in (1,2,3,4) or n['rx_count'] or lab.get('p25.state')=='1' or view.get('state') not in ('idle','tx_denied'):
        raise ValueError('Stop the current radio/codec test before updating')
    expected.append(b'\x00');_,info=link.request(0x1a,b'\x00',busy_wait=0)
    if len(info)!=19:raise ValueError('Malformed update-helper span')
    magic,base,size,chunk,armed,received=struct.unpack('<IIIHBI',info)
    if magic!=MAGIC or base!=BASE or size!=0x14000 or not chunk:raise ValueError('Unexpected update-helper span')
    chunk=min(chunk,59)
    begin=b'\x01'+struct.pack('<IIII',MAGIC,len(image),zlib.crc32(image)&0xffffffff,BASE)
    expected.append(begin);_,reply=link.request(0x1a,begin,busy_wait=0)
    if reply!=bytes(4):raise ValueError('RAM-helper BEGIN acknowledgment mismatch')
    log(f'Uploading approved {len(image)}-byte RAM helper; no flash reads or writes.')
    for offset in range(0,len(image),chunk):
        part=image[offset:offset+chunk];p=b'\x02'+struct.pack('<I',offset)+part
        expected.append(p);_,reply=link.request(0x1a,p,busy_wait=0)
        if reply!=struct.pack('<I',offset+len(part)):raise ValueError('RAM-helper DATA acknowledgment mismatch')
    p=b'\x03'+struct.pack('<I',MAGIC);expected.append(p)
    link.request(0x1a,p,busy_wait=0)
    time.sleep(0.3)
    return {'approved_helper_sha256':backup.RAM_SHA,'helper_bytes':len(image),'load_address':BASE,
            'preinstall_reads_requested':False,'rf_transmit_requested':False}
