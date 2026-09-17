"""Real SimBricks adapter + slow EtherLink directed counted-drop check."""
import argparse
import m5
from m5.objects import EtherLink, LoadGeneratorPcap, Root, SimBricksEthernet

parser = argparse.ArgumentParser()
parser.add_argument('--socket', required=True)
parser.add_argument('--pcap', required=True)
args = parser.parse_args()
root = Root(full_system=False)
root.net = SimBricksEthernet(uxsocket_path=args.socket, shm_path='', listen=False,
                            sync=True, link_latency='500ns', sync_tx_interval='500ns')
root.gen = LoadGeneratorPcap(pcap_filename=args.pcap, start_tick=1,
                            stop_tick=m5.MaxTick, stack_mode='DPDKStack',
                            packet_rate=800_000)
root.link = EtherLink(speed='1Gbps', delay='200us')
root.net.int0 = root.link.int0
root.gen.interface = root.link.int1
m5.instantiate()
m5.simulate(m5.ticks.fromSeconds(.002))
assert root.gen.replayEof() and root.gen.replaySent() == 64
m5.stats.dump()
print('ETHERNET_BACKPRESSURE_DONE')
