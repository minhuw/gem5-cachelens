#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Run one real ordinary i40e/NET-only/native fixture, with exact phase fences."""
import argparse
import collections
import hashlib
import json
import os
from pathlib import Path
import queue
import shutil
import signal
import subprocess
import threading
import time

from run import stats
from same_run_verify import load_parser, verify, finish, require

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class Child:
    def __init__(self, name, command, directory, env=None):
        self.name = name
        self.command = command
        self.events = queue.Queue()
        self.lines = []
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, text=True, bufsize=1, env=env)
        self.log = (directory / (name + '.log')).open('x')
        self.thread = threading.Thread(target=self.read, daemon=True)
        self.thread.start()

    def read(self):
        for line in self.process.stdout:
            self.log.write(line)
            self.log.flush()
            self.lines.append(line.rstrip('\n'))
            self.events.put(line.rstrip('\n'))
        self.events.put(None)

    def wait_line(self, predicate, timeout=120):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            line = self.events.get(timeout=max(.01, deadline - time.monotonic()))
            require(line is not None, f'{self.name} exited before required protocol line')
            if predicate(line): return line
        raise RuntimeError(self.name + ' protocol timeout')

    def send(self, command):
        self.process.stdin.write(command + '\n')
        self.process.stdin.flush()

    def wait(self, timeout=120):
        code = self.process.wait(timeout=timeout)
        self.thread.join(timeout=5)
        self.log.flush()
        return code

    def cleanup(self):
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGINT)
            try: self.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.wait(timeout=5)
        self.thread.join(timeout=5)
        self.log.close()


def wait_sockets(paths, runner, timeout=30):
    deadline = time.monotonic() + timeout
    while not all(p.exists() for p in paths):
        require(runner.process.poll() is None,
                'accuracy Runner exited before listen (exit=' + str(runner.process.returncode) +
                '); inspect runner.log for loader/protocol root cause')
        require(time.monotonic() < deadline, 'accuracy Runner listen timeout')
        time.sleep(.005)


