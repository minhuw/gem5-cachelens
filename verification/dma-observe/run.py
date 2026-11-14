#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Execute directed real PCI peers, check exact lifecycle/fragment/cache joins."""
import argparse
import collections
import hashlib
import json
import math
from pathlib import Path
import subprocess
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


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
        assert r['schema'] == 'cachelens.gem5.dma' and r['version'] == 1
        assert r['nic'] == 'directed-pci-0' and r['epoch'] == 'directed-fresh-1'
        assert isinstance(r['tick'], int) and r['ticks_per_second'] == 10**12
        if r['event'] == 'wire_accept':
            assert r['wire_req_id'] not in native_active
            native_active[r['wire_req_id']] = r['observed_instance']
            by_instance[r['observed_instance']] = r
        elif r['event'] == 'wire_completion_publish':
            assert native_active.pop(r['wire_req_id']) == r['observed_instance']
    assert not native_active
    # The test peer supplies a complete stream with one endpoint and epoch.
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
    unknown = [r for r in lookups if r.get('unmatched_reason')]
    matched = [r for r in lookups if not r.get('unmatched_reason')]
    assert len(unknown) == 2 and all(
        r['unmatched_reason'] == 'request_has_no_trace_extension' and
        r['wire_req_id'] is None and r['observed_instance'] is None for r in unknown)
    assert all(key(r) in {key(x) for x in accepted} for r in matched)
    assert all(r['lookup_line'] == r['fragment_line'] for r in matched)
    assert all(r['native_state'].startswith('MESI_' if protocol == 'mesi' else 'CHI_')
               for r in lookups)
    assert all(r['data_array_hit'] is None and r['any_cache_hit'] is None
               for r in lookups)
    assert any(r['tag_present'] for r in lookups)
    assert any(not r['tag_present'] for r in lookups)
    # Do not deduplicate cache events: compare every emitted event to the real
    # counter population, allowing modeled replay multiplicity.
    counters = collections.Counter((r['cache'], r['counter']) for r in lookups)
    observed_stats = stats(directory / 'stats.txt')
    for (cache, counter), value in counters.items():
        assert observed_stats[f'{cache}.{counter}'] == value
    actual_total = sum(value for name, value in observed_stats.items()
                       if name.endswith(('.dmaReadRequests', '.dmaWriteRequests')))
    assert actual_total == len(lookups), (actual_total, len(lookups))
    if protocol == 'mesi':
        ruby = [r for r in trace if r['event'] == 'ruby_fragment_enqueued']
        assert len(ruby) == 22
        assert all(r['ruby_physical_address'] == r['fragment_address'] and
                   r['ruby_size'] == r['fragment_size'] for r in ruby)
    return dict(wire_operations=len(accepts), prepared=len(prepared),
                accepted_fragments=len(accepted), rejected_attempts=len(rejected),
                accepted_cache_lookups=len(lookups),
                matched_cache_lookups=len(matched), unmatched_cache_lookups=len(unknown),
                all_counter_events_equal=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gem5', type=Path, required=True)
    parser.add_argument('--protocol', choices=('mesi', 'chi'), required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--lib', type=Path, default=ROOT / '.pi/deps/lib')
    args = parser.parse_args()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    manifest = dict(binary=str(args.gem5.resolve()), binary_sha256=digest(args.gem5),
                    protocol=args.protocol, commands=[], cases={},
                    source_sha256={str(p.relative_to(ROOT)): digest(p) for p in
                        list(HERE.glob('*')) + [ROOT / 'src/mem/dma_trace.hh',
                        ROOT / 'src/mem/dma_trace.cc'] if p.is_file()})
    peer = args.out / 'peer'
    command = ['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-parameter', '-I' + str(args.lib), str(HERE / 'peer.c'),
               str(args.lib / 'libsimbricks.a'), '-pthread', '-o', str(peer)]
    manifest['commands'].append(command)
    subprocess.run(command, check=True)
    try:
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
            manifest['commands'].append(dict(peer=peer_cmd, gem5=command))
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
            manifest['cases'][mode] = dict(gem5_exit=result.returncode,
                                          peer_readback_bytes=400, peer_exit=peer_exit)
        assert not (args.out / 'off/trace.jsonl').exists()
        off, on = stats(args.out / 'off/stats.txt'), stats(args.out / 'on/stats.txt')
        assert off.keys() == on.keys()
        differences = {name: [off[name], on[name]] for name in off if off[name] != on[name] and
                       not (math.isnan(off[name]) and math.isnan(on[name]))}
        (args.out / 'stats-differences.json').write_text(json.dumps(differences, indent=2))
        assert not differences, differences
        # Exact raw producer-independent peer data, operation ordering and ticks.
        assert rows(args.out / 'off/wire.jsonl') == rows(args.out / 'on/wire.jsonl')
        manifest['join_checks'] = check_trace(args.out / 'on', args.protocol)
        manifest['default_off_equivalent_stats'] = len(off)
        manifest['default_off_equivalent_wire_ticks'] = True
        manifest['passed'] = True
    finally:
        (args.out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(manifest['join_checks'], sort_keys=True))


if __name__ == '__main__':
    main()
