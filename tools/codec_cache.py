#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Content-checked cache for the unchanged, pinned target codec build."""
import hashlib
import importlib.util
import json
from pathlib import Path
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def key(root,speed):
    path=root/'experiments/codec/build.py'
    spec=importlib.util.spec_from_file_location('codec_cache_source',path)
    m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);deps=m.prepare()
    files=[path,Path(__file__),root/'toolchain/bin/csky-elf-g++',root/'toolchain/bin/csky-elf-ar']
    files+=list((root/'toolchain/libexec/gcc/csky-elf').glob('*/cc1plus'))
    for folder in (root/'src/codec',root/'experiments/codec/freestanding/include',deps/'imbe_vocoder'):
        files += [p for p in folder.rglob('*') if p.is_file() and '.git' not in p.parts]
    record={'speed':bool(speed),'inputs':{str(p.relative_to(root)):sha(p) for p in sorted(set(files))}}
    return hashlib.sha256(json.dumps(record,sort_keys=True).encode()).hexdigest()
def reusable(folder,want):
    p=folder/'checked-cache.json'
    if not p.exists():return False
    try:r=json.loads(p.read_text());return r['key']==want and bool(r['outputs']) and all((folder/name).is_file() and sha(folder/name)==digest for name,digest in r['outputs'].items())
    except (KeyError,ValueError,OSError):return False
def remember(folder,want):
    files=[folder/'libdm32_imbe.a',*folder.glob('*.o'),*folder.glob('*.ci')]
    r={'key':want,'outputs':{p.name:sha(p) for p in sorted(files)}}
    (folder/'checked-cache.json').write_text(json.dumps(r,indent=2)+'\n')