def producer_counters(lines):
    prefix = 'CACHELENS_I40E_DMA_STATS '
    rows = [json.loads(line[len(prefix):]) for line in lines if line.startswith(prefix)]
    require(len(rows) == 1, 'need one real producer exit counter snapshot')
    return rows[0]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--gem5', type=Path, required=True)
    p.add_argument('--producer-dir', type=Path, required=True, help='Delivered NET peer directory')
    p.add_argument('--runner-binary', type=Path, required=True)
    p.add_argument('--runner-build-receipt', type=Path, required=True)
    p.add_argument('--runner-library-dir', type=Path, help='Private verified runtime SONAMEs; Runner child only')
    p.add_argument('--expected-frame', type=Path, required=True)
    p.add_argument('--parser-source', type=Path, required=True)
    p.add_argument('--parser-sha256', required=True)
    p.add_argument('--protocol', choices=('mesi', 'chi'), default='mesi')
    p.add_argument('--case', choices=('on', 'off'), default='on')
    p.add_argument('--reference', type=Path)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--final-tick', type=int, default=50_000_000)
    a = p.parse_args()
    a.out = a.out.resolve()
    require(a.out.is_relative_to(ROOT), 'outputs must stay inside owned native worktree')
    a.out.mkdir(parents=True, exist_ok=False)
    parser = load_parser(a.parser_source, a.parser_sha256)
    expected = a.expected_frame.read_bytes()
    require(len(expected) == 1472, 'need exact delivered NET input frame')
    require(parser.clrx_identity(expected)[0] ==
            dict(format='clrx-v1', flow=5, seq=9, **{'global': 581}), 'NET fixture identity mismatch')
    producer_binary, peer_binary = a.runner_binary.resolve(), a.producer_dir / 'clrx_net_peer'
    accuracy = json.loads(a.runner_build_receipt.read_text())
    require(accuracy.get('schema') == 'cachelens-accuracy-nic-build-v1' and
            accuracy.get('status') == 'complete_accuracy_nic', 'need delivered accuracy NIC receipt')
    expected_runner = (a.runner_build_receipt.parent /
                       'component-i40e/i40e_bm/simb_i40e_bm_accuracy').resolve()
    require(producer_binary == expected_runner, 'final gate must select receipt-bound accuracy binary')
    require(accuracy['sources']['component_i40e']['commit'] ==
            '721dea58a1b0bbab240742fa7db1202d4458536a' and
            accuracy['sources']['simbricks']['commit'] ==
            '32ffdd340bb30d3ec25bbb27bdbe13bbc5b35515', 'accuracy source association mismatch')
    require('EXTRA_CPPFLAGS=-DSIMBRICKS_ACCURACY' in accuracy['accuracy_recipe']['nic_command'] and
            'BIN=simb_i40e_bm_accuracy' in accuracy['accuracy_recipe']['nic_command'],
            'accuracy feature profile missing from receipt')
    for name, value in accuracy['artifacts'].items():
        require(sha(a.runner_build_receipt.parent / name) == value,
                'accuracy artifact changed: ' + name)
    require(sha(producer_binary) == accuracy['artifacts'][
            'component-i40e/i40e_bm/simb_i40e_bm_accuracy'], 'accuracy binary hash mismatch')
    # The separate producer owner supplied this exact receipt. Read only.
    receipt_hashes = {}
    for line in (a.producer_dir / 'build-receipt.sha256').read_text().splitlines():
        value, path = line.split(maxsplit=1)
        require(sha(Path(path)) == value, 'producer receipt file changed: ' + path)
        receipt_hashes[path] = value
    runtime_libraries = {}
    if a.runner_library_dir:
        a.runner_library_dir = a.runner_library_dir.resolve()
        require(a.runner_library_dir.is_relative_to(ROOT), 'private Runner libraries must stay owned')
        runtime_libraries = {path.name: sha(path) for path in a.runner_library_dir.iterdir()
                             if path.is_file()}
        require(bool(runtime_libraries), 'empty Runner library directory')
    reference = json.loads(a.reference.read_text()) if a.reference else None
    if a.case == 'off':
        require(reference and reference.get('passed') and reference['case'] == 'on',
                'off case requires completed actual on-case reference')
    run_scope = a.out.relative_to(ROOT).as_posix().replace('/', '.')
    binding = parser.Binding('same-run-i40e-0', f'{run_scope}.native-process-1',
                             f'{run_scope}.producer-process-1')
    native_file, producer_file = a.out / 'native.jsonl', a.out / 'producer.jsonl'
    pci, net, shm = a.out / 'p', a.out / 'n', a.out / 's'
    require(max(len(str(pci)), len(str(net))) < 100, 'socket path too long')
    runner_cmd = [str(producer_binary),
        f'listen:{pci}:{shm}:sync=true:latency=1000:sync_interval=1000',
        f'listen:{net}:{shm}:sync=true:latency=1000:sync_interval=1000',
        '0', '029000000001', '/dev/null']
    peer_cmd = [str(peer_binary), str(net), str(shm), '1000', '1000']
    native_cmd = [str(a.gem5.resolve()), '-d', str(a.out / 'gem5'),
        str(HERE / 'same_run_config.py'), '--pci-socket', str(pci), '--protocol', a.protocol,
        '--nic', binding.nic, '--epoch', binding.native_epoch,
        '--expected-frame', str(a.expected_frame.resolve()), '--host-log', str(a.out / 'host.jsonl'),
        '--payload-output', str(a.out / 'payload.bin'), '--ready-output', str(a.out / 'ready.json'),
        '--final-tick', str(a.final_tick)]
    if a.case == 'on': native_cmd += ['--native-trace', str(native_file)]
    manifest = dict(schema='cachelens.i40e.same-run.receipt', version=1, case=a.case,
        protocol=a.protocol, binding=dict(nic=binding.nic, native_epoch=binding.native_epoch,
                                       producer_epoch=binding.producer_epoch),
        binary_sha256=sha(a.gem5), runner_sha256=sha(producer_binary), peer_sha256=sha(peer_binary),
        expected_frame_sha256=sha(a.expected_frame), producer_receipt_hashes=receipt_hashes,
        runner_runtime_library_sha256=runtime_libraries,
        runner_library_scope='accuracy Runner child only; native gem5 and NET peer unchanged',
        commands=dict(runner=runner_cmd, peer=peer_cmd, native=native_cmd),
        source_sha256={str(x.relative_to(ROOT)): sha(x) for x in
                       (HERE / 'same_run.py', HERE / 'same_run_config.py', HERE / 'same_run_verify.py')},
        parser_source_sha256=a.parser_sha256,
        runner_build_receipt=str(a.runner_build_receipt.resolve()),
        runner_build_receipt_sha256=sha(a.runner_build_receipt),
        feature_association=dict(runner_profile='SIMBRICKS_ACCURACY',
            runner_source=accuracy['sources']['component_i40e']['commit'],
            simbricks_source=accuracy['sources']['simbricks']['commit'],
            recipe_reference_native_source=accuracy['sources']['gem5']['commit'],
            actual_native_test_base='da29c8a260dd41fb2197115c772a2702c981b02d',
            actual_native_test_host='uncommitted test-only addition',
            combined_fs_or_forward_acceptance=False),
        passed=False, send_count=0)
    children = []
    try:
        env = dict(os.environ, SIMBRICKS_I40E_DMA_TRACE=str(producer_file))
        env.pop('SIMBRICKS_I40E_RX_DESC_TRACE', None)
        if a.runner_library_dir:
            env['LD_LIBRARY_PATH'] = str(a.runner_library_dir)
        manifest['runner_environment'] = {key: env.get(key) for key in
            ('SIMBRICKS_I40E_DMA_TRACE', 'SIMBRICKS_I40E_RX_DESC_TRACE', 'LD_LIBRARY_PATH')}
        runner = Child('runner', runner_cmd, a.out, env)
        children.append(runner)
        wait_sockets([pci, net], runner)
        # Actual committed NET_PEER_LAUNCH.md requires concurrent clients. Do not
        # wait for NET_READY before starting the real native PCI introduction.
        peer = Child('peer', peer_cmd, a.out)
        children.append(peer)
        native = Child('native', native_cmd, a.out)
        children.append(native)
        peer.wait_line(lambda line: line == 'NET_READY')
        native.wait_line(lambda line: line.startswith('I40E_SAME_RUN_READY tick='))
        ready = json.loads((a.out / 'ready.json').read_text())
        require(ready['qena_stat'] is True and ready['send_authorized'] is False,
                'native PIO/HMC setup status missing')
        # Native is now paused on stdin. Snapshot both actual endpoints at this
        # phase fence; a sleep/PIO write completion is never the send gate.
        setup_producer = a.out / 'setup-producer.jsonl'
        shutil.copyfile(producer_file, setup_producer)
        setup_native = None
        if a.case == 'on':
            setup_native = a.out / 'setup-native.jsonl'
            shutil.copyfile(native_file, setup_native)
        _, _, setup_map, setup_joined, setup_summary = verify(
            setup_producer, setup_native, parser, binding,
            str(a.out / 'send-gate.json'), setup_only=True)
        with (a.out / 'send-gate.json').open('x') as f:
            json.dump(dict(ready=ready, setup=setup_summary, lifecycle_map=setup_map,
                           joined=setup_joined, send_authorized=True), f, indent=2)
            f.write('\n')
        send_tick = ((ready['tick'] + 10_000 + 999) // 1000) * 1000
        if reference:
            require(ready['tick'] == reference['ready_tick'], 'off/on modeled ready ticks differ')
            require(send_tick == reference['send_tick'], 'off/on injection schedule differs')
            for field in ('binary_sha256', 'runner_sha256', 'peer_sha256', 'expected_frame_sha256',
                          'runner_runtime_library_sha256'):
                require(manifest[field] == reference[field], 'off/on input identity differs: ' + field)
        manifest.update(ready_tick=ready['tick'], send_tick=send_tick)
        peer.send(f'SEND {send_tick}')
        manifest['send_count'] = 1
        native.send(f'CONTINUE {send_tick}')
        peer.wait_line(lambda line: line == f'SENT {send_tick}')
        require(native.wait(timeout=120) == 0, 'native simulation failed; no reinjection')
        require(any(line.startswith('I40E_SAME_RUN_NATIVE_OK ') for line in native.lines),
                'native actual descriptor/payload proof missing')
        if peer.process.poll() is None:
            peer.send('STOP')
        require(peer.wait(timeout=10) == 0 and 'NET_STOPPED' in peer.lines, 'NET peer cleanup failed')
        if runner.process.poll() is None:
            runner.process.send_signal(signal.SIGINT)
        require(runner.wait(timeout=10) == 0, 'accuracy Runner did not exit cleanly')
        producer, native_trace, mapping, joined, summary = verify(
            producer_file, native_file if a.case == 'on' else None,
            parser, binding, str(a.out / 'lifecycle-map.json'))
        finish(producer, native_trace, mapping, joined, summary, expected,
               (a.out / 'payload.bin').read_bytes(), a.out / 'gem5/stats.txt')
        for name, data in [('lifecycle-map.json', mapping), ('joined.json', joined),
                           ('summary.json', summary)]:
            with (a.out / name).open('x') as f:
                json.dump(data, f, indent=2); f.write('\n')
        manifest.update(summary=summary, producer_counters=producer_counters(runner.lines),
                        wire_shapes=[[r['direction'], r['dma_address'], r['length_bytes']]
                                     for r in producer.records if r['event'] == 'issue'],
                        final_tick=stats(a.out / 'gem5/stats.txt')['finalTick'])
        if reference:
            on_stats = stats(a.reference.parent / 'gem5/stats.txt')
            off_stats = stats(a.out / 'gem5/stats.txt')
            require(on_stats.keys() == off_stats.keys(), 'off/on modeled stat key sets differ')
            import math
            differences = {k: [on_stats[k], off_stats[k]] for k in on_stats
                if on_stats[k] != off_stats[k] and not
                (math.isnan(on_stats[k]) and math.isnan(off_stats[k]))}
            require(not differences, 'off/on modeled statistics differ: ' + repr(differences))
            require(manifest['wire_shapes'] == reference['wire_shapes'], 'off/on ordered wire shapes differ')
            require(manifest['producer_counters'] == reference['producer_counters'],
                    'off/on existing producer counters differ')
            require(not native_file.exists(), 'default off unexpectedly created native trace')
            manifest['off_on_equivalence'] = dict(modeled_stat_values=len(off_stats),
                ordered_wire_shapes=True, producer_counters=True, bytes=True, ticks=True,
                native_default_off_no_file=True)
        manifest['passed'] = True
    except BaseException as exc:
        manifest['failure'] = dict(type=type(exc).__name__, detail=str(exc))
        raise
    finally:
        for child in reversed(children): child.cleanup()
        manifest['process_pids'] = {child.name: child.process.pid for child in children}
        manifest['process_exits'] = {child.name: child.process.returncode for child in children}
        with (a.out / 'manifest.json').open('x') as f:
            json.dump(manifest, f, indent=2); f.write('\n')
        # Socket/SHM files belong solely to this run and all users have exited.
        for path in (pci, net, shm):
            if path.exists(): path.unlink()
    print(json.dumps(manifest.get('summary', {}), sort_keys=True))


if __name__ == '__main__':
    main()
