#!/usr/bin/env python3
"""Lock, fetch, and verify public research inputs without modifying pristine trees."""
import argparse
import ast
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / 'sources.lock.json'
DOWNLOADS = {
    'research/toolchain/downloads/binutils-2.45.1.tar.xz': 'https://ftp.gnu.org/gnu/binutils/binutils-2.45.1.tar.xz',
    'research/toolchain/downloads/gcc-14.3.0.tar.xz': 'https://ftp.gnu.org/gnu/gcc/gcc-14.3.0/gcc-14.3.0.tar.xz',
    'research/originals/DM32.01.01.049.bin': 'https://raw.githubusercontent.com/M7OCM/DM-32UV/8efe0e504392aefbfbd84169df6f2da1d32cdd3c/DM32.01.01.049.bin',
    'research/modem/HR_C7000_Document_2.pdf': 'https://www.connectsystems.com/products/top/radios/CS120D/HR_C7000%20Document%202.pdf',
    'research/protocol/Zetron-TG-001-5-0-0-P25-Training-Guide.pdf': 'https://www.zetron.com/wp-content/uploads/2023/12/TG-001-5-0-0-P25-Training-Guide_rebrand.pdf',
    'research/protocol/Tait-TRG-00001-01-M-Intro-to-P25.pdf': 'https://www.taitradioacademy.com/wp-content/uploads/2016/02/TRG-00001-01-M_Intro_to_P25.pdf',
    'research/modem/DM32_component_side_annotated.jpg': 'https://raw.githubusercontent.com/yo3hjv/Baofeng-DM32/b6c9703934eaccfacaacc10b64944c3721c18734/DM32%20Component%20side%20adnotated.jpg',
}


def sha_file(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for data in iter(lambda: f.read(1 << 20), b''):
            h.update(data)
    return h.hexdigest()


def git(path, *args):
    return subprocess.check_output(['git', '-C', str(path), *args], text=True).strip()


def archive_sha(path, commit):
    h = hashlib.sha256()
    proc = subprocess.Popen(['git', '-C', str(path), 'archive', '--format=tar', commit], stdout=subprocess.PIPE)
    for data in iter(lambda: proc.stdout.read(1 << 20), b''):
        h.update(data)
    proc.stdout.close()
    if proc.wait():
        raise RuntimeError('git archive failed')
    return h.hexdigest()


def record():
    repos = []
    for category in ('upstream', 'codec', 'modem', 'protocol'):
        for dotgit in sorted((ROOT / 'research' / category).glob('*/.git')):
            path = dotgit.parent
            commit = git(path, 'rev-parse', 'HEAD')
            dirty = git(path, 'status', '--porcelain', '--untracked-files=no')
            if dirty:
                raise ValueError('Pristine dependency modified: ' + str(path))
            tracked = git(path, 'ls-files').splitlines()
            license_files = [p for p in tracked if Path(p).name.lower().startswith(('license', 'copying', 'notice'))]
            repos.append({'path': str(path.relative_to(ROOT)), 'url': git(path, 'remote', 'get-url', 'origin'),
                          'commit': commit, 'commit_date': git(path, 'show', '-s', '--format=%cI', 'HEAD'),
                          'tree': git(path, 'rev-parse', 'HEAD^{tree}'),
                          'git_archive_sha256': archive_sha(path, commit),
                          'license_files': license_files,
                          'license_note': 'No root license file found; inspect per-file notices / docs/licenses.md' if not license_files else 'See original license files and docs/licenses.md'})
    files = []
    for relative, url in DOWNLOADS.items():
        path = ROOT / relative
        if path.exists():
            files.append({'path': relative, 'url': url, 'bytes': path.stat().st_size, 'sha256': sha_file(path)})
    result = {'schema': 1, 'retrieved_date': '2026-10-26', 'repositories': repos, 'files': files,
              'note': 'Original public inputs, separate from generated source/patches/artifacts. Toolchain prerequisite checksums are pinned by GCC contrib/download_prerequisites and gcc/contrib/prerequisites.sha512.'}
    LOCK.write_text(json.dumps(result, indent=2) + '\n')
    print(f'Locked {len(repos)} repositories and {len(files)} direct downloads')


def verify_record(repo):
    path = ROOT / repo['path']
    if git(path, 'rev-parse', 'HEAD') != repo['commit']:
        raise ValueError('Incompatible dependency commit: ' + repo['path'])
    if git(path, 'status', '--porcelain', '--untracked-files=no'):
        raise ValueError('Modified pristine dependency: ' + repo['path'])
    if archive_sha(path, repo['commit']) != repo['git_archive_sha256']:
        raise ValueError('Dependency archive SHA-256 mismatch: ' + repo['path'])


def fetch_or_verify(fetch):
    lock = json.loads(LOCK.read_text())
    for repo in lock['repositories']:
        path = ROOT / repo['path']
        if not path.exists() and fetch:
            path.parent.mkdir(parents=True, exist_ok=True)
            subprocess.run(['git', 'init', str(path)], check=True, stdout=subprocess.DEVNULL)
            subprocess.run(['git', '-C', str(path), 'remote', 'add', 'origin', repo['url']], check=True)
            subprocess.run(['git', '-C', str(path), 'fetch', '--depth=1', 'origin', repo['commit']], check=True)
            subprocess.run(['git', '-C', str(path), 'checkout', '--detach', repo['commit']], check=True)
        verify_record(repo)
        print('verified', repo['path'])
    for f in lock['files']:
        path = ROOT / f['path']
        if not path.exists() and fetch:
            path.parent.mkdir(parents=True, exist_ok=True)
            partial = path.with_name(path.name + '.partial')
            with urllib.request.urlopen(f['url'], timeout=60) as incoming, partial.open('xb') as out:
                for data in iter(lambda: incoming.read(1 << 20), b''):
                    out.write(data)
            if sha_file(partial) != f['sha256'] or partial.stat().st_size != f['bytes']:
                raise ValueError('Downloaded input hash mismatch (partial preserved): ' + f['path'])
            partial.rename(path)
        if path.stat().st_size != f['bytes'] or sha_file(path) != f['sha256']:
            raise ValueError('Input hash mismatch: ' + f['path'])
        print('verified', f['path'])
    if fetch:
        source = ROOT / 'research/upstream/roger/python/C7000_read_progmem.py'
        tree = ast.parse(source.read_text())
        chunks = next(ast.literal_eval(n.value) for n in tree.body
                      if isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and
                      t.id == 'firmwareBytes' for t in n.targets))
        data = b''.join(chunks)
        expected = '59de726d5a50cf17573d35e98d183a184d3cc5da81ca30e198fb061cd3ee0f46'
        if hashlib.sha256(data).hexdigest() != expected:
            raise ValueError('Unexpected original helper literal')
        dest = ROOT / 'research/originals/C7000_read_progmem-helper.bin'
        if dest.exists():
            if sha_file(dest) != expected:
                raise ValueError('Preserve modified helper original; refusing replacement')
        else:
            with dest.open('xb') as out:
                out.write(data)
        print('verified derived original loader helper')


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('command', choices=['record', 'fetch', 'verify'])
    args = ap.parse_args()
    try:
        record() if args.command == 'record' else fetch_or_verify(args.command == 'fetch')
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as exc:
        raise SystemExit(str(exc))
