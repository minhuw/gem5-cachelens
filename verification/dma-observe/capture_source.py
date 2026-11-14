#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Capture source bytes without claiming an in-flight snapshot was build-start evidence."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def git(*args):
    return subprocess.check_output(['git', '-C', str(ROOT), *args])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--phase', required=True)
    parser.add_argument('--build-id', action='append', required=True)
    parser.add_argument('--build-start-witness', action='store_true')
    args = parser.parse_args()
    output = args.output.resolve()
    assert output.is_relative_to(ROOT), 'witness must stay in owned worktree'
    names = git('ls-files', '-z', '--cached', '--others', '--exclude-standard',
                '--', 'src', 'ext', 'include', 'build_opts', 'build_tools',
                'site_scons', 'SConstruct', 'verification/dma-observe').split(b'\0')
    hashes = {}
    for raw in sorted(set(names)):
        if not raw:
            continue
        name = raw.decode()
        path = ROOT / name
        # Symlink identity must not depend on files outside this worktree.
        if path.is_symlink():
            hashes[name] = {'symlink': path.readlink().as_posix()}
        elif path.is_file():
            hashes[name] = hashlib.sha256(path.read_bytes()).hexdigest()
        else:
            hashes[name] = {'missing': True}
    encoded = json.dumps(hashes, sort_keys=True, separators=(',', ':')).encode()
    document = dict(
        captured_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
        phase=args.phase, managed_build_ids=args.build_id,
        build_start_witness=args.build_start_witness,
        source_fingerprint_sha256=hashlib.sha256(encoded).hexdigest(),
        git_head=git('rev-parse', 'HEAD').decode().strip(),
        base_commit='1f3eb7a3b2544eb86c57e9aed8b96a780d5a39fb',
        compiled_commit_claim=False,
        interpretation=('Snapshot at capture time only. An in-flight snapshot cannot '
                        'prove which earlier bytes each existing object compiled.'),
        file_sha256=hashes,
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open('x') as stream:
        json.dump(document, stream, indent=2, sort_keys=True)
        stream.write('\n')
    print(json.dumps({key: document[key] for key in (
        'phase', 'managed_build_ids', 'source_fingerprint_sha256',
        'build_start_witness', 'compiled_commit_claim')}))


if __name__ == '__main__':
    main()
