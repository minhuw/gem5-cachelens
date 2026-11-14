#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Real-peer rejection checks for explicitly unsupported observation lifecycles."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time

HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gem5', type=Path, required=True)
    parser.add_argument('--peer', type=Path, required=True)
    parser.add_argument('--protocol', choices=('mesi', 'chi'), required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    cases = (
        ('epoch', ['--epoch', ''], 'requires explicit NIC and fresh epoch'),
        ('nic', ['--nic', ''], 'requires explicit NIC and fresh epoch'),
        ('path', [], 'Cannot exclusively create DMA trace'),
        ('atomic', ['--atomic'], 'DMA observation requires timing mode'),
        ('checkpoint', ['--checkpoint-mode'], 'DMA observation does not support checkpoint mode'),
        ('multiple', ['--two-nics'], 'supports exactly one SimBricks PCI endpoint'),
        ('duplicate', [], 'duplicate active wire request ID'),
    )
    manifest = dict(binary_sha256=hashlib.sha256(args.gem5.read_bytes()).hexdigest(), cases=[])
    try:
        for name, extra, message in cases:
            directory = args.out / name
            directory.mkdir()
            trace = directory / 'trace.jsonl'
            if name == 'path':
                trace.write_text('preserve-existing-log\n')
            sockets = [directory / 'p']
            if name == 'multiple':
                sockets.append(directory / 'p.other')
            peers = []
            logs = []
            try:
                for index, socket in enumerate(sockets):
                    log = (directory / f'peer{index}.log').open('w')
                    logs.append(log)
                    cmd = [str(args.peer.resolve()), str(socket), str(directory / f's{index}'),
                           str(directory / f'wire{index}.jsonl')]
                    if name == 'duplicate':
                        cmd.append('--duplicate-active')
                    process = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)
                    peers.append(process)
                    for _ in range(100):
                        if socket.exists(): break
                        assert process.poll() is None
                        time.sleep(.01)
                    assert socket.exists()
                command = [str(args.gem5.resolve()), '-d', str(directory), str(HERE / 'config.py'),
                           '--socket', str(sockets[0]), '--protocol', args.protocol,
                           '--trace', str(trace), *extra]
                with (directory / 'gem5.log').open('w') as output:
                    result = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT,
                                            timeout=30)
                assert result.returncode != 0, name
                assert message in (directory / 'gem5.log').read_text(), name
                if name == 'path':
                    assert trace.read_text() == 'preserve-existing-log\n'
                manifest['cases'].append(dict(name=name, command=command,
                                              exit=result.returncode, expected=message))
            finally:
                # These are intentionally rejected runs; peers need not finish
                # their DMA workload when the native frontend rejects startup.
                for process in peers:
                    process.terminate()
                    try: process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill(); process.wait()
                for log in logs: log.close()
        manifest['passed'] = True
    finally:
        (args.out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'DMA_OBSERVE_REJECTIONS_OK cases={len(cases)}')


if __name__ == '__main__':
    main()
