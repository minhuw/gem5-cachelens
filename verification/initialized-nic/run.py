#!/usr/bin/env python3
"""Run real-peer native checkpoint plumbing and corruption rejection checks."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import signal
import struct
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gem5', type=Path, required=True)
    parser.add_argument('--nic', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--interrupts', action='store_true', help='Require X86 test objects')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    pcap = args.out / 'input.pcap'
    # One valid minimum Ethernet/IPv4/UDP frame accepted by the deferred source.
    frame = (bytes.fromhex('0290000000010080000000010800')
             + struct.pack('!BBHHHBBH4s4s', 0x45, 0, 46, 0, 0, 64, 17, 0,
                           b'\x0a\x00\x00\x01', b'\x0a\x00\x00\x02')
             + struct.pack('!HHHH', 1234, 11211, 26, 0) + bytes(18))
    pcap.write_bytes(struct.pack('<IHHIIII', 0xa1b2c3d4, 2, 4, 0, 0, 1514, 1)
                     + struct.pack('<IIII', 0, 0, len(frame), len(frame)) + frame)
    checkpoint = args.out / 'checkpoint'

    def run(name, state, restore, expected, extra=(), pause_peer=False, stall_barrier=False,
            error=None):
        work = args.out / name
        work.mkdir()
        pci, eth, shm = (work / x for x in ('pci', 'eth', 'shm'))
        with (work / 'nic.log').open('w') as log:
            nic = subprocess.Popen([str(args.nic.resolve()),
                f'listen:{pci.resolve()}:{shm.resolve()}:sync=true',
                f'listen:{eth.resolve()}:{shm.resolve()}:sync=true', '0',
                '029000000001', '/dev/null', '--initialized-checkpoint'],
                stdout=log, stderr=subprocess.STDOUT)
            try:
                end = time.monotonic() + 30
                while not pci.exists() or not eth.exists():
                    if nic.poll() is not None or time.monotonic() > end:
                        raise RuntimeError('NIC failed to create both interfaces')
                    time.sleep(.01)
                if pause_peer:
                    nic.send_signal(signal.SIGSTOP)
                command = [str(args.gem5.resolve()), '-d', str(work / 'gem5'),
                    str(Path(__file__).with_name('probe.py')), '--pci', str(pci.resolve()),
                    '--eth', str(eth.resolve()), '--pcap', str(pcap.resolve()),
                    '--checkpoint', str(state.resolve())]
                if restore:
                    command.append('--restore')
                command.extend(extra)
                if stall_barrier:
                    command.extend(['--stall-peer', str(nic.pid)])
                started = time.monotonic()
                result = subprocess.run(command, stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT, text=True, timeout=60)
                (work / 'gem5.log').write_text(result.stdout)
                if expected:
                    assert result.returncode == 0 and expected in result.stdout, result.stdout
                else:
                    assert result.returncode != 0, result.stdout
                if pause_peer or stall_barrier:
                    assert 'wall deadline exceeded' in result.stdout, result.stdout
                    assert time.monotonic() - started < 35
                if error:
                    assert error in result.stdout, result.stdout
            finally:
                nic.send_signal(signal.SIGINT)
                try:
                    nic.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    nic.kill()
                    nic.wait()
        print('PASS', name, flush=True)

    run('save', checkpoint, False, 'INITIALIZED_NIC_GEM5_SAVE_OK')
    run('restore', checkpoint, True, 'INITIALIZED_NIC_GEM5_RESTORE_OK')
    run('destination-exists', checkpoint, False, None)
    run('geometry-mismatch', checkpoint, True, None, ['--latency', '750ns'])
    run('pcap-settings', checkpoint, True, None, ['--rate', '2000000'],
        error='replay settings mismatch')
    changed_pcap = args.out / 'changed.pcap'
    data = bytearray(pcap.read_bytes())
    data[-1] ^= 1
    changed_pcap.write_bytes(data)
    run('pcap-content', checkpoint, True, None, ['--pcap', str(changed_pcap.resolve())],
        error='changed since the checkpoint')
    started_state = args.out / 'started-state'
    run('started-source', started_state, False, None, ['--start-before-save'],
        error='never-started pristine replay')
    assert not started_state.exists()
    run('resume-source', args.out / 'resume-state', False, None, ['--resume-frozen'],
        error='frozen')
    for mode in ('missing', 'truncated', 'corrupt', 'metadata-mismatch',
                 'missing-manifest', 'wrong-epoch', 'wrong-token', 'wrong-version'):
        bad = args.out / ('state-' + mode)
        shutil.copytree(checkpoint, bad)
        sidecar = next(bad.glob('*.nic-state'))
        if mode == 'missing':
            sidecar.unlink()
        elif mode == 'truncated':
            sidecar.write_bytes(sidecar.read_bytes()[:-1])
        elif mode == 'corrupt':
            data = bytearray(sidecar.read_bytes())
            data[-1] ^= 1
            sidecar.write_bytes(data)
        elif mode == 'metadata-mismatch':
            cp = bad / 'm5.cpt'
            cp.write_bytes(cp.read_bytes() + b'\n# changed after publication\n')
        elif mode == 'missing-manifest':
            (bad / 'initialized-nic.json').unlink()
        else:
            data = bytearray(sidecar.read_bytes())
            data[{'wrong-epoch': 32, 'wrong-token': 16, 'wrong-version': 0}[mode]] ^= 1
            sidecar.write_bytes(data)
            # Pass the outer SHA check to exercise C++ identity/version checks.
            path = bad / 'initialized-nic.json'
            manifest = json.loads(path.read_text())
            manifest['files'][sidecar.name] = hashlib.sha256(data).hexdigest()
            path.write_text(json.dumps(manifest))
        run(mode, bad, True, None)
    if args.interrupts:
        idle = args.out / 'timing-idle-state'
        run('timing-idle-save', idle, False, 'INITIALIZED_NIC_GEM5_SAVE_OK',
            ['--interrupt-case', 'idle'])
        run('timing-idle-restore', idle, True, 'INITIALIZED_NIC_GEM5_RESTORE_OK',
            ['--interrupt-case', 'idle'])
        for case in ('queued', 'delivered', 'writeback'):
            state = args.out / ('interrupt-' + case + '-state')
            run('interrupt-' + case, state, False, None, ['--interrupt-case', case],
                error='Timing packets produced while globally drained')
            assert not state.exists(), 'Unsafe checkpoint was published'
            text = (args.out / ('interrupt-' + case) / 'gem5.log').read_text()
            if case == 'writeback':
                assert 'WRITEBACK_INTERRUPT_QUEUED' in text, text
            else:
                assert 'PIT_BARRIER_FIRED' in text, text
                assert ('delivered=1' if case == 'delivered' else 'delivered=0') in text, text
    run('stalled-introduction', checkpoint, True, None, pause_peer=True)
    stalled_state = args.out / 'stalled-state'
    run('stalled-barrier', stalled_state, False, None, stall_barrier=True)
    assert not stalled_state.exists()


if __name__ == '__main__':
    main()
