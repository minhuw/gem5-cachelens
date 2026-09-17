import os
import sys
import m5
from m5.objects import *
system = System(mem_mode='atomic', mem_ranges=[AddrRange('64MiB')])
system.voltage_domain = VoltageDomain()
system.clk_domain = SrcClockDomain(clock='1GHz', voltage_domain=system.voltage_domain)
system.bus = SystemXBar()
system.memory = SimpleMemory(range=system.mem_ranges[0], latency="10ns")
system.memory.port = system.bus.mem_side_ports
system.platform = SimBricksTestPlatform()
system.host = GenericPciHost(platform=system.platform, conf_base=0x20000000,
                            conf_size='64KiB', conf_device_bits=8)
system.dev = SimBricksPciPortTester(host=system.host, pci_bus=0, pci_dev=1, pci_func=0, listen=True,
    uxsocket_path=os.path.join(m5.options.outdir, 'pci.sock'),
    shm_path=os.path.join(m5.options.outdir, 'pci.shm'),
    failure=sys.argv[1] if len(sys.argv) > 1 else '')
system.dev.pio = system.bus.mem_side_ports
system.dev.dma = system.bus.cpu_side_ports
system.host.pio = system.bus.mem_side_ports
system.system_port = system.bus.cpu_side_ports
root = Root(full_system=False, system=system)
m5.instantiate()
result = m5.simulate(m5.ticks.fromSeconds(10e-6))
assert result.getCause() == 'SIMBRICKS_PCI_PORT_OK', result.getCause()
print(result.getCause())
