#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Verify complete same-run endpoint ledgers; reuse the separately pinned parser."""
import argparse
import collections
import dataclasses
import hashlib
import importlib.util
import json
from pathlib import Path
import sys


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def load_parser(path, expected_hash):
    require(hashlib.sha256(path.read_bytes()).hexdigest() == expected_hash,
            'external parser source hash mismatch')
    spec = importlib.util.spec_from_file_location('same_run_pinned_parser', path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    sys.dont_write_bytecode = True
    spec.loader.exec_module(module)
    return module


def ledger(records, producer):
    active, per_id, used = {}, collections.defaultdict(list), set()
    start, end = ('issue', 'complete') if producer else ('wire_accept', 'wire_completion_publish')
    for row in records:
        if row['event'] not in (start, end):
            continue
        wire = row['wire_req_id']
        identity = row['op_seq'] if producer else row['observed_instance']
        if row['event'] == start:
            require(wire not in active, 'overlapping active wire ID; lifecycle ordering unsupported')
            require(identity not in used, 'duplicate operation/lifecycle identity')
            active[wire] = identity
            used.add(identity)
            per_id[wire].append(row)
        else:
            require(active.pop(wire, None) == identity, 'orphan or wrong-lifecycle completion')
    require(not active, 'inflight endpoint ledger at phase fence')
    return per_id


def verify(producer_path, native_path, parser, binding, evidence_name, setup_only=False):
    producer = parser.load_trace(producer_path, 'producer')
    native = parser.load_trace(native_path, 'native') if native_path else parser.parse_native('')
    producer_ids = ledger(producer.records, True)
    require(bool(producer_ids), 'empty producer ledger')
    if native_path:
        require(all((r['nic'], r['epoch']) == (binding.nic, binding.native_epoch)
                    for r in native.records), 'native scope differs from explicit binding')
        native_ids = ledger(native.records, False)
        require(producer_ids.keys() == native_ids.keys(), 'endpoint wire-ID populations differ')
    else:
        native_ids = {}
    entries = []
    for wire, issues in producer_ids.items():
        if not native_path:
            continue
        accepts = native_ids[wire]
        require(len(issues) == len(accepts), 'incomplete per-ID lifecycle coverage')
        # Pair by causal lifecycle ordinal only after both complete ledgers close.
        # Metadata below may reject this exact key match; it never selects one.
        for ordinal, (issue, accept) in enumerate(zip(issues, accepts), 1):
            require((issue['direction'], issue['dma_address'], issue['length_bytes']) ==
                    (accept['direction'], accept['wire_address'], accept['length_bytes']),
                    'key-joined endpoint operation metadata mismatch')
            entries.append(dict(op_seq=issue['op_seq'], wire_req_id=wire,
                                observed_instance=accept['observed_instance'],
                                per_id_lifecycle_ordinal=ordinal))
    mapping = dict(schema='cachelens.dma.lifecycle-map', version=1,
                   binding=dataclasses.asdict(binding), producer_sha256=producer.sha256,
                   native_sha256=native.sha256,
                   verified_by='native-i40e-same-run.complete-endpoint-ledgers-v1',
                   evidence=evidence_name, entries=entries)
    joined = parser.join(producer, native, binding=binding,
                         lifecycle_map=mapping if native_path else None)
    observations = [r for r in producer.records if r['event'] == 'packet_observation']
    issues = {(r['op_seq'], r['wire_req_id']): r for r in producer.records if r['event'] == 'issue'}
    if setup_only:
        require(not observations, 'packet arrived before actual send gate')
        fetched = [r for r in issues.values() if r['operation'] == 'descriptor_fetch' and
                   r['traffic_class'] == 'descriptor' and r['queue_kind'] == 'rx' and
                   r.get('queue_id') == 1]
        require(len(fetched) == 1 and fetched[0]['descriptor_first_index'] == 0 and
                fetched[0]['descriptor_count'] == 7, 'real queue1 descriptor fetch is not complete')
    for row in native.records:
        require(row.get('producer_op_seq') is None, 'native producer_op_seq must remain absent/null')
    summary = dict(producer_operations=len(issues),
                   native_wire_lifecycles=sum(map(len, native_ids.values())) if native_path else None,
                   complete_endpoint_ledgers=bool(native_path),
                   producer_ledger_closed=True,
                   native_ledger_closed=True if native_path else None,
                   native_ledger_available=bool(native_path),
                   setup_gate_verified=setup_only,
                   packet_observations=len(observations))
    return producer, native, mapping, joined, summary


def finish(producer, native, mapping, joined, summary, expected, actual, stats_path):
    require(expected == actual and len(actual) == 1472, 'actual RX readback is not exact 1472B NET input')
    observations = [r for r in producer.records if r['event'] == 'packet_observation']
    require(len(observations) == 1, 'fixture requires exactly one observed frame; no reinjection')
    packet = observations[0]
    require(packet['observation_point'] == 'rx_frame_received' and packet.get('queue_id') == 1 and
            packet['dma_association_status'] == 'all_issued' and
            packet['identity'] == dict(format='clrx-v1', flow=5, seq=9, **{'global': 581}),
            'real RX observation identity/association mismatch')
    refs = sorted(packet['dma_refs'], key=lambda r: r['frame_range_bytes'][0])
    end, pairs = 0, set()
    for ref in refs:
        start, stop = ref['frame_range_bytes']
        require(start == end and stop > start and ref['byte_relation'] == 'unchanged',
                'packet refs do not prove complete unchanged half-open frame coverage')
        end = stop
        pairs.add((ref['op_seq'], ref['wire_req_id']))
    require(end == 1472, 'packet ref bytes do not cover 1472B')
    payload_issues = {(r['op_seq'], r['wire_req_id']) for r in producer.records
                      if r['event'] == 'issue' and r['traffic_class'] == 'rx_payload'}
    require(pairs == payload_issues, 'RX payload operation lacks an exact packet ref')
    summary.update(payload_bytes_verified=1472, packet_ref_operations=len(pairs),
                   readback_sha256=hashlib.sha256(actual).hexdigest())
    if not native.records:
        summary['native_fragment_and_lookup_coverage'] = 'unavailable_default_off'
        return
    by_native = {(r['wire_req_id'], r['observed_instance']): (r['op_seq'], r['wire_req_id'])
                 for r in mapping['entries']}
    accepted = collections.defaultdict(list)
    for row in native.records:
        if row['event'] == 'fragment_accepted':
            accepted[row['wire_req_id'], row['observed_instance']].append(row)
    for row in native.records:
        if row['event'] != 'wire_accept': continue
        fragments = sorted(accepted[row['wire_req_id'], row['observed_instance']],
                           key=lambda r: r['operation_offset'])
        end = 0
        for fragment in fragments:
            require(fragment['operation_offset'] == end, 'actual Packet fragment gap/overlap')
            end += fragment['fragment_size']
        require(end == row['length_bytes'], 'actual Packet fragments do not cover wire length')
    frame_spans = []
    for key, fragments in accepted.items():
        pair = by_native[key]
        for ref in refs:
            if pair != (ref['op_seq'], ref['wire_req_id']): continue
            low, high = ref['source_range_bytes']
            for fragment in fragments:
                a, b = fragment['operation_offset'], fragment['operation_offset'] + fragment['fragment_size']
                start, stop = max(a, low), min(b, high)
                if start < stop:
                    offset = ref['frame_range_bytes'][0]
                    frame_spans.append([offset + start - low, offset + stop - low])
    end = 0
    for start, stop in sorted(frame_spans):
        require(start == end, 'packet-ref joined actual fragments overlap/gap')
        end = stop
    require(end == 1472, 'actual native payload fragments do not cover frame')
    lookup_rows = [r for r in native.records if r['event'] == 'accepted_cache_lookup']
    counters = collections.Counter((r['cache'], r['counter']) for r in lookup_rows)
    modeled = {}
    for line in stats_path.read_text().splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[0].endswith(('.dmaReadRequests', '.dmaWriteRequests')):
            modeled[fields[0]] = int(fields[1])
    require(bool(modeled), 'missing existing eligible lookup counters')
    require(all(modeled.get(cache + '.' + counter) == count
                for (cache, counter), count in counters.items()) and
            sum(modeled.values()) == len(lookup_rows), 'lookup trace/counter population mismatch')
    summary.update(accepted_fragments=sum(map(len, accepted.values())),
                   payload_fragment_spans=sorted(frame_spans),
                   accepted_lookup_events=len(lookup_rows),
                   explicitly_unmatched_lookups=sum(r['wire_req_id'] is None for r in lookup_rows),
                   lookup_counters_equal=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--producer', type=Path, required=True)
    p.add_argument('--native', type=Path)
    p.add_argument('--parser-source', type=Path, required=True)
    p.add_argument('--parser-sha256', required=True)
    p.add_argument('--nic', required=True)
    p.add_argument('--native-epoch', required=True)
    p.add_argument('--producer-epoch', required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--setup-only', action='store_true')
    p.add_argument('--expected-frame', type=Path)
    p.add_argument('--actual-payload', type=Path)
    p.add_argument('--stats', type=Path)
    a = p.parse_args()
    parser = load_parser(a.parser_source, a.parser_sha256)
    binding = parser.Binding(a.nic, a.native_epoch, a.producer_epoch)
    producer, native, mapping, joined, summary = verify(
        a.producer, a.native, parser, binding, str(a.output), a.setup_only)
    if not a.setup_only:
        require(a.expected_frame and a.actual_payload and a.stats, 'final proof needs bytes/statistics')
        finish(producer, native, mapping, joined, summary, a.expected_frame.read_bytes(),
               a.actual_payload.read_bytes(), a.stats)
    with a.output.open('x') as output:
        json.dump(dict(summary=summary, lifecycle_map=mapping, joined=joined), output, indent=2)
        output.write('\n')
    print(json.dumps(summary, sort_keys=True))


if __name__ == '__main__':
    main()
