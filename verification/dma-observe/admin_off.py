#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""One native-observation-OFF Admin case and one retained-ON comparison."""
import argparse
import collections
from decimal import Decimal, InvalidOperation
import json
import os
from pathlib import Path
import signal

from admin_run import ROOT, NATIVE_SHA, NIC_SHA, PARSER_SHA, sha, write_json
from same_run import Child, wait_sockets, producer_counters
from same_run_verify import load_parser, ledger, require


def wire_projection(records):
    """Comparison projection only; original IDs remain in each raw ledger.

    ON is read as JSON for this comparison, never semantically parsed/joined again.
    Issue ordinals are local to each run, never a native/producer identity claim.
    """
    ordinals = {}
    wire, controls = [], []
    for row in records:
        if row['event'] == 'issue':
            ordinals[row['op_seq'], row['wire_req_id']] = len(ordinals) + 1
        if row['event'] in ('issue', 'complete'):
            item = {k: v for k, v in row.items() if k not in ('op_seq', 'wire_req_id')}
            item['local_issue_ordinal'] = ordinals[row['op_seq'], row['wire_req_id']]
            wire.append(item)
        elif row['event'] == 'control_intent':
            item = {k: v for k, v in row.items() if k != 'descriptor_fetch_ref'}
            ref = row['descriptor_fetch_ref']
            item['descriptor_fetch_local_issue_ordinal'] = ordinals[ref['op_seq'], ref['wire_req_id']]
            controls.append(item)
    return wire, controls


def stat_tables(path):
    modeled, host = {}, {}
    for line in path.read_text().splitlines():
        fields = line.split()
        if len(fields) < 2:
            continue
        try:
            Decimal(fields[1])
        except InvalidOperation:
            continue
        (host if fields[0].startswith('host') else modeled)[fields[0]] = fields[1]
    return modeled, host


def equal_number(a, b):
    x, y = Decimal(a), Decimal(b)
    return (x.is_nan() and y.is_nan()) or x == y


