# SPDX-License-Identifier: BSD-3-Clause
"""No CPU/guest: actual PCI peer -> DmaPort -> production Ruby hierarchy."""
import argparse
import importlib.util
from pathlib import Path

import m5
from m5.objects import (
    AddrRange, DMASequencer, DmaObserveTester, GenericPciHost, IOXBar, LRURP,
    Root, RubyCache, RubyPortProxy, RubySystem, SimpleMemory, SimBricksPci,
    SimBricksTestPlatform, SrcClockDomain, System, VoltageDomain,
)

parser = argparse.ArgumentParser()
parser.add_argument('--socket', required=True)
parser.add_argument('--protocol', choices=('mesi', 'chi'), required=True)
parser.add_argument('--trace', default='')
parser.add_argument('--nic', default='directed-pci-0')
parser.add_argument('--atomic', action='store_true')
parser.add_argument('--checkpoint-mode', action='store_true')
args = parser.parse_args()

system = System(
    clk_domain=SrcClockDomain(clock='1GHz', voltage_domain=VoltageDomain()),
    mem_ranges=[AddrRange('64MiB')], cache_line_size=64,
    mem_mode='atomic' if args.atomic else 'timing',
)
system.untagged = DmaObserveTester()
system.memory = SimpleMemory(range=system.mem_ranges[0], latency='30ns')
system.iobus = IOXBar()
system.platform = SimBricksTestPlatform()
system.host = GenericPciHost(
    platform=system.platform, conf_base=0x20000000, conf_size='64KiB',
    conf_device_bits=8, pci_dma_base=0x1000,
)
system.host.pio = system.iobus.mem_side_ports
system.dev = SimBricksPci(
    host=system.host, pci_bus=0, pci_dev=1, pci_func=0,
    uxsocket_path=args.socket, sync=True,
    sync_tx_interval='1ns', link_latency='1ns',
    dma_observe_file=args.trace, dma_observe_nic=args.nic,
    initialized_checkpoint=args.checkpoint_mode,
)
system.dev.pio = system.iobus.mem_side_ports


# Avoid CPU/guest resource imports in the intentionally ISA-free directed build.
# These are existing protocol node constructors, not replacement cache models.
from gem5.prebuilt.cachelens.topology import SimpleCrossbar

system.ruby = RubySystem(cache_trace_warmup=False)
ruby = system.ruby
ruby.randomization = False
ruby.number_of_virtual_networks = 3 if args.protocol == 'mesi' else 4
ruby.network = SimpleCrossbar(ruby, buffer_size=4)
ruby.network.number_of_virtual_networks = ruby.number_of_virtual_networks
ports = [system.dev.dma, system.untagged.dma]

if args.protocol == 'mesi':
    from gem5.components.cachehierarchies.ruby.caches.mesi_three_level.l3_cache import L3Cache
    from gem5.components.cachehierarchies.ruby.caches.mesi_three_level.directory import Directory
    from gem5.components.cachehierarchies.ruby.caches.mesi_three_level.dma_controller import DMAController
    Directory._version = 0
    ruby.llcs = [L3Cache(
        l3_size='4KiB', l3_assoc=4, network=ruby.network, num_l3Caches=2,
        cache_line_size=64, cluster_id=0, ddio_way_part=1, addr_hash=False,
    ) for _ in range(2)]
    ruby.directories = [Directory(ruby.network, 64, system.mem_ranges[0],
                                  system.memory.port)]
    for node in list(ruby.llcs) + list(ruby.directories):
        node.ruby_system = ruby
    ruby.dmas = [DMAController(
        DMASequencer(version=i, in_ports=port, max_outstanding_requests=1,
                     supports_masked_writes=True, ruby_system=ruby), ruby,
        l2_select_num_bits=1,
    ) for i, port in enumerate(ports)]
    for node in ruby.dmas:
        node.ddio_eligible = True
    controllers = list(ruby.llcs) + list(ruby.directories) + list(ruby.dmas)
else:
    # Legacy CHI_config explicitly supports custom configurations without CPUs.
    source = Path(__file__).resolve().parents[2] / 'configs/ruby/CHI_config.py'
    spec = importlib.util.spec_from_file_location('dma_observe_chi_nodes', source)
    nodes = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(nodes)

    class HNFCache(RubyCache):
        size = '4KiB'
        assoc = 4
        dataAccessLatency = 10
        tagAccessLatency = 2
        ddio_way_part = 1
        addr_hash = False
        replacement_policy = LRURP()

    nodes.CHI_HNF.createAddrRanges(system.mem_ranges, 64, [0, 1])
    ruby.hnfs = [nodes.CHI_HNF(i, ruby, HNFCache, None) for i in range(2)]
    ruby.memory_nodes = [nodes.CHI_SNF_MainMem(ruby, None, system.memory)]
    ruby.dmas = [nodes.CHI_RNI_DMA(ruby, port, None) for port in ports]
    homes = [node.cntrl for node in ruby.hnfs]
    memories = [node.cntrl for node in ruby.memory_nodes]
    for node in ruby.hnfs:
        # Match the existing CacheLens inclusive HNF policy parameters.
        node.cntrl.enable_DMT = False
        node.cntrl.enable_DCT = False
        node.cntrl.alloc_on_readunique = True
        node.cntrl.dealloc_on_unique = False
        node.cntrl.dealloc_backinv_unique = True
        node.cntrl.dealloc_backinv_shared = True
        node.cntrl.nic_read_no_allocate = False
        node.cntrl.accept_write_stash = False
        node.setDownstream(memories)
    for node in ruby.dmas:
        node.cntrl.ddio_eligible = True
        node.cntrl.sequencer.max_outstanding_requests = 1
        node.setDownstream(homes)
    controllers = homes + memories + [node.cntrl for node in ruby.dmas]
    for controller in controllers:
        controller.data_channel_size = nodes.NoC_Params.data_width
    ruby.network.control_msg_size = nodes.NoC_Params.cntrl_msg_size
    ruby.network.data_msg_size = nodes.NoC_Params.data_width

ruby.num_of_sequencers = len(ports)
ruby.network.connect(controllers)
ruby.network.setup_buffers()
ruby.sys_port_proxy = RubyPortProxy(ruby_system=ruby)
ruby.sys_port_proxy.pio_request_port = system.iobus.cpu_side_ports
system.system_port = ruby.sys_port_proxy.in_ports

root = Root(full_system=False, system=system)
m5.instantiate()
result = m5.simulate(m5.ticks.fromSeconds(50e-6))
assert result.getCause() == 'simulate() limit reached', result.getCause()
print(f'DMA_OBSERVE_QUIESCENT tick={m5.curTick()} protocol={args.protocol}')
m5.stats.dump()
