#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""ONE independent ae7 accuracy Admin ON run, then exactly one c21 analysis."""
import argparse
import collections
import hashlib
import json
import os
from pathlib import Path
import signal
import time

from same_run import Child, wait_sockets, producer_counters
from same_run_verify import load_parser, verify, require

ROOT = Path(__file__).resolve().parents[2]
PARSER_SHA = 'e5c8b83b07401d8125af8a778cb36be4bd027b2955843e1687bc07a506ee3d6d'
NATIVE_SHA = '5fc779bccaa32d36411eaae02a00cf94c7dec84eb7ac05b7147a7dc2b20ba0fe'
NIC_SHA = 'a4a9241b627f5edfbff91396e32e3f71cc5b97daeea69ea3d950e7429a9e15d2'


def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()


def write_json(path, data):
    with path.open('x') as stream:
        json.dump(data, stream, indent=2)
        stream.write('\n')


def analyze(directory, parser, binding):
    # This function is called once, only after all three actual children exit.
    producer, native, mapping, joined, summary = verify(
        directory / 'producer.jsonl', directory / 'native.jsonl', parser, binding,
        str(directory / 'lifecycle-map.json'))
    write_json(directory / 'lifecycle-map.json', mapping)
    write_json(directory / 'joined.json', joined)
    require(not joined['packet_observations'], 'Admin fixture unexpectedly observed a packet')
    controls = joined['control_intents']
    summary['control_resolution'] = dict(total=len(controls),
        matched=sum(c['join_status'] == 'matched' for c in controls),
        unresolved=[dict(control_obs_seq=c['record']['control_obs_seq'],
                         reason=c['unmatched_reason']) for c in controls
                    if c['join_status'] != 'matched'])
    # Save actual coverage, including any gap, before enforcing the gate.
    write_json(directory / 'coverage-before-gate.json', summary)
    require(len(controls) == 2, 'expected two actual Admin controls, not historical eight')
    require([(c['record']['descriptor_index'], c['record']['opcode'],
              c['record']['opcode_name']) for c in controls] ==
            [(0, 1, 'get_version'), (1, 2, None)], 'named/null actual control mismatch')
    require(all(c['join_status'] == 'matched' and c['producer_issue_present'] and
                c['producer_complete_present'] and c['native_key'] is not None
                for c in controls), 'actual control fetch/native completion unresolved')
    # A real fetch may contain both descriptors. Count actual fetch instances;
    # neither require one DMA per control nor invent separate fetch identities.
    summary['resolved_native_descriptor_fetch_instances'] = [list(key) for key in
        sorted({tuple(c['native_key']) for c in controls})]
    payload = (directory / 'admin-readback.bin').read_bytes()
    require(len(payload) == 80 and payload[64:] == bytes(16), 'actual Admin80B/RD16B readback mismatch')
    for index in range(2):
        flags = int.from_bytes(payload[index * 32:index * 32 + 2], 'little')
        opcode = int.from_bytes(payload[index * 32 + 2:index * 32 + 4], 'little')
        retval = int.from_bytes(payload[index * 32 + 6:index * 32 + 8], 'little')
        require(flags & 3 == 3 and not flags & 4 and retval == 0 and opcode == index + 1,
                'actual Admin DD/CMP/status readback failed')
    accepts = [r for r in native.records if r['event'] == 'wire_accept']
    fragments = collections.defaultdict(list)
    for row in native.records:
        if row['event'] == 'fragment_accepted':
            fragments[row['wire_req_id'], row['observed_instance']].append(row)
    for accept in accepts:
        end = 0
        for fragment in sorted(fragments[accept['wire_req_id'], accept['observed_instance']],
                               key=lambda r: r['operation_offset']):
            require(fragment['operation_offset'] == end, 'actual DMA fragment gap/overlap')
            end += fragment['fragment_size']
        require(end == accept['length_bytes'], 'wire op missing actual accepted fragments')
    lookups = [r for r in native.records if r['event'] == 'accepted_cache_lookup']
    event_counts = collections.Counter((r['cache'], r['counter']) for r in lookups)
    modeled = {}
    for line in (directory / 'gem5/stats.txt').read_text().splitlines():
        f = line.split()
        if len(f) >= 2 and f[0].endswith(('.dmaReadRequests', '.dmaWriteRequests')):
            modeled[f[0]] = int(f[1])
    require(bool(modeled) and sum(modeled.values()) == len(lookups) and
            all(modeled.get(cache + '.' + counter) == count
                for (cache, counter), count in event_counts.items()), 'native lookup denominator mismatch')
    summary.update(actual_admin_commands=2, actual_admin_readback_bytes=80,
        rd_source_buffer_bytes_verified=16,
        rd_payload_control_parser_proof='UNAVAILABLE: no independent control-to-payload ref; source guarantee only',
        actual_packet_observations=0, native_accepted_fragments=sum(map(len, fragments.values())),
        native_retry_attempts=sum(r['event'] == 'fragment_rejected_wait_retry' for r in native.records),
        accepted_lookup_events=len(lookups),
        explicitly_unmatched_lookup_events=sum(r['wire_req_id'] is None for r in lookups),
        lookup_counters_equal=True, readback_sha256=sha(directory / 'admin-readback.bin'))
    write_json(directory / 'summary.json', summary)
    return summary


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--child-state', type=Path, required=True)
    a = p.parse_args()
    directory = a.out.resolve()
    require(directory.is_relative_to(ROOT), 'Admin outputs must remain owned')
    directory.mkdir(parents=True, exist_ok=False)
    source_commit = json.loads((ROOT / '.pi/run-on/source-freeze.json').read_text())
    binary = ROOT / 'build/DMA_OBSERVE_MESI/gem5.fast'
    accuracy = json.loads((ROOT / '.pi/accuracy-input/actual-accuracy-handoff.json').read_text())
    nic_root = Path(accuracy['artifact_root'])
    runner_path = nic_root / 'component-i40e/i40e_bm/simb_i40e_bm_accuracy'
    peer = Path('/home/minhuw/cachelens/.pi/workspaces/i40e-dma-trace/net-peer-review-v1/clrx_net_peer')
    libraries = ROOT / '.pi/build/attempt1/runner-runtime'
    require(sha(binary) == NATIVE_SHA and sha(runner_path) == NIC_SHA, 'actual native/ae7 ELF hash mismatch')
    require(accuracy['source']['component_i40e'] == 'ae7d288dc85654591d5ce50c8656bd7a1f9a0960'
            and accuracy['source']['simbricks'] == '32ffdd340bb30d3ec25bbb27bdbe13bbc5b35515'
            and accuracy['accuracy_recipe']['EXTRA_CPPFLAGS'] == '-DSIMBRICKS_ACCURACY',
            'actual ae7 accuracy profile binding mismatch')
    parser = load_parser(ROOT / '.pi/source-inputs/dma_observations.py', PARSER_SHA)
    scope = directory.relative_to(ROOT).as_posix().replace('/', '.')
    binding = parser.Binding('independent-admin-nic0', scope + '.native-process1', scope + '.ae7-process1')
    pci, net, shm = directory / 'p', directory / 'n', directory / 's'
    require(max(len(str(pci)), len(str(net))) < 100, 'Admin socket path too long')
    runner_cmd = [str(runner_path),
        f'listen:{pci}:{shm}:sync=true:latency=1000:sync_interval=1000',
        f'listen:{net}:{shm}:sync=true:latency=1000:sync_interval=1000',
        '0', '029000000001', '/dev/null']
    peer_cmd = [str(peer), str(net), str(shm), '1000', '1000']
    native_cmd = [str(binary), '-d', str(directory / 'gem5'),
        str(ROOT / 'verification/dma-observe/admin_config.py'), '--pci-socket', str(pci),
        '--protocol', 'mesi', '--native-trace', str(directory / 'native.jsonl'),
        '--nic', binding.nic, '--epoch', binding.native_epoch, '--host-log', str(directory / 'host.jsonl'),
        '--admin-output', str(directory / 'admin-readback.bin'), '--final-tick', '50000000']
    manifest = dict(schema='cachelens.independent-admin-on-v1', case='ON_ONLY',
        source_freeze=source_commit, compiled_commit_claim=False,
        native_sha256=NATIVE_SHA, runner_sha256=NIC_SHA, parser_sha256=PARSER_SHA,
        binding=dict(nic=binding.nic, native_epoch=binding.native_epoch, producer_epoch=binding.producer_epoch),
        commands=dict(runner=runner_cmd, peer=peer_cmd, native=native_cmd),
        send_count=0, analysis_invocations=0, off_executed=False, passed=False)
    children = []
    def child_state():
        a.child_state.write_text(json.dumps(dict(process_pids={c.name: c.process.pid for c in children},
            pid_namespace='container; docker top provides host namespace mapping',
            phase='ON_RUNNING', commands=manifest['commands'], binding=manifest['binding'], send_count=0), indent=2) + '\n')
    try:
        env = dict(os.environ, SIMBRICKS_I40E_DMA_TRACE=str(directory / 'producer.jsonl'),
                   LD_LIBRARY_PATH=str(libraries))
        env.pop('SIMBRICKS_I40E_RX_DESC_TRACE', None)
        manifest['runner_environment'] = {k: env.get(k) for k in
            ('SIMBRICKS_I40E_DMA_TRACE', 'LD_LIBRARY_PATH', 'SIMBRICKS_I40E_RX_DESC_TRACE')}
        runner = Child('runner', runner_cmd, directory, env)
        children.append(runner); child_state()
        wait_sockets([pci, net], runner)
        network = Child('peer', peer_cmd, directory)
        children.append(network); child_state()
        native = Child('native', native_cmd, directory)
        children.append(native); child_state()
        network.wait_line(lambda line: line == 'NET_READY')
        require(native.wait(timeout=300) == 0, 'Admin native case failed')
        require(any(line.startswith('I40E_ADMIN_SAME_RUN_NATIVE_OK ') for line in native.lines),
                'actual native Admin completion receipt missing')
        if network.process.poll() is None: network.send('STOP')
        require(network.wait(timeout=15) == 0 and 'NET_STOPPED' in network.lines,
                'NET-only idle sync/STOP cleanup failed')
        require(not any(line.startswith('SENT ') for line in network.lines), 'Admin case must not SEND')
        if runner.process.poll() is None: runner.process.send_signal(signal.SIGINT)
        require(runner.wait(timeout=15) == 0, 'ae7 accuracy Runner exit failed')
        manifest['producer_counters'] = producer_counters(runner.lines)
        manifest['analysis_invocations'] = 1
        summary = analyze(directory, parser, binding)
        manifest.update(summary=summary, passed=True)
    except BaseException as exc:
        manifest['failure'] = dict(type=type(exc).__name__, detail=str(exc))
        raise
    finally:
        for c in reversed(children): c.cleanup()
        manifest['process_pids'] = {c.name: c.process.pid for c in children}
        manifest['process_exits'] = {c.name: c.process.returncode for c in children}
        write_json(directory / 'manifest.json', manifest)
        a.child_state.write_text(json.dumps(dict(process_pids=manifest['process_pids'],
            pid_namespace='container',
            process_exits=manifest['process_exits'], phase='ON_CASE_EXIT', passed=manifest['passed'],
            analysis_invocations=manifest['analysis_invocations'], manifest=str(directory / 'manifest.json')),
            indent=2) + '\n')
        for path in (pci, net, shm):
            if path.exists(): path.unlink()
    print(json.dumps(manifest['summary'], sort_keys=True))


if __name__ == '__main__':
    main()