def compare_once(directory, reference, parser, manifest):
    producer = parser.load_trace(directory / 'producer.jsonl', 'producer')
    ids = ledger(producer.records, True)
    require(bool(ids), 'empty OFF producer ledger')
    entries, per_id = [], collections.Counter()
    for row in producer.records:
        if row['event'] == 'issue':
            per_id[row['wire_req_id']] += 1
            entries.append(dict(op_seq=row['op_seq'], wire_req_id=row['wire_req_id'],
                per_id_lifecycle_ordinal=per_id[row['wire_req_id']]))
    write_json(directory / 'producer-ledger.json', dict(producer_sha256=producer.sha256,
        binding=manifest['binding'], closed=True, entries=entries,
        native_ledger_available=False, native_ledger_closed=None))
    # Raw retained ON records are used solely for ordered comparison. No ON parser,
    # native analyzer, ledger validation or join is invoked here.
    on_records = [json.loads(line) for line in (reference / 'producer.jsonl').read_text().splitlines()]
    on_wire, on_controls = wire_projection(on_records)
    off_wire, off_controls = wire_projection(producer.records)
    on_manifest = json.loads((reference / 'manifest.json').read_text())
    on_stats, on_host = stat_tables(reference / 'gem5/stats.txt')
    off_stats, off_host = stat_tables(directory / 'gem5/stats.txt')
    different_keys = {key: dict(on=on_stats.get(key), off=off_stats.get(key))
        for key in sorted(on_stats.keys() | off_stats.keys())
        if key not in on_stats or key not in off_stats or
        not equal_number(on_stats[key], off_stats[key])}
    stat_table = dict(semantics='baseline378: numeric stats excluding host*; paired NaN equal; exact Decimal values',
        on=on_stats, off=off_stats, masked_host_values=dict(on=on_host, off=off_host),
        different_keys=different_keys)
    write_json(directory / 'stat-comparison.json', stat_table)
    on_bytes = (reference / 'admin-readback.bin').read_bytes()
    off_bytes = (directory / 'admin-readback.bin').read_bytes()
    byte_differences = [i for i in range(max(len(on_bytes), len(off_bytes)))
        if on_bytes[i:i + 1] != off_bytes[i:i + 1]]
    native_file = directory / 'native.jsonl'
    packets = [r for r in producer.records if r['event'] == 'packet_observation']
    controls = [r for r in producer.records if r['event'] == 'control_intent']
    comparison = dict(schema='cachelens.independent-admin-off-on-comparison-v1',
        on_manifest_sha256=sha(reference / 'manifest.json'),
        on_producer_sha256=sha(reference / 'producer.jsonl'), off_producer_sha256=producer.sha256,
        comparison_identity_policy='Per-run issue ordinals compare event order only; no cross-run wire-ID equality or native identity inference',
        comparison_masks=dict(wire=['process-local wire_req_id and op_seq values replaced by local issue ordinal; raw IDs retained'],
            stats=['numeric host* measurements only; raw values retained separately'],
            environment=['fresh output/socket/SHM paths and native/producer epochs']),
        flag_difference=dict(native_observation_on='--native-trace <ON/native.jsonl>',
            native_observation_off='--native-trace omitted; unchanged config default empty',
            producer_trace='SIMBRICKS_I40E_DMA_TRACE enabled in BOTH cases'),
        ordered_wire=dict(on=on_wire, off=off_wire, equal=on_wire == off_wire),
        ordered_controls=dict(on=on_controls, off=off_controls, equal=on_controls == off_controls),
        producer_counters=dict(on=on_manifest['producer_counters'], off=manifest['producer_counters'],
            equal=on_manifest['producer_counters'] == manifest['producer_counters']),
        data=dict(on_length=len(on_bytes), off_length=len(off_bytes), byte_difference_offsets=byte_differences,
            on_sha256=sha(reference / 'admin-readback.bin'), off_sha256=sha(directory / 'admin-readback.bin'),
            equal=on_bytes == off_bytes and len(off_bytes) == 80),
        modeled_stats=dict(on_value_count=len(on_stats), off_value_count=len(off_stats), different_keys=different_keys,
            equal=not different_keys, full_tables=str(directory / 'stat-comparison.json')),
        final_ticks=dict(on=on_stats.get('finalTick'), off=off_stats.get('finalTick'),
            equal=on_stats.get('finalTick') == off_stats.get('finalTick') == '50000000'),
        native_default_off_no_file=not native_file.exists(),
        native_fragment_and_lookup_coverage='unavailable_default_off',
        on_native_coverage=on_manifest['summary'],
        off_population=dict(producer_operations=len(entries), producer_ledger_closed=True,
            control_intents=len(controls), packet_observations=len(packets),
            native_ledger_available=False, native_ledger_closed=None,
            native_lookup_count=None, native_accepted_fragment_count=None),
        rd_payload_control_parser_proof='UNAVAILABLE:16B readback is source/runtime evidence, no independent control-to-payload ref',
        passed=False)
    write_json(directory / 'comparison-before-gate.json', comparison)
    require(comparison['native_default_off_no_file'], 'OFF unexpectedly created a native trace')
    require(not packets and not on_manifest['summary']['packet_observations'], 'Admin pair must have no packet observations')
    require([(c['opcode'], c['opcode_name']) for c in controls] == [(1, 'get_version'), (2, None)],
            'OFF named/supported-null control sequence differs')
    for key in ('ordered_wire', 'ordered_controls', 'producer_counters', 'data', 'modeled_stats', 'final_ticks'):
        require(comparison[key]['equal'], 'OFF/ON mismatch: ' + key + '; see complete saved comparison')
    require(off_bytes[64:] == bytes(16), 'OFF RD16 source changed')
    comparison['passed'] = True
    write_json(directory / 'pair-summary.json', comparison)
    return comparison


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--child-state', type=Path, required=True)
    a = p.parse_args()
    directory = a.out.resolve()
    require(directory == ROOT / '.pi/off/a1', 'one fresh authorized OFF directory only')
    directory.mkdir(parents=True, exist_ok=False)
    reference = ROOT / '.pi/on/a1'
    review = json.loads((ROOT / '.pi/run-on/parent-review.json').read_text())
    require(review['verdict'] == 'ON_REQUIREMENTS_MET_AUTHORIZED_ONE_OFF_MAY_PROCEED', 'parent ON gate missing')
    on = json.loads((reference / 'manifest.json').read_text())
    freeze = json.loads((ROOT / '.pi/run-off/source-freeze.json').read_text())
    require(on['passed'] and on['analysis_invocations'] == 1, 'retained ON proof unavailable')
    parser = load_parser(ROOT / '.pi/source-inputs/dma_observations.py', PARSER_SHA)
    binding = dict(nic=on['binding']['nic'], native_epoch='.pi.off.a1.native-process1',
                   producer_epoch='.pi.off.a1.ae7-process1')
    commands = {key: [word.replace(str(reference), str(directory)) for word in value]
                for key, value in on['commands'].items()}
    native_cmd = commands['native']
    index = native_cmd.index('--native-trace')
    del native_cmd[index:index + 2]
    native_cmd[native_cmd.index('--epoch') + 1] = binding['native_epoch']
    require(sha(Path(native_cmd[0])) == NATIVE_SHA and sha(Path(commands['runner'][0])) == NIC_SHA,
            'OFF must reuse actual native/ae7 accuracy ELF bytes')
    for name, expected in on['source_freeze']['runtime_source_sha256'].items():
        require(sha(ROOT / name) == expected, 'frozen ON config/helper changed: ' + name)
    manifest = dict(schema='cachelens.independent-admin-off-v1', case='NATIVE_OBSERVATION_OFF_ONLY',
        source_freeze=freeze, compiled_commit_claim=False, binding=binding,
        native_sha256=NATIVE_SHA, runner_sha256=NIC_SHA, parser_sha256=PARSER_SHA,
        commands=commands, native_observation=False, producer_trace=True,
        reference_manifest=str(reference / 'manifest.json'), passed=False, analysis_invocations=0,
        on_analysis_invocations=0, native_parse_invocations=0, native_join_invocations=0, send_count=0)
    children = []
    def child_state(phase):
        a.child_state.write_text(json.dumps(dict(process_pids={c.name: c.process.pid for c in children},
            pid_namespace='container', phase=phase, commands=commands, binding=binding), indent=2) + '\n')
    try:
        env = dict(os.environ, SIMBRICKS_I40E_DMA_TRACE=str(directory / 'producer.jsonl'),
            LD_LIBRARY_PATH=str(ROOT / '.pi/build/attempt1/runner-runtime'))
        env.pop('SIMBRICKS_I40E_RX_DESC_TRACE', None)
        manifest['runner_environment'] = {key: env.get(key) for key in
            ('SIMBRICKS_I40E_DMA_TRACE', 'LD_LIBRARY_PATH', 'SIMBRICKS_I40E_RX_DESC_TRACE')}
        runner = Child('runner', commands['runner'], directory, env)
        children.append(runner); child_state('OFF_RUNNING')
        wait_sockets([directory / 'p', directory / 'n'], runner)
        peer = Child('peer', commands['peer'], directory)
        children.append(peer); child_state('OFF_RUNNING')
        native = Child('native', native_cmd, directory)
        children.append(native); child_state('OFF_RUNNING')
        peer.wait_line(lambda line: line == 'NET_READY')
        require(native.wait(timeout=300) == 0, 'OFF native Admin case failed')
        require(any(line.startswith('I40E_ADMIN_SAME_RUN_NATIVE_OK ') for line in native.lines),
                'OFF actual Admin completion receipt missing')
        if peer.process.poll() is None: peer.send('STOP')
        require(peer.wait(timeout=15) == 0 and 'NET_STOPPED' in peer.lines, 'OFF NET sync/STOP failed')
        require(not any(line.startswith('SENT ') for line in peer.lines), 'Admin OFF must not SEND')
        if runner.process.poll() is None: runner.process.send_signal(signal.SIGINT)
        require(runner.wait(timeout=15) == 0, 'OFF ae7 accuracy Runner exit failed')
        manifest['producer_counters'] = producer_counters(runner.lines)
        manifest['analysis_invocations'] = 1
        result = compare_once(directory, reference, parser, manifest)
        manifest.update(passed=True, pair_summary=str(directory / 'pair-summary.json'),
            off_population=result['off_population'])
    except BaseException as exc:
        manifest['failure'] = dict(type=type(exc).__name__, detail=str(exc))
        raise
    finally:
        for child in reversed(children): child.cleanup()
        manifest['process_pids'] = {c.name: c.process.pid for c in children}
        manifest['process_exits'] = {c.name: c.process.returncode for c in children}
        write_json(directory / 'manifest.json', manifest)
        child_state('OFF_CASE_EXIT')
        for path in (directory / 'p', directory / 'n', directory / 's'):
            if path.exists(): path.unlink()
    print('ADMIN_OFF_ON_PAIR_PASS summary=' + str(directory / 'pair-summary.json'), flush=True)


if __name__ == '__main__':
    main()
