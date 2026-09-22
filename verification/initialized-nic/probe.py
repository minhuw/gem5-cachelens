"""Real gem5 drain/save/restore plumbing probe; no CPU or Linux boot claim."""
import argparse
import os
import signal
import m5
from m5.objects import *

parser = argparse.ArgumentParser()
parser.add_argument('--pci', required=True)
parser.add_argument('--eth', required=True)
parser.add_argument('--pcap', required=True)
parser.add_argument('--checkpoint', required=True)
parser.add_argument('--restore', action='store_true')
parser.add_argument('--latency', default='500ns')
parser.add_argument('--rate', type=int, default=1_000_000)
parser.add_argument('--stall-peer', type=int)
parser.add_argument('--start-before-save', action='store_true')
parser.add_argument('--resume-frozen', action='store_true')
args = parser.parse_args()

system = System(mem_mode='atomic', mem_ranges=[AddrRange(0x80000000, size='16MiB')])
system.voltage_domain = VoltageDomain()
system.clk_domain = SrcClockDomain(clock='1GHz', voltage_domain=system.voltage_domain)
system.bus = SystemXBar()
system.memory = SimpleMemory(range=system.mem_ranges[0], latency='10ns')
system.memory.port = system.bus.mem_side_ports
system.platform = SimBricksTestPlatform()
system.host = GenericPciHost(platform=system.platform, conf_base=0x20000000,
                            conf_size='64KiB', conf_device_bits=8)
system.eth = SimBricksEthernet(uxsocket_path=args.eth, shm_path='', sync=True,
                               initialized_checkpoint=True, link_latency=args.latency)
system.dev = SimBricksCheckpointTester(host=system.host, pci_bus=0, pci_dev=1,
    pci_func=0, uxsocket_path=args.pci, sync=True, semantic_dma=True, initialized_checkpoint=True,
    link_latency=args.latency,
    checkpoint_ethernet=system.eth, restore_probe=args.restore)
system.dev.pio = system.bus.mem_side_ports
system.dev.dma = system.bus.cpu_side_ports
system.host.pio = system.bus.mem_side_ports
system.system_port = system.bus.cpu_side_ports
system.source = LoadGeneratorPcap(pcap_filename=args.pcap, deferred_start=True,
    stop_tick=1_000_000_000, stack_mode='DPDKStack', packet_rate=args.rate,
    exit_on_eof=True, eof_drain_delay='1us')
system.source.interface = system.eth.int0
root = Root(full_system=False, system=system)
m5.instantiate(args.checkpoint if args.restore else None)
event = m5.simulate(100_000_000)
assert event.getCause() == 'INITIALIZED_NIC_PROBE_READY', event.getCause()
if args.restore:
    system.source.startReplay()
    m5.simulate(100_000_000)
    assert system.source.replaySent() == 1
    print('INITIALIZED_NIC_GEM5_RESTORE_OK')
else:
    if args.stall_peer:
        os.kill(args.stall_peer, signal.SIGSTOP)
    if args.start_before_save:
        system.source.startReplay()
    before = m5.curTick()
    m5.checkpoint(args.checkpoint)
    assert m5.curTick() > before
    assert system.dev.initializedCheckpointReady()
    assert system.dev.initializedCheckpointFrozen()
    assert system.source.replaySent() == 0
    if args.resume_frozen:
        m5.simulate(1)  # Must fail without resuming a frozen source.
        raise AssertionError('frozen source resumed')
    print('INITIALIZED_NIC_GEM5_SAVE_OK')
