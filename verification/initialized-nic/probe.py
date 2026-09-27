"""Real gem5 drain/save/restore plumbing probe; no CPU or Linux boot claim."""
import argparse
import os
import signal
import time
import m5
from m5.objects import *

parser = argparse.ArgumentParser()
parser.add_argument('--pci', required=True)
parser.add_argument('--eth', required=True)
parser.add_argument('--pcap', required=True)
parser.add_argument('--checkpoint', required=True)
parser.add_argument('--restore', action='store_true')
parser.add_argument('--mem-size', default='16MiB')
parser.add_argument('--test-startup-delay-seconds', type=int, default=0,
                    help='TEST ONLY: delay after instantiate, before Adapter.startup')
parser.add_argument('--latency', default='500ns')
parser.add_argument('--rate', type=int, default=1_000_000)
parser.add_argument('--stall-peer', type=int)
parser.add_argument('--start-before-save', action='store_true')
parser.add_argument('--resume-frozen', action='store_true')
parser.add_argument('--interrupt-case', choices=['idle', 'queued', 'delivered', 'writeback'])
args = parser.parse_args()
if not 0 <= args.test_startup_delay_seconds <= 3600:
    parser.error('--test-startup-delay-seconds must be 0..3600')

system = System(mem_mode='timing' if args.interrupt_case else 'atomic', mem_ranges=[AddrRange(0x80000000, size=args.mem_size)])
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
    pci_func=0, uxsocket_path=args.pci, sync=True, initialized_checkpoint=True,
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
if args.interrupt_case:
    system.pit = I8254(pio_addr=0x30000000)
    system.ioapic = I82094AA(pio_addr=0x30001000,
        int_latency='1ns' if args.interrupt_case == 'delivered' else '100us')
    system.irqprobe = SimBricksCheckpointInterruptTester(pit=system.pit,
        ioapic=system.ioapic, system=system,
        inject_writeback=args.interrupt_case == 'writeback')
    system.pit.pio = system.bus.mem_side_ports
    system.ioapic.pio = system.bus.mem_side_ports
    system.pit.int_pin = system.ioapic.inputs
    system.ioapic.int_requestor = system.irqprobe.interrupt
m5.instantiate(args.checkpoint if args.restore else None)
if args.test_startup_delay_seconds:
    # Fault injection only: keep the NIC waiting for BOOT before startup runs.
    # This is not a runtime workaround or simulated-time advancement.
    print(f'TEST_ONLY_BEFORE_STARTUP_DELAY seconds={args.test_startup_delay_seconds}', flush=True)
    time.sleep(args.test_startup_delay_seconds)
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
    if args.interrupt_case in ('queued', 'delivered'):
        system.irqprobe.armPit()
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
