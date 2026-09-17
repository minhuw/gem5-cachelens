#!/usr/bin/env python3
"""Run fresh/deferred native replay checks and the unchanged legacy suite."""
import argparse
import subprocess
import tempfile
from pathlib import Path
from make_rss_pcap import decode_frame, read_pcap, write_pcap

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gem5', type=Path, required=True)
    parser.add_argument('--skip-legacy', action='store_true')
    args = parser.parse_args()
    binary = args.gem5.resolve()
    with tempfile.TemporaryDirectory(prefix='gem5-rss-test-') as directory:
        work = Path(directory)
        pcap = work / 'input.pcap'
        write_pcap(pcap, 64, 800_000)
        for mode, diagnostic in (
                ('normal', 'DEFERRED_REPLAY_OK'),
                ('twice', 'startReplay requires an inactive'),
                ('checkpoint', 'Deferred PCAP replay does not support checkpoints'),
                ('late', 'startReplay must precede stop_tick')):
            out = work / mode
            result = subprocess.run([str(binary), '-d', str(out),
                                     str(HERE / 'deferred_replay.py'),
                                     '--pcap', str(pcap), '--mode', mode],
                                    text=True, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, timeout=60)
            assert (result.returncode == 0) == (mode == 'normal'), result.stdout
            assert diagnostic in result.stdout, result.stdout
            if mode == 'normal':
                records = read_pcap(out / 'link.pcap')
                assert [decode_frame(frame)[2] for _, frame in records] == list(range(64))
                # Existing EtherLink serializes 1472B (+ one tick) at 1Gbps;
                # EtherDump timestamps quantize down to microseconds.
                assert all(b[0] - a[0] >= 11_000 for a, b in zip(records, records[1:]))
            print(f'PASS deferred-{mode}')
    if not args.skip_legacy:
        subprocess.run(['python3', str(SOURCE / 'tests/gem5/cachelens/run_pcap_replay_tests.py'),
                        '--gem5', str(binary), '--source', str(SOURCE)], check=True)
    print('NATIVE_RSS_TESTS_OK')


if __name__ == '__main__':
    main()
