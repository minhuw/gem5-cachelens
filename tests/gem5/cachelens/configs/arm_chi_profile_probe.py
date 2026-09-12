# Copyright (c) 2026 The Regents of the University of California
# All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause

"""Offline Python graph checks only: no guest download, instantiate, or boot."""

import argparse
from pathlib import Path

from gem5.components.memory import DualChannelDDR4_2400
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.isas import ISA
from gem5.prebuilt.cachelens.board import CacheLensArmBoard
from gem5.prebuilt.cachelens.cache_hierarchy import CacheLensCHIHierarchy

parser = argparse.ArgumentParser()
parser.add_argument(
    "--model-profile",
    choices=("arm-generic", "abstract", "intel-ddio"),
    default="arm-generic",
)
parser.add_argument(
    "--hnf-inclusion", choices=("noninclusive", "inclusive"), default="noninclusive"
)
args = parser.parse_args()
hierarchy = CacheLensCHIHierarchy(
    model_profile=args.model_profile,
    ddio_way_part=2 if args.model_profile == "abstract" else None,
    hnf_inclusion=args.hnf_inclusion,
    num_hnfs=2,
)
board = CacheLensArmBoard(
    clk_freq="1GHz",
    processor=SimpleProcessor(cpu_type=CPUTypes.TIMING, isa=ISA.ARM, num_cores=2),
    memory=DualChannelDDR4_2400(size="2GiB"),
    cache_hierarchy=hierarchy,
    network_options={"num_nics": 1, "num_loadgens": 1},
)


class _DiskImage:
    def get_local_path(self):
        return Path(__file__).as_posix()


# FS setup creates the platform and DMA list; attach PCI before incorporation.
# Do not call _pre_instantiate(): ARM would generate a DTB/setup bootloaders.
board._set_fullsystem(True)
board._add_disk_to_board(_DiskImage())
try:
    board._connect_things()
except ValueError as error:
    assert args.model_profile == "intel-ddio", str(error)
    assert "only valid for x86" in str(error)
    print("CACHELENS_ARM_CHI_REJECT_OK profile=intel-ddio")
    raise SystemExit(0)
assert args.model_profile != "intel-ddio", "ARM accepted Intel-DDIO"

configuration = hierarchy.get_configuration()
assert configuration["coherence_protocol"] == "CHI"
assert configuration["model_profile"] == args.model_profile
assert configuration["hnf_inclusion"] == args.hnf_inclusion
assert not configuration["nic_read_no_allocate"]
assert configuration["ddio_way_part"] == (2 if args.model_profile == "abstract" else -1)
assert len(hierarchy.core_clusters) == len(hierarchy.hnfs) == 2
for level in ("l1i", "l1d", "l2"):
    assert getattr(hierarchy.core_clusters[0], level) is not getattr(hierarchy.core_clusters[1], level)
for cluster in hierarchy.core_clusters:
    assert cluster.l1i is not cluster.l1d
    assert list(cluster.l1i.downstream_destinations) == [cluster.l2]
    assert list(cluster.l1d.downstream_destinations) == [cluster.l2]
    assert list(cluster.l2.downstream_destinations) == list(hierarchy.hnfs)
for hnf in hierarchy.hnfs:
    inclusive = args.hnf_inclusion == "inclusive"
    assert bool(hnf.is_HN) and not bool(hnf.nic_read_no_allocate)
    assert int(hnf.cache.ddio_way_part) == configuration["ddio_way_part"]
    if args.model_profile == "abstract":
        assert hnf.cache.replacement_policy.type == "LRURP"
    assert bool(hnf.alloc_on_readunique) == inclusive
    assert bool(hnf.dealloc_backinv_unique) == bool(hnf.dealloc_backinv_shared) == inclusive
    assert bool(hnf.enable_DMT) == bool(hnf.enable_DCT) == (not inclusive)
    assert list(hnf.downstream_destinations) == list(hierarchy.memory_controllers)
assert board.get_nic_bdfs() == ["0000:00:02.0"]
dma_ports = board.get_dma_ports()
assert board._pci_devices[0].dma in dma_ports and board.nics[0].dma in dma_ports
assert len(hierarchy.dma_requestors) == len(dma_ports)
for index, (requestor, port) in enumerate(zip(hierarchy.dma_requestors, dma_ports)):
    assert requestor.sequencer.in_ports[0].peer is port
    assert int(requestor.sequencer.version) == 4 + index
    assert list(requestor.downstream_destinations) == list(hierarchy.hnfs)
assert len(hierarchy.misc_nodes) == 1
assert hierarchy.misc_nodes[0] in [link.ext_node for link in hierarchy.ruby_system.network.ext_links]
assert list(hierarchy.misc_nodes[0].upstream_destinations) == [
    cluster.l1d for cluster in hierarchy.core_clusters
]
for core in board.get_processor().get_cores():
    assert all(bool(decoder.dvm_enabled) for decoder in core.get_simobject().decoder)
print(f"CACHELENS_ARM_CHI_GRAPH_OK profile={args.model_profile} inclusion={args.hnf_inclusion}")
