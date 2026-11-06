#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Native modem control/register/protocol host tests. No radio or serial access."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/"build/native-host"
UP=ROOT/"research/upstream/radiofoundry"
def run(args): subprocess.run([str(x) for x in args],cwd=ROOT,check=True)
def main():
    OUT.mkdir(parents=True,exist_ok=True)
    run(["python3","tools/build_radiofoundry.py","--prepare-only"])
    app=ROOT/"build/radiofoundry-p25"
    flags=["-std=c11","-O1","-g","-Wall","-Wextra","-Werror","-fsanitize=address,undefined","-fno-omit-frame-pointer",
           "-Isrc/modem","-Iintegration/radiofoundry/native","-I"+str(UP/"drivers")]
    run(["clang",*flags,"src/modem/native_test.c","integration/radiofoundry/native/hrc_native_chip.c",
         "tests/native/test_native.c","-o",OUT/"test_native"])
    run([OUT/"test_native"])
    run(["clang",*flags,"-Itests/native/stubs","-Iintegration/radiofoundry","-I"+str(app/"core"),
         "-I"+str(app/"hal"),"-DCONFIG_P25_NATIVE=1","-Wno-unused-parameter",
         "src/modem/native_test.c","integration/radiofoundry/native/native_lab.c","tests/native/test_lab.c",
         "-o",OUT/"test_lab"])
    run([OUT/"test_lab"])
    owner_flags=[*flags,'-Itests/native/stubs','-Iintegration/radiofoundry','-I'+str(app/'core'),'-I'+str(app/'hal'),'-Wno-unused-parameter']
    run(['clang',*owner_flags,'-c','src/modem/native_test.c','-o',OUT/'owner_controller.o'])
    run(['clang',*owner_flags,'-DCONFIG_FIRSTBOOT=1','-DCONFIG_DEV_UPDATE=1','-DCONFIG_P25_NATIVE=1',
         'integration/radiofoundry/native/native_lab.c','tests/native/test_update_owner.c',OUT/'owner_controller.o','-o',OUT/'test_update_owner'])
    run([OUT/'test_update_owner'])
    run(['clang',*owner_flags,'-DCONFIG_FIRSTBOOT=1','-DCONFIG_DEV_UPDATE=1','-DCONFIG_DEV_TX=1','-DCONFIG_P25_NATIVE=1',
         'src/modem/native_test.c','integration/radiofoundry/native/native_lab.c',
         'integration/radiofoundry/firstboot/native_tx_gate.c','integration/radiofoundry/firstboot/firstboot.c',
         'tests/native/test_tx_owner.c','-o',OUT/'test_tx_owner'])
    run([OUT/'test_tx_owner'])
    run(["clang",*flags,"-Itests/native/stubs","-I"+str(app/"core"),"-DCONFIG_P25_NATIVE=1","-DCONFIG_DEV_TX=1","src/modem/native_test.c",
         "integration/radiofoundry/native/hrc_native_chip.c","integration/radiofoundry/native/hrc_native_port.c",
         "tests/native/test_port.c","-o",OUT/"test_port"])
    run([OUT/"test_port"])
    run(["python3","-m","unittest","discover","-s","tests","-p","test_native_client.py","-v"])
if __name__=="__main__": main()
