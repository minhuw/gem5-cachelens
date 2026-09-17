import os
import sys
import m5
from m5.objects import *
system = System(mem_mode=sys.argv[2] if len(sys.argv) > 2 else 'atomic',
                mem_ranges=[AddrRange('64KiB')])
system.voltage_domain = VoltageDomain()
system.clk_domain = SrcClockDomain(clock='1GHz', voltage_domain=system.voltage_domain)
system.bus = SystemXBar()
system.memory = SimBricksDmaChecker()
system.memory.port = system.bus.mem_side_ports
system.platform = SimBricksTestPlatform()
system.host = GenericPciHost(platform=system.platform, conf_base=0x20000000,
                            conf_size='64KiB', conf_device_bits=8)
system.dev = SimBricksPci(host=system.host, pci_bus=0, pci_dev=1, pci_func=0,
    uxsocket_path=sys.argv[1], semantic_dma=True,
    semantic_dma_trace=os.path.join(m5.options.outdir, 'dma.csv'),
    poll_interval='1ns')
system.dev.pio = system.bus.mem_side_ports
system.dev.dma = system.bus.cpu_side_ports
system.host.pio = system.bus.mem_side_ports
system.system_port = system.bus.cpu_side_ports
root = Root(full_system=False, system=system)
m5.instantiate()
result = m5.simulate(m5.ticks.fromSeconds(1.0))
assert result.getCause() == 'DMA_MEMORY_FLAGS_OK', result.getCause()
print(result.getCause())
