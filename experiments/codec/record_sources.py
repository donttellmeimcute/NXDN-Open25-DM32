#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Record immutable codec sources and licenses without modifying dependencies."""
import datetime
import hashlib
import json
from pathlib import Path
import subprocess

from build import PINS, ROOT, prepare


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    source_root = prepare()
    originals = source_root / "originals"
    originals.mkdir(exist_ok=True)
    records = []
    for name, (url, commit) in PINS.items():
        repo = source_root / name
        archive = originals / f"{name}-{commit}.tar"
        with archive.open("wb") as stream:
            subprocess.run(["git", "-C", str(repo), "archive", "--format=tar", commit],
                           stdout=stream, check=True)
        files = subprocess.check_output(["git", "-C", str(repo), "ls-files"], text=True).splitlines()
        records.append({
            "name": name, "url": url, "commit": commit,
            "commit_date": subprocess.check_output(["git", "-C", str(repo), "show", "-s", "--format=%cI", commit], text=True).strip(),
            "archive": str(archive.relative_to(ROOT)), "archive_bytes": archive.stat().st_size,
            "archive_sha256": digest(archive),
            "license": "GPL-3.0-or-later notices in DSP sources; repository omits COPYING" if name == "imbe_vocoder" else "ISC in COPYRIGHT and C sources",
            "files": {file: {"bytes": (repo/file).stat().st_size, "sha256": digest(repo/file)} for file in files},
        })
    record = {
        "recorded_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "pristine": True,
        "archive_method": "git archive --format=tar <pinned-commit>; generated from fetched upstream Git objects, not a vendor-signed release",
        "dependencies": records,
        "additional_license_text": {"url": "https://www.gnu.org/licenses/gpl-3.0.txt",
                                     "path": "LICENSES/GPL-3.0-or-later.txt",
                                     "sha256": digest(ROOT / "LICENSES" / "GPL-3.0-or-later.txt")},
    }
    path = source_root / "sources.json"
    path.write_text(json.dumps(record, indent=2) + "\n")
    print(path)


if __name__ == "__main__":
    main()
