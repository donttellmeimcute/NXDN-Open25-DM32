#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Real fixed-point decoder, CAI/replay worker and PCM pacing, host ASan/UBSan."""
from pathlib import Path
import subprocess
import re
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/audio-host'
APP=ROOT/'build/radiofoundry-p25'
CODEC=ROOT/'build/codec-speed-host'
def run(args,**kw):return subprocess.run([str(a) for a in args],cwd=ROOT,check=True,**kw)
def main():
    OUT.mkdir(parents=True,exist_ok=True)
    run(['python3','tools/build_radiofoundry.py','--prepare-only'])
    run(['python3','experiments/codec/build.py','--speed','--full-ubsan','--out',CODEC])
    flags=['-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
           '-fsanitize=address,undefined','-fno-omit-frame-pointer','-Isrc/p25','-Isrc/codec',
           '-Itests/audio/stubs','-Iintegration/radiofoundry','-I'+str(APP/'core'),
           '-I'+str(APP/'hal'),'-DCONFIG_P25_NATIVE=1']
    objects=[]
    sources=[ROOT/'src/p25'/f'{name}.c' for name in ('p25','voice','phy_stream','selftest','audio_fifo')]
    sources += [ROOT/'integration/radiofoundry/p25_lab.c',ROOT/'integration/radiofoundry/cpu_profile.c',APP/'core/debuglink/dl_fmt.c',ROOT/'tests/audio/test_receive_lab.c']
    for source in sources:
        obj=OUT/(source.stem+'.o')
        run(['clang','-std=c11',*flags,'-c',source,'-o',obj]);objects.append(obj)
    run(['clang++','-fsanitize=address,undefined',*objects,CODEC/'libdm32_imbe.a','-o',OUT/'test_receive_lab'])
    baseline=run([OUT/'test_receive_lab'],stdout=subprocess.PIPE,text=True).stdout;print(baseline,end='')
    lean=[]
    for source in (ROOT/'integration/radiofoundry/p25_lab.c',ROOT/'tests/audio/test_receive_lab.c'):
        obj=OUT/(source.stem+'-lean.o')
        run(['clang','-std=c11',*flags,'-DCONFIG_P25_BENCH=0','-c',source,'-o',obj]);lean.append(obj)
    common=[o for o in objects if o.stem not in ('p25_lab','test_receive_lab')]
    run(['clang++','-fsanitize=address,undefined',*common,*lean,CODEC/'libdm32_imbe.a','-o',OUT/'test_receive_lean'])
    optimized=run([OUT/'test_receive_lean'],stdout=subprocess.PIPE,text=True).stdout;print(optimized,end='')
    hashes=re.findall(r'PCM_CALLBACK_CRC=([0-9a-f]{8})',baseline)
    if len(hashes)!=1 or hashes!=re.findall(r'PCM_CALLBACK_CRC=([0-9a-f]{8})',optimized):
        raise SystemExit('Lean and bench speaker PCM differ')
    run(['clang','-std=c11',*flags,'tests/test_audio_fifo.c','src/p25/audio_fifo.c','-o',OUT/'test_audio_fifo'])
    run([OUT/'test_audio_fifo'])
    run(['clang','-std=c11',*flags,'tests/test_pcm_clock.c','src/p25/pcm_clock.c','-o',OUT/'test_pcm_clock'])
    run([OUT/'test_pcm_clock'])
    run(['clang','-std=c11',*flags,'-I'+str(APP/'drivers'),'-DCONFIG_DMR=1','-DCONFIG_P25=1','-DCONFIG_VOX=0',
         'tests/audio/test_pcm_driver.c','src/p25/pcm_clock.c','-o',OUT/'test_pcm_driver'])
    run([OUT/'test_pcm_driver'])
    for enabled in (0,1):
        out=OUT/f'test_cpu_profile_{enabled}'
        run(['clang','-std=c11',*flags,f'-DCONFIG_CPU_PROFILE={enabled}',
             'tests/audio/test_cpu_profile.c','integration/radiofoundry/cpu_profile.c',APP/'core/debuglink/dl_fmt.c','-o',out])
        run([out])
if __name__=='__main__':main()
