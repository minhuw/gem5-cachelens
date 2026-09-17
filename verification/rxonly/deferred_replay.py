"""Directed native replay test: real EtherLink forces backpressure, no guest."""
import argparse
import json
import m5
from m5.objects import EtherDump, EtherLink, LoadGeneratorPcap, Root

parser = argparse.ArgumentParser()
parser.add_argument('--pcap', required=True)
parser.add_argument('--mode', choices=('normal', 'twice', 'checkpoint', 'late'), default='normal')
args = parser.parse_args()
root = Root(full_system=False)
root.gen = LoadGeneratorPcap(pcap_filename=args.pcap, deferred_start=True,
                            stop_tick=100 if args.mode == 'late' else m5.MaxTick,
                            stack_mode='DPDKStack', packet_rate=800_000)
root.dump = EtherDump(file='link.pcap', maxlen=1472)
root.link = EtherLink(speed='1Gbps', delay='200us', dump=root.dump)
root.gen.interface = root.link.int0
m5.instantiate()
m5.simulate(1_000_000)
assert root.gen.replaySent() == root.gen.replayAttempts() == 0
assert not root.gen.replayEof()
if args.mode == 'checkpoint':
    m5.checkpoint(m5.options.outdir + '/unsupported.cpt')
    raise AssertionError('deferred checkpoint must fail')
start = m5.curTick()
root.gen.startReplay()
assert root.gen.replayAttempts() == 0
m5.simulate(1)
assert root.gen.replayAttempts() == root.gen.replaySent() == 1
assert m5.curTick() == start + 1
if args.mode == 'twice':
    root.gen.startReplay()
    raise AssertionError('second start must fail')
m5.simulate(m5.ticks.fromSeconds(.002))
gen = root.gen
assert gen.replayEof()
assert gen.replaySent() == gen.replayEofCount() == 64
assert gen.replayRetries() > 0
assert gen.replayAttempts() == gen.replaySent() + gen.replayRetries()
assert start < gen.replayEofTick() <= m5.curTick()
print('DEFERRED_REPLAY_OK ' + json.dumps(dict(sent=gen.replaySent(),
      attempts=gen.replayAttempts(), retries=gen.replayRetries(),
      eof_tick=gen.replayEofTick())))
m5.stats.dump()
