#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Execute directed real PCI peers, check exact lifecycle/fragment/cache joins."""
import argparse
import collections
import json
import math
from pathlib import Path
import subprocess
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def rows(path):
    return [json.loads(line) for line in path.read_text().splitlines()]


def stats(path):
    result = {}
    for line in path.read_text().splitlines():
        f = line.split()
        if len(f) >= 2:
            try:
                value = float(f[1])
            except ValueError:
                continue
            # Wall-clock and process-memory measurements are nondeterministic.
            if f[0].startswith('host'):
                continue
            result[f[0]] = value
    return result


def check_trace(directory, protocol):
    trace = rows(directory / 'trace.jsonl')
    wire = rows(directory / 'wire.jsonl')
    issued = [r for r in wire if r['event'] == 'issue']
    completed = [r for r in wire if r['event'] == 'complete']
    accepts = [r for r in trace if r['event'] == 'wire_accept']
    publishes = [r for r in trace if r['event'] == 'wire_completion_publish']
    assert len(issued) == len(completed) == len(accepts) == len(publishes) == 6
    assert len({r['observed_instance'] for r in accepts}) == 6
    assert [r['observed_instance'] for r in accepts] == list(range(1, 7))
    native_active = {}
    by_instance = {}
    for r in trace:
        assert r['nic'] == 'directed-pci-0' and isinstance(r['tick'], int)
        if r['event'] == 'wire_accept':
            assert r['wire_req_id'] not in native_active
            native_active[r['wire_req_id']] = r['observed_instance']
            by_instance[r['observed_instance']] = r
        elif r['event'] == 'wire_completion_publish':
            assert native_active.pop(r['wire_req_id']) == r['observed_instance']
    assert not native_active
    # The test peer supplies a complete stream with one endpoint.
    # Match per-ID lifecycle ordinal, never nearest address or timestamp.
    native_by_id = collections.defaultdict(list)
    peer_by_id = collections.defaultdict(list)
    for r in accepts: native_by_id[r['wire_req_id']].append(r)
    for r in issued: peer_by_id[r['wire_req_id']].append(r)
    assert native_by_id.keys() == peer_by_id.keys()
    for token, native in native_by_id.items():
        assert len(native) == len(peer_by_id[token])
        for n, p in zip(native, peer_by_id[token]):
            for field in ('wire_req_id', 'wire_address', 'length_bytes', 'direction'):
                assert n[field] == p[field], (field, n, p)
            assert n['pci_to_dma_address'] == n['wire_address'] + 0x1000
    prepared = [r for r in trace if r['event'] == 'fragment_prepared']
    accepted = [r for r in trace if r['event'] == 'fragment_accepted']
    rejected = [r for r in trace if r['event'] == 'fragment_rejected_wait_retry']
    lookups = [r for r in trace if r['event'] == 'accepted_cache_lookup']
    key = lambda r: (r['observed_instance'], r['operation_offset'])
    assert len(prepared) == len(accepted) == 22
    assert len({key(r) for r in accepted}) == len(accepted)
    assert {key(r) for r in prepared} == {key(r) for r in accepted}
    assert rejected, 'directed backpressure did not execute'
    assert all(key(r) in {key(x) for x in prepared} for r in rejected)
    for r in accepted:
        op = by_instance[r['observed_instance']]
        assert r['fragment_address'] == op['pci_to_dma_address'] + r['operation_offset']
        assert r['fragment_line'] == r['fragment_address'] // 64 * 64
        assert r['fragment_size'] + r['fragment_address'] % 64 <= 64
    for instance, op in by_instance.items():
        fragments = sorted((r for r in accepted if r['observed_instance'] == instance),
                           key=lambda r: r['operation_offset'])
        offset = 0
        for f in fragments:
            assert f['operation_offset'] == offset
            offset += f['fragment_size']
        assert offset == op['length_bytes']
    assert lookups, 'no actual accepted cache lookup events'
    assert all(key(r) in {key(x) for x in accepted} for r in lookups)
    assert all(r['lookup_line'] == r['fragment_line'] for r in lookups)
    assert all(r['native_state'].startswith('MESI_' if protocol == 'mesi' else 'CHI_')
               for r in lookups)
    assert any(r['tag_present'] for r in lookups)
    assert any(not r['tag_present'] for r in lookups)
    # Do not deduplicate cache events: compare every emitted event to the real
    # counter population, allowing modeled replay multiplicity.
    # The untagged tester's write and read are counted but not traced.
    counters = collections.Counter((r['cache'], r['counter']) for r in lookups)
    observed_stats = stats(directory / 'stats.txt')
    for (cache, counter), value in counters.items():
        assert observed_stats[f'{cache}.{counter}'] >= value
    actual_total = sum(value for name, value in observed_stats.items()
                       if name.endswith(('.dmaReadRequests', '.dmaWriteRequests')))
    assert actual_total == len(lookups) + 2, (actual_total, len(lookups))
    if protocol == 'mesi':
        ruby = [r for r in trace if r['event'] == 'ruby_fragment_enqueued']
        assert len(ruby) == 22
        assert all(r['ruby_physical_address'] == r['fragment_address'] and
                   r['ruby_size'] == r['fragment_size'] for r in ruby)
    return dict(wire_operations=len(accepts), prepared=len(prepared),
                accepted_fragments=len(accepted), rejected_attempts=len(rejected),
                traced_cache_lookups=len(lookups))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gem5', type=Path, required=True)
    parser.add_argument('--protocol', choices=('mesi', 'chi'), required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--lib', type=Path, required=True, help='SimBricks lib directory with libsimbricks.a')
    args = parser.parse_args()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    peer = args.out / 'peer'
    command = ['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-parameter', '-I' + str(args.lib), str(HERE / 'peer.c'),
               str(args.lib / 'libsimbricks.a'), '-pthread', '-o', str(peer)]
    subprocess.run(command, check=True)
    for mode in ('off', 'on'):
        directory = args.out / mode
        directory.mkdir()
        socket = directory / 'p'
        assert len(str(socket)) < 100
        peer_cmd = [str(peer), str(socket), str(directory / 's'),
                    str(directory / 'wire.jsonl')]
        command = [str(args.gem5.resolve()), '-d', str(directory),
                   str(HERE / 'config.py'), '--socket', str(socket),
                   '--protocol', args.protocol]
        if mode == 'on': command += ['--trace', str(directory / 'trace.jsonl')]
        with (directory / 'peer.log').open('w') as peer_log:
            process = subprocess.Popen(peer_cmd, stdout=peer_log, stderr=subprocess.STDOUT)
            try:
                for _ in range(100):
                    if socket.exists(): break
                    assert process.poll() is None, 'peer failed before listen'
                    time.sleep(.01)
                assert socket.exists(), 'peer socket timeout'
                with (directory / 'gem5.log').open('w') as log:
                    result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                            timeout=120)
                assert result.returncode == 0, (directory / 'gem5.log').read_text()
                assert 'DMA_OBSERVE_QUIESCENT' in (directory / 'gem5.log').read_text()
                assert 'DMA_OBSERVE_UNTAGGED_OK' in (directory / 'gem5.log').read_text()
                peer_exit = process.wait(timeout=5)
                assert peer_exit == 0, (directory / 'peer.log').read_text()
                assert (directory / 'peer.log').read_text() == (
                    'DMA_WIRE_PEER_OK operations=6 readback_bytes=400\n')
            finally:
                process.terminate()
                try: process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait()
    assert not (args.out / 'off/trace.jsonl').exists()
    off, on = stats(args.out / 'off/stats.txt'), stats(args.out / 'on/stats.txt')
    assert off.keys() == on.keys()
    differences = {name: [off[name], on[name]] for name in off if off[name] != on[name] and
                   not (math.isnan(off[name]) and math.isnan(on[name]))}
    assert not differences, differences
    # Exact raw producer-independent peer data, operation ordering and ticks.
    assert rows(args.out / 'off/wire.jsonl') == rows(args.out / 'on/wire.jsonl')
    print(json.dumps(check_trace(args.out / 'on', args.protocol), sort_keys=True))


if __name__ == '__main__':
    main()
