"""Asset-free real i40e synchronization/clock progress only; no CPU or traffic."""
import argparse
import m5
from m5.objects import *
from timing_params import add_timing_args, validate_timing, ethernet_params

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--simbricks-pci', required=True)
parser.add_argument('--i40e-network-socket', required=True)
add_timing_args(parser)
a = parser.parse_args()
try:
    validate_timing(a)
except ValueError as e:
    parser.error(str(e))
# The launcher supplies the same explicit URL as the guest config.
from pathlib import Path
from timing_params import pci_url
s = Path(a.i40e_network_socket).parent
if a.simbricks_pci != pci_url(s, a):
    parser.error('PCI URL does not match required synchronized configuration')
m5.ticks.setGlobalFrequency('1THz')
system = System(mem_mode='timing', mem_ranges=[AddrRange('64MiB')])
system.voltage_domain = VoltageDomain()
system.clk_domain = SrcClockDomain(clock='1GHz', voltage_domain=system.voltage_domain)
system.bus = SystemXBar()
system.platform = SimBricksTestPlatform()
system.host = GenericPciHost(platform=system.platform, conf_base=0x20000000,
                            conf_size='64KiB', conf_device_bits=8)
system.dev = SimBricksPci(host=system.host, pci_bus=0, pci_dev=1, pci_func=0, listen=False, sync=True,
    uxsocket_path=str(s / 'pci.sock'), shm_path='',
    link_latency=f'{a.pci_latency_ns}ns', sync_tx_interval=f'{a.sync_interval_ns}ns')
system.dev.pio = system.bus.mem_side_ports
system.dev.dma = system.bus.cpu_side_ports
system.host.pio = system.bus.mem_side_ports
system.memory = SimpleMemory(range=system.mem_ranges[0])
system.memory.port = system.bus.mem_side_ports
system.system_port = system.bus.cpu_side_ports
root = Root(full_system=False, system=system)
root.i40e_network = SimBricksEthernet(**ethernet_params(a.i40e_network_socket, a))
root.probe_link = EtherLink(speed='10Gbps', delay='200us')
root.probe_sink = LoadGenerator(start_tick=0, stop_tick=0)
root.i40e_network.int0 = root.probe_link.int0
root.probe_sink.interface = root.probe_link.int1
m5.instantiate()
result = m5.simulate(100_000_000)  # 100us at 1THz; wall time bounded by launcher.
if result.getCause() != 'simulate() limit reached' or m5.curTick() != 100_000_000:
    raise RuntimeError(f'unexpected exit: {result.getCause()} tick={m5.curTick()}')
print('CACHELENS_SYNC_SMOKE_OK tick=100000000 cause=' + result.getCause())
