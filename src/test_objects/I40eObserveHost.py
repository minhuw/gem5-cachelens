# SPDX-License-Identifier: BSD-3-Clause
"""Test-only PIO/DMA initiator for one ordinary real i40e Runner endpoint."""
from m5.objects.ClockedObject import ClockedObject
from m5.params import Param, RequestPort
from m5.proxy import Parent


class I40eObserveHost(ClockedObject):
    type = 'I40eObserveHost'
    cxx_class = 'gem5::I40eObserveHost'
    cxx_header = 'test_objects/i40e_observe_host.hh'
    system = Param.System(Parent.any, 'System owning real host request ports')
    admin_only = Param.Bool(False, 'Test-only two legal Admin commands, no HMC/RX/NET send')
    pio = RequestPort('Actual timing PCI configuration/BAR requests')
    memory = RequestPort('Actual untagged DmaPort requests into Ruby')
    config_address = Param.Addr(0x20000800, 'BDF 00:01.0 with 8 config bits')
    bar0_address = Param.Addr(0x10000000, 'BAR0 address programmed through PCI host')
    pci_dma_base = Param.Addr(0x100000, 'Must equal GenericPciHost.pci_dma_base')
    expected_frame = Param.String('Exact 1472-byte NET peer input file; no packet parser')
    output_file = Param.String('Exclusive fresh absolute host action JSONL')
    payload_output = Param.String('Exclusive fresh absolute actual RX readback file')
    poll_interval = Param.Latency('1us', 'Real PIO/memory polling interval')
    settle_interval = Param.Latency('10us', 'Delay after queue enable and RX descriptor completion')
    timeout = Param.Latency('2ms', 'Bounded test timeout, fails loudly')
