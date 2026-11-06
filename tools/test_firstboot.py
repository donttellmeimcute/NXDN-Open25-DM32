#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise actual restricted derivative drivers on host doubles; no hardware."""
from pathlib import Path
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from tools.dm32_readonly_backup import load_upstream
APP=ROOT/'build/radiofoundry-p25'
OUT=ROOT/'build/firstboot-host'
def run(args):subprocess.run([str(a) for a in args],cwd=ROOT,check=True)
def main():
    run(['python3','tools/build_radiofoundry.py','--prepare-only'])
    OUT.mkdir(parents=True,exist_ok=True)
    flags=['-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
           '-fno-omit-frame-pointer','-DCONFIG_FIRSTBOOT=1','-Itests/firstboot/stubs',
           '-Isrc/modem',*('-I'+str(APP/p) for p in ('core','hal','drivers','boards/dm32','.'))]
    run(['clang',*flags,'tests/firstboot/test_guards.c',APP/'core/firstboot/firstboot.c',
         APP/'drivers/flash/spi_nor/spi_nor.c',APP/'drivers/rf/fd6818/fd6818.c',
         APP/'drivers/rf/subaudio/subaudio.c',APP/'core/storage/storage.c',
         APP/'core/native_modem/native_test.c','-o',OUT/'test_guards'])
    run([OUT/'test_guards'])
    run(['clang',*flags,'-I'+str(APP/'platform/hrc7000'),
         'tests/firstboot/test_receive_chip.c',APP/'core/firstboot/firstboot.c',
         APP/'core/native_modem/native_test.c',APP/'platform/hrc7000/hrc_native_chip.c',
         '-o',OUT/'test_receive_chip'])
    run([OUT/'test_receive_chip'])
    run(['clang',*flags,'-DCONFIG_DEV_UPDATE=1','tests/firstboot/test_update_gate.c',
         APP/'core/firstboot/update_gate.c','-o',OUT/'test_update_gate'])
    helper=OUT/'approved-helper.bin';helper.write_bytes(load_upstream().stub_univ_image());helper.chmod(0o600)
    run([OUT/'test_update_gate',helper])
    run(['clang',*flags,'-DCONFIG_DEV_TX=1','-DCONFIG_DEV_UPDATE=1','tests/firstboot/test_tx_gate.c',
         APP/'core/firstboot/firstboot.c',APP/'core/firstboot/native_tx_gate.c',
         APP/'drivers/flash/spi_nor/spi_nor.c',APP/'drivers/rf/fd6818/fd6818.c',
         APP/'drivers/rf/subaudio/subaudio.c',APP/'core/storage/storage.c',
         APP/'core/native_modem/native_test.c','-o',OUT/'test_tx_gate'])
    run([OUT/'test_tx_gate'])
    run(['clang',*flags,'-DCONFIG_DEV_TX=1','-DCONFIG_P25_NATIVE=1',
         '-Iintegration/radiofoundry/native','tests/firstboot/test_tx_prepare.c',
         APP/'core/firstboot/firstboot.c',APP/'core/firstboot/native_tx_gate.c',
         APP/'core/native_modem/native_test.c','-o',OUT/'test_tx_prepare'])
    run([OUT/'test_tx_prepare'])
if __name__=='__main__':main()
