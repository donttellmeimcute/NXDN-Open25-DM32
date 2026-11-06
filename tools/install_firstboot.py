#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exact-image DM32 first-boot installation, bounded writes and full readback.

Uses the pinned RAM monitor already used for backups. Only changed application
sectors can be erased/programmed, with their exact planned page bytes. Boot and
data flash are read-only. Reset is permitted only after independent verification.
"""
import argparse
import io
import json
import os
from pathlib import Path
import sys
import time
import zlib
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from tools import dm32_readonly_backup as backup
from tools.prepare_unit_image import saved_images
from tools import firstboot_check
from tools import normal_reboot
from tools import warm_update

START=0xc000
SIZE=0x100000
SECTOR=4096
IMAGE_SHA='e794eb6dc4188ec84cc5d937ba7543ba5b2f7b563986c0353be00882694de32b'
PREVIOUS_APPLICATION_SHA='82432fdbdfca2f4619dd52a29019ae8de8d56f8f0b22d8ac1d7cc506d90561df'

def packet(op,payload=b''):
    return bytes([2,op])+len(payload).to_bytes(2,'big')+bytes([sum(payload)&255,3])+payload
def spi_packet(tx,rx=0):
    return packet(5,bytes.fromhex('040000000100')+len(tx).to_bytes(2,'big')+rx.to_bytes(2,'big')+tx)

class InstallPort(backup.ReadOnlyPort):
    def __init__(self,raw,module,original,target,skip_original=False,bulk=False):
        super().__init__(raw,module)
        if len(original)!=SIZE or len(target)!=SIZE or original[:START]!=target[:START]:
            raise ValueError('Exact 1 MiB images with identical protected boot area required')
        self.original,self.target=bytes(original),bytes(target)
        self.todo=[a for a in range(START,SIZE,SECTOR) if original[a:a+SECTOR]!=target[a:a+SECTOR]]
        self.plan=[];self.index=0;self.enabled=False;self.closed_writes=False
        self.reset_plan=[];self.reset_index=0
        self.skip_original=skip_original;self.sector_ends={};self.bulk=bulk
        for a in self.todo:
            if bulk:
                sector=target[a:a+SECTOR]
                body=bytes.fromhex('040000000100')+a.to_bytes(4,'big')
                self.plan.append(packet(14,body+(sector if sector!=b'\xff'*SECTOR else b'')))
                self.sector_ends[a]=len(self.plan);continue
            self.plan += [spi_packet(b'\x06'),spi_packet(b'\x20'+a.to_bytes(3,'big'))]
            for q in range(a,a+SECTOR,256):
                page=target[q:q+256]
                if page!=b'\xff'*256:
                    self.plan += [spi_packet(b'\x06'),spi_packet(b'\x02'+q.to_bytes(3,'big')+page)]
            self.sector_ends[a]=len(self.plan)
    def authorize(self,program,data,expected_data):
        if self.enabled or self.closed_writes or program!=self.original or data!=expected_data:
            raise ValueError('Live pre-write comparison failed; writes remain disabled')
        self.enabled=True
    def authorize_saved_backup(self):
        if not self.skip_original or self.enabled or self.closed_writes:
            raise ValueError('Saved-backup-only installation was not selected')
        self.enabled=True
    def finish_writes(self):
        if not self.enabled or self.index!=len(self.plan):raise ValueError('Write plan incomplete')
        self.enabled=False;self.closed_writes=True
    def authorize_reset(self,program,data,expected_data):
        if not self.closed_writes or program!=self.target or data!=expected_data:
            raise ValueError('Post-write verification failed; reset remains disabled')
        self._allow_reset()
    def authorize_reset_application(self,application):
        if not self.skip_original or not self.closed_writes or application!=self.target[START:]:
            raise ValueError('Application readback failed; reset remains disabled')
        self._allow_reset()
    def _allow_reset(self):
        if self.reset_plan:raise ValueError('Restart was already authorized')
        m=self.m
        values=((m.CRASH_SOFT_MARK,m.CRASH_BOOT_SOFT),(m.WDT_LOCK,m.WDT_KEY),(m.WDT_EN,0),
                (m.WDT_LOCK,0),(m.QUAD_ENABLE,0),(m.LCSFC_BAUDR,4),(m.SYS_SOFT_RSTN,0))
        self.reset_plan=[packet(3,a.to_bytes(4,'big')+v.to_bytes(4,'little')) for a,v in values]
    def validate(self,data):
        if self.monitor:
            if self.skip_original:
                op=data[1] if len(data)>1 else -1
                payload=data[6:]
                if op in (7,9):
                    if (not self.closed_writes or len(payload)!=12 or self.memory(payload)!='progmem' or
                        int.from_bytes(payload[6:10],'big')<START):
                        raise ValueError('Original firmware/data reads were disabled by the owner')
                if op==5 and len(payload)>=14 and payload[10]==3:
                    address=int.from_bytes(payload[11:14],'big');n=int.from_bytes(payload[8:10],'big')
                    sectors=range(address//SECTOR*SECTOR,(address+n+SECTOR-1)//SECTOR*SECTOR,SECTOR)
                    if (self.memory(payload)!='progmem' or address<START or
                        not self.closed_writes and not (self.enabled and all(
                            a in self.sector_ends and self.index>=self.sector_ends[a] for a in sectors))):
                        raise ValueError('Only already-written application bytes may be read back')
            if data==spi_packet(b'\x05',1):return 'program_status_read'
            if self.enabled and self.index<len(self.plan) and data==self.plan[self.index]:
                self.index+=1
                if self.bulk:return 'program_sector_write'
                tx=data[16:]
                return {6:'program_write_enable',0x20:'program_sector_erase',2:'program_page'}[tx[0]]
            if self.reset_index<len(self.reset_plan) and data==self.reset_plan[self.reset_index]:
                self.reset_index+=1;return 'verified_restart_register'
        return super().validate(data)

def crc_list(data):return [zlib.crc32(data[i:i+SECTOR])&0xffffffff for i in range(0,len(data),SECTOR)]
def device_crcs(radio,memory,size,label):
    out=[];last=-1
    for a in range(0,size,16*SECTOR):
        n=min(16,(size-a)//SECTOR)
        out.extend(crc for crc,blank in radio.sums(memory,a,n))
        percent=(a+n*SECTOR)*100//size
        if percent//10!=last:
            print(f'{label}: {percent}%',flush=True);last=percent//10
    return out
def read_program(radio,path,label,start=0):
    if path.exists():raise FileExistsError(path)
    partial=path.with_suffix(path.suffix+'.partial')
    last=[-1]
    def progress(done,total):
        p=done*100//total
        if p//10!=last[0]:print(f'{label}: {p}%',flush=True);last[0]=p//10
    with partial.open('xb+') as out:
        radio.dump(out,'progmem',start,SIZE,progress=progress)
        out.flush();os.fsync(out.fileno())
    data=partial.read_bytes()
    if len(data)!=SIZE-start:raise ValueError('Incomplete program readback')
    os.link(partial,path);partial.unlink()
    return data
def write_plan(radio,guard,progress=None):
    # Force known generic SPI commands. No opaque sector-write operation or
    # automatic re-erase/reprogram retry is allowed after a serial error.
    radio.retries=0
    if guard.bulk and not radio.sector_op:raise ValueError('Pinned helper does not report sector-write support')
    for i,a in enumerate(guard.todo):
        if guard.bulk:
            sector=guard.target[a:a+SECTOR]
            crc=radio.write_sector('progmem',a,None if sector==b'\xff'*SECTOR else sector)
            if crc!=zlib.crc32(sector)&0xffffffff:raise ValueError(f'Sector CRC readback mismatch at {a:#x}; stopped without retry')
            if progress:progress(i+1,len(guard.todo))
            continue
        tx=b'\x20'+a.to_bytes(3,'big')
        radio._write_cmd('progmem',tx,f'erase {a:#x}',3)
        for q in range(a,a+SECTOR,256):
            page=guard.target[q:q+256]
            if page!=b'\xff'*256:
                radio._write_cmd('progmem',b'\x02'+q.to_bytes(3,'big')+page,f'program {q:#x}',1)
        got=b''.join(radio.read_flash('progmem',q,1024) for q in range(a,a+SECTOR,1024))
        if got!=guard.target[a:a+SECTOR]:raise ValueError(f'Sector readback mismatch at {a:#x}; stopped')
        if progress:progress(i+1,len(guard.todo))
    guard.finish_writes();radio.retries=3

def inputs(folder,package,installed_application=None):
    images=saved_images(folder)
    m=json.loads((package/'preflight.json').read_text())
    if not m.get('firstboot_restrictions_enabled') or m['exact_profile']!='DM32.01.L01.048.bin' or m['codec_slot']['tag']!=48:
        raise ValueError('Only the reviewed exact .048 restricted package is permitted')
    for name,record in m['outputs'].items():
        data=(package/name).read_bytes()
        if len(data)!=record['bytes'] or backup.sha(data)!=record['sha256']:raise ValueError('Staged package mismatch')
    raw=(package/'rfoundry-unit048-firstboot.candidate.bin').read_bytes()
    if backup.sha(raw)!=IMAGE_SHA or not raw or len(raw)>SIZE-START:raise ValueError('Wrong reviewed first-boot payload')
    original=images['progmem-pass1.bin']
    target=original[:START]+raw+b'\xff'*(SIZE-START-len(raw))
    if installed_application is not None:
        prior=installed_application.read_bytes()
        if len(prior)!=SIZE-START or backup.sha(prior)!=PREVIOUS_APPLICATION_SHA:
            raise ValueError('Previous installed-application file is not the verified first-boot readback')
        original=original[:START]+prior
    return original,target,images['q128-pass1.bin']

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--port',required=True);ap.add_argument('--backup',type=Path,required=True)
    ap.add_argument('--package',type=Path,required=True);ap.add_argument('--out',type=Path,required=True)
    ap.add_argument('--wait',type=int,default=120)
    ap.add_argument('--warm-update',action='store_true',help='approved runtime RAM-helper entry; no physical power cycle')
    ap.add_argument('--sector-write',action='store_true',help='pinned protocol-7 whole-sector writes with readback CRC; no write retry')
    ap.add_argument('--installed-application',type=Path,help='use the previously verified application readback file as the write basis; no device read')
    ap.add_argument('--use-saved-backup',action='store_true',help='owner-requested skip of all pre-install firmware/data reads')
    ap.add_argument('--attach-known-monitor',action='store_true',help='reuse the pinned helper left running by this installation session')
    ap.add_argument('--restart-running',action='store_true',help='normal restart then attempt the reviewed power-on RAM helper, keeping COM open')
    args=ap.parse_args()
    if args.warm_update and (not args.use_saved_backup or args.restart_running or args.attach_known_monitor):
        ap.error('Warm update requires saved-backup mode and excludes other entry modes')
    if not 1<=args.wait<=600:ap.error('Wait must be 1..600 seconds')
    if args.restart_running and (args.attach_known_monitor or not args.use_saved_backup or not args.installed_application):
        ap.error('--restart-running requires saved-backup and verified previous application, with no attach-known-monitor')
    original,target,data=inputs(args.backup,args.package,args.installed_application)
    module=backup.load_upstream()
    expected_data=crc_list(data)
    args.out.mkdir(parents=True,exist_ok=False,mode=0o700)
    report={'started_utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),'image_sha256':IMAGE_SHA,
            'writes_started':False,'installation_verified':False,'bootloader_unchanged':False,
            'data_sector_crcs_unchanged':False,'restart_sent':False,'health_receipt_passed':False}
    report['sector_write_mode']=args.sector_write
    report['preinstall_reads_requested']=not args.use_saved_backup
    report['bootloader_readback_performed']=not args.use_saved_backup
    report['data_crc_check_performed']=not args.use_saved_backup
    report['write_basis']='previous verified application file' if args.installed_application else 'saved original stock backup'
    def receipt():
        report['outgoing_operations']=guard.counts
        report['planned_write_frames']=len(guard.plan);report['sent_write_frames']=guard.index
        (args.out/'install-receipt.json').write_text(json.dumps(report,indent=2)+'\n')
    import serial
    raw=serial.Serial(port=None,baudrate=115200,timeout=0.02,write_timeout=3)
    raw.rts=raw.dtr=False;raw.port=args.port
    guard=InstallPort(raw,module,original,target,skip_original=args.use_saved_backup,bulk=args.sector_write)
    report['changed_application_sectors']=guard.todo
    try:
        raw.open()
        if args.warm_update:
            report['warm_update']=warm_update.enter(args.port,raw,lambda s:print(s,flush=True))
            guard.monitor=True;raw.reset_input_buffer()
        if args.restart_running:
            link=normal_reboot.open_link(args.port,raw)
            normal_reboot.request_restart(link)
            report['normal_restart_before_helper_requested']=True
            print('Normal remote restart requested; waiting for the power-on loader on the same cable.',flush=True)
        radio=module.Radio(guard,log=lambda s:print(s,flush=True),retries=3)
        if args.attach_known_monitor or args.warm_update:
            guard.monitor=True;raw.reset_input_buffer();radio.hello(retries=3)
            print('Attached to the known RAM helper; no power cycle needed.',flush=True)
        else:
            print(('Waiting for the remote restart loader.' if args.restart_running else f'READY on {args.port}: turn ON normally, no buttons held.')+f' Waiting {args.wait}s.',flush=True)
            radio.start(wait=args.wait)
        radio.configure('dm32')
        if not radio.v6 or radio.v6['board']!=1 or radio.v6['flags']!=0 or not radio.configured:
            raise ValueError('Monitor configuration/identity did not validate')
        radio.check_writable()
        for memory,want in (('progmem','c84014'),('q128','ef4018')):
            got=radio.jedec(memory)
            if got.hex()!=want:raise ValueError('Attached flash IDs differ from saved unit')
            guard.limits[memory]=backup.capacity(got,memory)
        if args.use_saved_backup:
            print('Using verified saved backups. No pre-install firmware or settings-chip reads.',flush=True)
            guard.authorize_saved_backup()
        else:
            before=read_program(radio,args.out/'program-before.bin','Read original firmware')
            before_data=device_crcs(radio,'q128',len(data),'Check original settings chip')
            report['precheck_data_crc32']=before_data
            report['precheck_changed_data_sectors']=[i*SECTOR for i,(a,b) in enumerate(zip(before_data,expected_data)) if a!=b]
            report['precheck_program_matches_saved_stock']=before==original
            receipt();guard.authorize(before,before_data,expected_data)
            report['original_program_sha256']=backup.sha(before)
        report['writes_started']=True;receipt()
        print(f'INSTALLING: {len(guard.todo)} changed application sectors. Boot/data writes blocked.',flush=True)
        last=[-1]
        def progress(n,total):
            p=n*100//total
            if p//5!=last[0]:print(f'Firmware written and sector-readback checked: {p}% ({n}/{total})',flush=True);last[0]=p//5;receipt()
        write_started=time.monotonic();write_plan(radio,guard,progress)
        report['write_seconds']=time.monotonic()-write_started
        verify_started=time.monotonic()
        if args.use_saved_backup:
            after=read_program(radio,args.out/'application-after.bin','Verify newly installed application',start=START)
            guard.authorize_reset_application(after)
            report['installed_application_sha256']=backup.sha(after)
        else:
            after=read_program(radio,args.out/'program-after.bin','Read complete installed program chip')
            after_data=device_crcs(radio,'q128',len(data),'Recheck untouched settings chip')
            guard.authorize_reset(after,after_data,expected_data)
            report.update(bootloader_unchanged=True,data_sector_crcs_unchanged=True,
                          installed_program_sha256=backup.sha(after),data_sector_crc32=after_data)
        report['verification_seconds']=time.monotonic()-verify_started
        report['installation_verified']=True
        receipt();print('VERIFIED: installed application readback matches. Restarting.',flush=True)
        radio.retries=0;radio.reset_chip();report['restart_sent']=True;receipt();raw.close()
        time.sleep(3)
        health_error=None
        for attempt in range(3):
            try:
                with firstboot_check.open_link(args.port) as link:health=firstboot_check.collect(link)
                with (args.out/'firstboot-health.json').open('x') as f:json.dump(health,f,indent=2);f.write('\n')
                report['health_receipt_passed']=True;health_error=None;break
            except Exception as exc:
                health_error=f'{type(exc).__name__}: {exc}'
                print(f'Health check {attempt+1}/3: {health_error}',flush=True);time.sleep(2)
        if health_error:raise RuntimeError('Installed bytes verified, but boot/cable health did not pass: '+health_error)
        print('PASS: restricted firmware boots and answers cable health queries. No RF test run.',flush=True)
    except BaseException as exc:
        report['error']=f'{type(exc).__name__}: {exc}';raise
    finally:
        report['finished_utc']=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())
        raw.close();receipt()
if __name__=='__main__':main()
