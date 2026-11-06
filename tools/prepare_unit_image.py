#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline unit-image compatibility check and private candidate packaging.

Validates both saved flash passes, the exact stock-codec profile, the existing
build manifest, codec relocation/CRC and updater bounds. Never opens a port.
Outputs include proprietary/unit data and belong only in ignored artifacts.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[1]
UP=ROOT/'research/upstream/radiofoundry'
PIN='dfbaf20c400c73e2ccf6edb80af7d1d255bee418'


def sha(data): return hashlib.sha256(data).hexdigest()
def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module


def saved_images(folder):
    manifest=json.loads((folder/'backup-manifest.json').read_text())
    names={f'{m}-pass{n}.bin' for m in ('progmem','q128') for n in (1,2)}
    if not manifest.get('backup_verified') or {r['file'] for r in manifest['captures']}!=names:
        raise ValueError('A complete verified two-pass backup is required')
    images={}
    for record in manifest['captures']:
        data=(folder/record['file']).read_bytes()
        if len(data)!=record['bytes'] or sha(data)!=record['sha256']:
            raise ValueError('Saved image length/hash differs from its manifest')
        sectors=[sha(data[i:i+4096]) for i in range(0,len(data),4096)]
        if sectors!=record['sector_sha256']: raise ValueError('Saved sector hashes differ')
        images[record['file']]=data
    for m in ('progmem','q128'):
        if images[m+'-pass1.bin']!=images[m+'-pass2.bin']: raise ValueError('Saved passes differ')
    if len(images['progmem-pass1.bin'])!=0x100000 or len(images['q128-pass1.bin'])!=0x1000000:
        raise ValueError('Candidate board preset requires 1 MiB program and 16 MiB data flash')
    return images


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--backup',type=Path,required=True)
    ap.add_argument('--image',type=Path,default=ROOT/'build/radiofoundry-p25/build/dm32-p25-native/rfoundry_dm32.bin')
    ap.add_argument('--expected-profile',default='DM32.01.L01.048.bin')
    ap.add_argument('--require-first-boot',action='store_true',help='refuse an image without immutable first-boot restrictions')
    ap.add_argument('--development',action='store_true',help='allow a hash-verified incremental development build without forced rebuild proof')
    ap.add_argument('--out',type=Path,required=True)
    args=ap.parse_args()
    out=args.out.resolve()
    if not out.is_relative_to(ROOT/'artifacts') and not out.is_relative_to(ROOT/'build'):
        ap.error('Private outputs must stay under ignored artifacts/ or build/')
    if out.exists(): raise FileExistsError('Preserving existing output directory')
    if subprocess.check_output(['git','-C',str(UP),'rev-parse','HEAD'],text=True).strip()!=PIN:
        raise ValueError('Upstream pin changed')
    if subprocess.check_output(['git','-C',str(UP),'status','--porcelain','--untracked-files=no'],text=True).strip():
        raise ValueError('Pristine upstream has tracked modifications')
    images=saved_images(args.backup)
    codec=load('unit_codec',UP/'tools/hrc7000_codec.py')
    loader=load('unit_loader',UP/'tools/serial_loader.py')
    profile,code,tables=codec.extract(images['progmem-pass1.bin'],board='dm32')
    if profile['name']!=args.expected_profile: raise ValueError('Detected codec profile differs from expected unit')
    bm=json.loads(args.image.with_suffix('.json').read_text())
    if bm['upstream_commit']!=PIN or not bm['native_interface_enabled'] or not bm['p25_enabled']:
        raise ValueError('Wrong laboratory image preset')
    firstboot=bm.get('firstboot_restrictions_enabled',False)
    if args.require_first_boot and not firstboot: raise ValueError('First-boot restrictions are required')
    if firstboot:
        dev=bm.get('dev_update_enabled',False)
        required={'CONFIG_FIRSTBOOT=y','CONFIG_STUB_ENTRY='+('y' if dev else 'n'),'CONFIG_CPS_OPENFW=n','CONFIG_CPS_FAST=n','CONFIG_AUDIO_PROBE=n'}
        if dev:required.add('CONFIG_DEV_UPDATE=y')
        tx=bm.get('native_tx_bench_enabled',False)
        if tx:
            required.add('CONFIG_DEV_TX=y')
            if not dev or bm.get('firstboot_flags')!=47 or not bm['audits'].get('native_tx_scoped_grant'):
                raise ValueError('Native TX package lacks its explicit bounded grant profile')
        if not required.issubset(bm['command']) or not (bm['forced_rebuild_identical'] or args.development):
            raise ValueError('First-boot build flags/reproducibility do not validate')
    if firstboot and not bm.get('generated_sources'):raise ValueError('Missing generated-source provenance')
    for path,digest in {**bm['sources'],**bm.get('generated_sources',{})}.items():
        if sha((ROOT/path).read_bytes())!=digest: raise ValueError('Build inputs changed: '+path)
    key=str(args.image.resolve().relative_to(ROOT))
    if sha(args.image.read_bytes())!=bm['outputs'][key]: raise ValueError('Application differs from build manifest')
    raw=codec.merge(args.image.read_bytes(),profile,code,tables)
    slot_offset=codec.BOARDS['dm32']['slot'][0]-0x0300c000
    slot=codec.parse_slot(raw[slot_offset:])
    if not slot or slot['tag']!=48 or slot['code_sha256']!=sha(code) or slot['tables_sha256']!=sha(tables):
        raise ValueError('Codec slot did not validate')
    model=loader.MODELS['dm32']
    if (model.app_base,model.flash_end)!=(0x0300c000,0x03100000): raise ValueError('Unexpected updater boundaries')
    original=images['progmem-pass1.bin'][0xc000:]
    label='firstboot' if firstboot else 'native'
    payloads={f'rfoundry-unit048-{label}.candidate.bin':raw,
              f'rfoundry-unit048-{label}.candidate.BFUV32.bin':loader.wrap(raw,model),
              'stock048-unit-restore.candidate.BFUV32.bin':loader.wrap(original,model)}
    for name,body in payloads.items():
        decoded,kind=loader.unwrap(body,model);loader.check_image(decoded,model)
        if name.startswith('stock') and decoded!=original: raise ValueError('Restore candidate differs from original range')
    out.mkdir(parents=True,mode=0o700)
    for name,body in payloads.items():
        with (out/name).open('xb') as f:f.write(body)
        (out/name).chmod(0o600)
    report={'upstream_commit':PIN,'exact_profile':profile['name'],'base_application_manifest_verified':True,
            'firstboot_restrictions_enabled':firstboot,'dev_update_enabled':bm.get('dev_update_enabled',False),
            'native_tx_bench_enabled':bm.get('native_tx_bench_enabled',False),'firstboot_flags':bm.get('firstboot_flags'),
            'forced_rebuild_identical':bm['forced_rebuild_identical'],'development_incremental':bool(args.development and not bm['forced_rebuild_identical']),
            'application_start':'0x0300C000','program_flash_end':'0x03100000',
            'bootloader_range_excluded':['0x03000000','0x0300C000'],'data_flash_in_update_payload':False,
            'codec_sha256':sha(code),'tables_sha256':sha(tables),'codec_slot':slot,
            'outputs':{name:{'bytes':len(body),'sha256':sha(body)} for name,body in payloads.items()},
            'hardware_installation_tested':False,'restore_write_tested':False,
            'private_unit_backup_manifest_sha256':sha((args.backup/'backup-manifest.json').read_bytes())}
    with (out/'preflight.json').open('x') as f:json.dump(report,f,indent=2);f.write('\n')
    (out/'preflight.json').chmod(0o600)
    print(json.dumps({'exact_profile':profile['name'],'codec_tag':slot['tag'],'relocation_count':len(slot['relocations']),
                      'candidate_outputs':list(payloads),'radio_touched':False},indent=2))


if __name__=='__main__': main()
