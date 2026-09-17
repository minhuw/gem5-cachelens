#!/usr/bin/env python3
"""Compile a directed SimBricks peer; assert real adapter packet/byte accounting."""
import argparse
import subprocess
import tempfile
import time
from pathlib import Path
from make_rss_pcap import frame, write_pcap

HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gem5', type=Path, required=True)
    parser.add_argument('--simbricks-lib', type=Path,
                        default=HERE.parents[2] / 'simbricks/lib')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='gem5-eth-test-') as directory:
        work = Path(directory)
        peer = work / 'peer'
        subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-isystem', str(args.simbricks_lib), str(HERE / 'ethernet_peer.c'),
                        str(args.simbricks_lib / 'libsimbricks.a'), '-pthread',
                        '-o', str(peer)], check=True)
        pcap = work / 'input.pcap'
        write_pcap(pcap, 64, 800_000)
        raw = work / 'frame.bin'
        raw.write_bytes(frame(0))
        socket = work / 'eth.sock'
        with (work / 'peer.log').open('w+') as log:
            process = subprocess.Popen([str(peer), str(socket), str(work / 'eth.shm'), str(raw)],
                                       stdout=log, stderr=subprocess.STDOUT)
            try:
                for _ in range(100):
                    if socket.exists():
                        break
                    assert process.poll() is None, 'peer failed before listen'
                    time.sleep(.01)
                assert socket.exists(), 'peer listen timeout'
                result = subprocess.run([str(args.gem5.resolve()), '-d', str(work / 'out'),
                                         str(HERE / 'ethernet_backpressure.py'),
                                         '--socket', str(socket), '--pcap', str(pcap)],
                                        text=True, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, timeout=60)
                assert result.returncode == 0, result.stdout
                assert 'ETHERNET_BACKPRESSURE_DONE' in result.stdout, result.stdout
                log.seek(0)
                assert 'ETHERNET_PEER_OK packets=64 bytes=94208' in log.read()
                stats = {}
                for line in (work / 'out/stats.txt').read_text().splitlines():
                    fields = line.split()
                    if len(fields) >= 2 and fields[0].startswith('net.Ethernet.'):
                        stats[fields[0].split('.')[-1]] = int(fields[1])
                assert stats == dict(incomingPackets=2, incomingBytes=2944,
                                     acceptedPackets=1, acceptedBytes=1472,
                                     droppedPackets=1, droppedBytes=1472,
                                     outgoingPackets=64, outgoingBytes=94208), stats
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
    print('ETHERNET_BACKPRESSURE_TEST_OK incoming=2 accepted=1 dropped=1 outgoing=64')


if __name__ == '__main__':
    main()
