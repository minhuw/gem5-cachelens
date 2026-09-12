# Copyright (c) 2026 The Regents of the University of California
# All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause

"""The modern CacheLens CHI cache hierarchy.

This module preserves the experiment topology used by the legacy CacheLens
configuration: split L1 caches, a private L2 per core, cached HNFs, direct CHI
requestors for every DMA port, and a simple-network crossbar. Checkpoint
restores intentionally start with empty caches because Ruby cache traces do not
preserve CacheLens provenance, placement, replacement, or telemetry state.
"""

import math
from itertools import chain
from typing import (
    Dict,
    List,
    Optional,
)

from m5.objects import (
    LRURP,
    NULL,
    AddrRange,
    CHI_MiscNode_Controller,
    MessageBuffer,
    RubyCache,
    RubyPortProxy,
    RubySequencer,
    RubySystem,
    SrcClockDomain,
)
from m5.objects.SubSystem import SubSystem
from m5.util.convert import toMemorySize

from gem5.coherence_protocol import CoherenceProtocol
from gem5.components.boards.abstract_board import AbstractBoard
from gem5.components.cachehierarchies.abstract_cache_hierarchy import (
    AbstractCacheHierarchy,
)
from gem5.components.cachehierarchies.chi.nodes.abstract_node import (
    AbstractNode,
    TriggerMessageBuffer,
)
from gem5.components.cachehierarchies.chi.nodes.dma_requestor import (
    DMARequestor,
)
from gem5.components.cachehierarchies.chi.nodes.memory_controller import (
    MemoryController,
)
from gem5.components.cachehierarchies.ruby.abstract_ruby_cache_hierarchy import (
    AbstractRubyCacheHierarchy,
)
from gem5.components.processors.abstract_core import AbstractCore
from gem5.isas import ISA
from gem5.utils.override import overrides
from gem5.utils.requires import requires

from .topology import SimpleCrossbar

_PROFILE_ALIASES = {
    "x86": "x86-generic",
    "arm": "arm-generic",
    "intel_ddio": "intel-ddio",
    "x86_intel_ddio": "intel-ddio",
}

# These are intentionally descriptive model profiles, not claims that the
# resulting configuration is calibrated to a particular commercial CPU.
_MODEL_PROFILES = {
    "abstract": {
        "description": "architecture-neutral abstract CHI model",
        "default_ddio_way_part": -1,
        "nic_read_no_allocate": False,
    },
    "x86-generic": {
        "description": "generic x86 CHI model (DDIO disabled by default)",
        "default_ddio_way_part": -1,
        "nic_read_no_allocate": False,
    },
    "arm-generic": {
        "description": "generic ARM CHI model (DDIO disabled by default)",
        "default_ddio_way_part": -1,
        "nic_read_no_allocate": False,
    },
    "intel-ddio": {
        "description": "experimental Intel-style DDIO model",
        "default_ddio_way_part": 2,
        "nic_read_no_allocate": True,
    },
}


class _L1Cache(AbstractNode):
    def __init__(
        self,
        *,
        size: str,
        assoc: int,
        is_icache: bool,
        network,
        core: AbstractCore,
        cache_line_size: int,
        clk_domain,
    ) -> None:
        super().__init__(network, cache_line_size)
        self.cache = RubyCache(
            size=size,
            assoc=assoc,
            start_index_bit=self.getBlockSizeBits(),
            is_icache=is_icache,
            dataAccessLatency=1 if is_icache else 2,
            tagAccessLatency=1,
        )
        self.clk_domain = clk_domain
        self.send_evictions = core.requires_send_evicts()
        self.use_prefetcher = False
        self.prefetcher = NULL
        self.is_HN = False
        self.enable_DMT = False
        self.enable_DCT = False
        self.allow_SD = True
        self.alloc_on_seq_acc = True
        self.alloc_on_seq_line_write = False
        self.alloc_on_readshared = True
        self.alloc_on_readunique = True
        self.alloc_on_readonce = True
        self.alloc_on_writeback = True
        self.alloc_on_atomic = False
        self.dealloc_on_unique = False
        self.dealloc_on_shared = False
        self.dealloc_backinv_unique = True
        self.dealloc_backinv_shared = True
        self.number_of_TBEs = 16
        self.number_of_repl_TBEs = 16
        self.number_of_snoop_TBEs = 4
        self.number_of_DVM_TBEs = 16
        self.number_of_DVM_snoop_TBEs = 4
        self.unify_repl_TBEs = False


class _PrivateL2Cache(AbstractNode):
    def __init__(
        self,
        *,
        size: str,
        assoc: int,
        network,
        cache_line_size: int,
        clk_domain,
    ) -> None:
        super().__init__(network, cache_line_size)
        self.cache = RubyCache(
            size=size,
            assoc=assoc,
            start_index_bit=self.getBlockSizeBits(),
            is_icache=False,
            dataAccessLatency=6,
            tagAccessLatency=2,
        )
        self.clk_domain = clk_domain
        self.sequencer = NULL
        self.send_evictions = False
        self.use_prefetcher = False
        self.prefetcher = NULL
        self.is_HN = False
        self.enable_DMT = False
        self.enable_DCT = False
        self.allow_SD = True
        self.alloc_on_seq_acc = False
        self.alloc_on_seq_line_write = False
        self.alloc_on_readshared = True
        self.alloc_on_readunique = True
        self.alloc_on_readonce = True
        self.alloc_on_writeback = True
        self.alloc_on_atomic = False
        self.dealloc_on_unique = False
        self.dealloc_on_shared = False
        self.dealloc_backinv_unique = True
        self.dealloc_backinv_shared = True
        self.number_of_TBEs = 32
        self.number_of_repl_TBEs = 32
        self.number_of_snoop_TBEs = 16
        self.number_of_DVM_TBEs = 1
        self.number_of_DVM_snoop_TBEs = 1
        self.unify_repl_TBEs = False


class _CachedHNF(AbstractNode):
    def __init__(
        self,
        *,
        size: str,
        assoc: int,
        ddio_way_part: int,
        nic_read_no_allocate: bool,
        addr_hash: bool,
        dealloc_on_unique: bool,
        hnf_inclusion: str,
        addr_ranges: List[AddrRange],
        start_index_bit: int,
        network,
        cache_line_size: int,
        clk_domain,
    ) -> None:
        super().__init__(network, cache_line_size)
        cache_args = {
            "size": size,
            "assoc": assoc,
            "start_index_bit": start_index_bit,
            "dataAccessLatency": 10,
            "tagAccessLatency": 2,
            "ddio_way_part": ddio_way_part,
            "addr_hash": addr_hash,
        }
        if ddio_way_part > 0:
            cache_args["replacement_policy"] = LRURP()
        self.cache = RubyCache(**cache_args)
        self.addr_ranges = addr_ranges
        self.clk_domain = clk_domain
        self.sequencer = NULL
        self.send_evictions = False
        self.use_prefetcher = False
        self.prefetcher = NULL
        self.is_HN = True
        inclusive = hnf_inclusion == "inclusive"
        self.enable_DMT = not inclusive
        self.enable_DCT = not inclusive
        self.nic_read_no_allocate = nic_read_no_allocate
        self.allow_SD = True
        self.alloc_on_seq_acc = False
        self.alloc_on_seq_line_write = False
        self.alloc_on_readshared = True
        self.alloc_on_readunique = inclusive
        self.alloc_on_readonce = True
        self.alloc_on_writeback = True
        self.alloc_on_atomic = True
        self.dealloc_on_unique = False if inclusive else dealloc_on_unique
        self.dealloc_on_shared = False
        self.dealloc_backinv_unique = inclusive
        self.dealloc_backinv_shared = inclusive
        self.number_of_TBEs = 32
        self.number_of_repl_TBEs = 32
        self.number_of_snoop_TBEs = 1
        self.number_of_DVM_TBEs = 1
        self.number_of_DVM_snoop_TBEs = 1
        self.unify_repl_TBEs = False


class _MiscNode(CHI_MiscNode_Controller):
    _version = 0

    @classmethod
    def version_count(cls) -> int:
        version = cls._version
        cls._version += 1
        return version

    def __init__(self, *, ruby_system, network, l1d_caches) -> None:
        super().__init__(
            version=self.version_count(),
            ruby_system=ruby_system,
            mandatoryQueue=MessageBuffer(),
            triggerQueue=TriggerMessageBuffer(),
            retryTriggerQueue=TriggerMessageBuffer(),
            schedRspTriggerQueue=TriggerMessageBuffer(),
            reqRdy=TriggerMessageBuffer(),
            snpRdy=TriggerMessageBuffer(),
        )
        self.transitions_per_cycle = 1024
        self.addr_ranges = [AddrRange(0, size="1KiB")]
        self.number_of_DVM_TBEs = 16
        self.number_of_non_sync_TBEs = 1
        self.early_nonsync_comp = False
        self.upstream_destinations = l1d_caches
        self.data_channel_size = 32
        self._connect_queues(network)

    def _connect_queues(self, network) -> None:
        self.reqOut = MessageBuffer(out_port=network.in_port)
        self.rspOut = MessageBuffer(out_port=network.in_port)
        self.snpOut = MessageBuffer(out_port=network.in_port)
        self.datOut = MessageBuffer(out_port=network.in_port)
        self.reqIn = MessageBuffer(in_port=network.out_port)
        self.rspIn = MessageBuffer(in_port=network.out_port)
        self.snpIn = MessageBuffer(in_port=network.out_port)
        self.datIn = MessageBuffer(in_port=network.out_port)


class CacheLensCHIHierarchy(AbstractRubyCacheHierarchy):
    """A CacheLens-compatible modern CHI cache hierarchy."""

    def __init__(
        self,
        *,
        l1i_size: str = "32KiB",
        l1i_assoc: int = 2,
        l1d_size: str = "64KiB",
        l1d_assoc: int = 2,
        l2_size: str = "1MiB",
        l2_assoc: int = 8,
        hnf_size: str = "1MiB",
        hnf_assoc: int = 8,
        num_hnfs: int = 1,
        ddio_way_part: Optional[int] = None,
        indexing_policy: Optional[str] = None,
        # Retained as a compatibility spelling for older configurations.
        # New configurations should use indexing_policy explicitly.
        addr_hash: Optional[bool] = None,
        dealloc_on_unique: bool = False,
        hnf_inclusion: str = "noninclusive",
        model_profile: str = "abstract",
        core_clock: Optional[str] = None,
        link_latency: int = 1,
        router_latency: int = 1,
        network_buffer_size: int = 4,
    ) -> None:
        requires(coherence_protocol_required=CoherenceProtocol.CHI)
        super().__init__()

        model_profile = _PROFILE_ALIASES.get(model_profile, model_profile)
        if model_profile not in _MODEL_PROFILES:
            choices = ", ".join(sorted(_MODEL_PROFILES))
            raise ValueError(
                f"Unknown model_profile '{model_profile}'; use {choices}."
            )
        if num_hnfs <= 0 or num_hnfs & (num_hnfs - 1):
            raise ValueError("num_hnfs must be a positive power of two.")
        for name, assoc in (
            ("l1i_assoc", l1i_assoc),
            ("l1d_assoc", l1d_assoc),
            ("l2_assoc", l2_assoc),
            ("hnf_assoc", hnf_assoc),
        ):
            if assoc <= 0:
                raise ValueError(f"{name} must be positive.")
        if link_latency <= 0 or router_latency <= 0:
            raise ValueError("Network latencies must be positive.")
        if network_buffer_size <= 0:
            raise ValueError("network_buffer_size must be positive.")
        if hnf_inclusion not in ("noninclusive", "inclusive"):
            raise ValueError(
                "hnf_inclusion must be 'noninclusive' or 'inclusive'."
            )

        if indexing_policy is None:
            indexing_policy = "splitmix64" if addr_hash else "linear"
        if indexing_policy not in ("linear", "splitmix64"):
            raise ValueError(
                "indexing_policy must be 'linear' or 'splitmix64'."
            )
        if addr_hash is not None:
            compatibility_policy = "splitmix64" if addr_hash else "linear"
            if indexing_policy != compatibility_policy:
                raise ValueError(
                    "addr_hash and indexing_policy specify different policies."
                )

        if ddio_way_part is None:
            ddio_way_part = _MODEL_PROFILES[model_profile][
                "default_ddio_way_part"
            ]
        if ddio_way_part != -1 and not 1 <= ddio_way_part <= hnf_assoc:
            raise ValueError(
                "ddio_way_part must be -1 or between 1 and hnf_assoc."
            )
        if ddio_way_part > 0 and model_profile in (
            "arm-generic",
            "x86-generic",
        ):
            raise ValueError(
                "Positive ddio_way_part requires the intel-ddio or abstract "
                "model profile."
            )

        try:
            hnf_size_bytes = toMemorySize(hnf_size)
        except (TypeError, ValueError) as error:
            raise ValueError(
                "hnf_size must be a valid memory size."
            ) from error
        if hnf_size_bytes <= 0:
            raise ValueError("hnf_size must be positive.")

        self._l1i_size = l1i_size
        self._l1i_assoc = l1i_assoc
        self._l1d_size = l1d_size
        self._l1d_assoc = l1d_assoc
        self._l2_size = l2_size
        self._l2_assoc = l2_assoc
        self._hnf_size = hnf_size
        self._hnf_size_bytes = hnf_size_bytes
        self._hnf_assoc = hnf_assoc
        self._num_hnfs = num_hnfs
        self._ddio_way_part = ddio_way_part
        self._nic_read_no_allocate = _MODEL_PROFILES[model_profile][
            "nic_read_no_allocate"
        ]
        self._indexing_policy = indexing_policy
        self._addr_hash = indexing_policy == "splitmix64"
        self._dealloc_on_unique = dealloc_on_unique
        self._hnf_inclusion = hnf_inclusion
        self._model_profile = model_profile
        self._core_clock = core_clock
        self._link_latency = link_latency
        self._router_latency = router_latency
        self._network_buffer_size = network_buffer_size
        self._cache_line_size = 64

    def get_model_profile(self) -> str:
        return self._model_profile

    def get_indexing_policy(self) -> str:
        return self._indexing_policy

    def get_cache_state_restore_policy(self) -> str:
        """Return the explicit checkpoint cache-state restore policy."""
        return "cold"

    def get_hnf_size_per_hnf_bytes(self) -> int:
        return self._hnf_size_bytes

    def get_total_hnf_capacity_bytes(self) -> int:
        return self._hnf_size_bytes * self._num_hnfs

    def validate_architecture(self, isa: ISA) -> None:
        """Reject profiles whose stated architecture is not being modeled."""
        if self._model_profile == "intel-ddio" and isa != ISA.X86:
            raise ValueError("The Intel-DDIO profile is only valid for x86.")
        if self._model_profile == "arm-generic" and isa != ISA.ARM:
            raise ValueError("The arm-generic profile requires ARM.")
        if self._model_profile == "x86-generic" and isa != ISA.X86:
            raise ValueError("The x86-generic profile requires x86.")

    def hnf_index_for_address(self, address: int) -> int:
        """Return the unique HNF selected by the interleaving bits."""
        if address < 0:
            raise ValueError("address must be non-negative.")
        block_bits = int(math.log2(self._cache_line_size))
        return (address >> block_bits) & (self._num_hnfs - 1)

    def get_configuration(self) -> Dict[str, object]:
        """Return the bounded model choices for logs and configuration tests."""
        return {
            "coherence_protocol": "CHI",
            "model_profile": self._model_profile,
            "model_description": _MODEL_PROFILES[self._model_profile][
                "description"
            ],
            "topology": "private L1I/L1D + private L2 + shared HNF",
            "topology_matches_chi_three_level": True,
            "private_data_mapping": "native CHI private L1D and L2",
            "private_data_size": self._l2_size,
            "private_data_assoc": self._l2_assoc,
            "cache_state_restore_policy": (
                self.get_cache_state_restore_policy()
            ),
            "cache_state_continuity": False,
            "indexing_policy": self._indexing_policy,
            "ddio_way_part": self._ddio_way_part,
            "nic_read_no_allocate": self._nic_read_no_allocate,
            "hnf_inclusion": self._hnf_inclusion,
            "num_hnfs": self._num_hnfs,
            "hnf_size_per_hnf": self._hnf_size,
            "hnf_size_per_hnf_bytes": self._hnf_size_bytes,
            "total_hnf_capacity_bytes": self.get_total_hnf_capacity_bytes(),
            "hnf_assoc": self._hnf_assoc,
            "llc_replacement_policy": (
                "LRU" if self._ddio_way_part > 0 else "protocol-default"
            ),
            "cache_line_size": self._cache_line_size,
            "link_latency": self._link_latency,
            "router_latency": self._router_latency,
            "network_buffer_size": self._network_buffer_size,
            "core_clock": self._core_clock or "board",
            "hnf_clock": "board",
        }

    @overrides(AbstractCacheHierarchy)
    def get_coherence_protocol(self):
        return CoherenceProtocol.CHI

    @overrides(AbstractCacheHierarchy)
    def incorporate_cache(self, board: AbstractBoard) -> None:
        super().incorporate_cache(board)
        # Cache traces cannot reproduce CacheLens-specific NIC provenance,
        # exact cache level/way/replacement state, or rxPayloadEverAddrs.
        # Checkpoint writeback has already published authoritative data to
        # backing memory, so restore an empty hierarchy and reset telemetry.
        self.ruby_system = RubySystem(cache_trace_warmup=False)
        self.validate_architecture(board.get_processor().get_isa())
        self._cache_line_size = board.get_cache_line_size()
        if self._core_clock is not None:
            self.core_clk_domain = SrcClockDomain(
                clock=self._core_clock,
                voltage_domain=board.get_clock_domain().voltage_domain,
            )
            # Use the same domain for the CPU and its private cache side.  The
            # assignment does not rename or reparent the CPU SimObjects, which
            # keeps existing checkpoint paths stable when this option is used.
            for core in board.get_processor().get_cores():
                core.get_simobject().clk_domain = self.core_clk_domain
        self.ruby_system.network = SimpleCrossbar(
            self.ruby_system,
            link_latency=self._link_latency,
            router_latency=self._router_latency,
            buffer_size=self._network_buffer_size,
        )
        self.ruby_system.number_of_virtual_networks = 4
        self.ruby_system.network.number_of_virtual_networks = 4
        self.ruby_system.network.control_msg_size = 8
        self.ruby_system.network.data_msg_size = 32
        # SimpleCrossbar already sets this explicitly; retain the assignment
        # here for compatibility with callers that inspect the network before
        # setup_buffers().
        self.ruby_system.network.buffer_size = self._network_buffer_size

        cores = board.get_processor().get_cores()
        self.core_clusters = [
            self._create_core_cluster(core, index, board)
            for index, core in enumerate(cores)
        ]

        hnf_ranges = self._create_hnf_ranges(board)
        self._hnf_ranges = hnf_ranges
        hnf_bits = int(math.log2(self._num_hnfs))
        start_index_bit = (
            int(math.log2(board.get_cache_line_size())) + hnf_bits
        )
        self.hnfs = [
            _CachedHNF(
                size=self._hnf_size,
                assoc=self._hnf_assoc,
                ddio_way_part=self._ddio_way_part,
                nic_read_no_allocate=self._nic_read_no_allocate,
                addr_hash=self._addr_hash,
                dealloc_on_unique=self._dealloc_on_unique,
                hnf_inclusion=self._hnf_inclusion,
                addr_ranges=hnf_ranges[index],
                start_index_bit=start_index_bit,
                network=self.ruby_system.network,
                cache_line_size=board.get_cache_line_size(),
                clk_domain=board.get_clock_domain(),
            )
            for index in range(self._num_hnfs)
        ]

        for cluster in self.core_clusters:
            cluster.l2.downstream_destinations = self.hnfs

        self.memory_controllers = [
            self._create_memory_controller(board, addr_range, port)
            for addr_range, port in board.get_mem_ports()
        ]
        for hnf in self.hnfs:
            hnf.downstream_destinations = self.memory_controllers

        dma_requestors = (
            self._create_dma_requestors(board) if board.has_dma_ports() else []
        )
        if dma_requestors:
            self.dma_requestors = dma_requestors
        else:
            self.__dict__["dma_requestors"] = []

        if board.get_processor().get_isa() == ISA.ARM and len(cores) > 1:
            self._enable_arm_dvm(cores)
            self.misc_nodes = [
                _MiscNode(
                    ruby_system=self.ruby_system,
                    network=self.ruby_system.network,
                    l1d_caches=[cluster.l1d for cluster in self.core_clusters],
                )
            ]
        else:
            self.__dict__["misc_nodes"] = []

        controllers = list(
            chain.from_iterable(
                (cluster.l1i, cluster.l1d, cluster.l2)
                for cluster in self.core_clusters
            )
        )
        controllers += self.hnfs
        controllers += self.memory_controllers
        controllers += self.dma_requestors
        controllers += self.misc_nodes
        for controller in controllers:
            controller.ruby_system = self.ruby_system
            if hasattr(controller, "data_channel_size"):
                controller.data_channel_size = 32

        self.ruby_system.num_of_sequencers = 2 * len(cores) + len(
            self.dma_requestors
        )
        self.ruby_system.network.connect(controllers)
        self.ruby_system.network.setup_buffers()

        self.ruby_system.sys_port_proxy = RubyPortProxy(
            ruby_system=self.ruby_system
        )
        board.connect_system_port(self.ruby_system.sys_port_proxy.in_ports)

    def _create_core_cluster(
        self, core: AbstractCore, core_num: int, board: AbstractBoard
    ) -> SubSystem:
        cluster = SubSystem()
        core_clk_domain = (
            getattr(self, "core_clk_domain", None) or board.get_clock_domain()
        )
        common = {
            "network": self.ruby_system.network,
            "core": core,
            "cache_line_size": board.get_cache_line_size(),
            "clk_domain": core_clk_domain,
        }
        cluster.l1i = _L1Cache(
            size=self._l1i_size,
            assoc=self._l1i_assoc,
            is_icache=True,
            **common,
        )
        cluster.l1d = _L1Cache(
            size=self._l1d_size,
            assoc=self._l1d_assoc,
            is_icache=False,
            **common,
        )
        cluster.l2 = _PrivateL2Cache(
            size=self._l2_size,
            assoc=self._l2_assoc,
            network=self.ruby_system.network,
            cache_line_size=board.get_cache_line_size(),
            clk_domain=core_clk_domain,
        )

        cluster.l1i.sequencer = RubySequencer(
            version=2 * core_num,
            dcache=NULL,
            clk_domain=core_clk_domain,
            ruby_system=self.ruby_system,
        )
        cluster.l1d.sequencer = RubySequencer(
            version=2 * core_num + 1,
            dcache=cluster.l1d.cache,
            clk_domain=core_clk_domain,
            ruby_system=self.ruby_system,
        )
        cluster.l1d.sc_lock_enabled = True

        if board.has_io_bus():
            cluster.l1d.sequencer.connectIOPorts(board.get_io_bus())

        core.connect_icache(cluster.l1i.sequencer.in_ports)
        core.connect_dcache(cluster.l1d.sequencer.in_ports)
        core.connect_walker_ports(
            cluster.l1d.sequencer.in_ports,
            cluster.l1i.sequencer.in_ports,
        )
        if board.get_processor().get_isa() == ISA.X86:
            core.connect_interrupt(
                cluster.l1d.sequencer.interrupt_out_port,
                cluster.l1d.sequencer.in_ports,
            )
        else:
            core.connect_interrupt()

        cluster.l1i.downstream_destinations = [cluster.l2]
        cluster.l1d.downstream_destinations = [cluster.l2]
        return cluster

    def get_hnf_ranges(self):
        """Return the per-HNF interleaved address ranges after incorporation."""
        return getattr(self, "_hnf_ranges", [])

    def _create_hnf_ranges(self, board: AbstractBoard):
        unique_ranges = list(board.get_memory().get_uninterleaved_range())
        if hasattr(board, "realview") and hasattr(board.realview, "bootmem"):
            boot_range = board.realview.bootmem.range
            unique_ranges.insert(
                0,
                AddrRange(
                    start=int(boot_range.start),
                    size=int(boot_range.size()),
                ),
            )

        block_bits = int(math.log2(board.get_cache_line_size()))
        hnf_bits = int(math.log2(self._num_hnfs))
        ranges = []
        for index in range(self._num_hnfs):
            node_ranges = []
            for addr_range in unique_ranges:
                if hnf_bits == 0:
                    node_ranges.append(addr_range)
                else:
                    node_ranges.append(
                        AddrRange(
                            start=addr_range.start,
                            size=addr_range.size(),
                            intlvHighBit=block_bits + hnf_bits - 1,
                            intlvBits=hnf_bits,
                            intlvMatch=index,
                        )
                    )
            ranges.append(node_ranges)
        return ranges

    def _create_memory_controller(self, board, addr_range, port):
        controller = MemoryController(
            self.ruby_system.network, addr_range, port
        )
        controller.ruby_system = self.ruby_system
        return controller

    def _create_dma_requestors(self, board: AbstractBoard):
        requestors = []
        sequence_base = 2 * board.get_processor().get_num_cores()
        for index, port in enumerate(board.get_dma_ports()):
            controller = DMARequestor(
                self.ruby_system.network,
                board.get_cache_line_size(),
                board.get_clock_domain(),
            )
            controller.sequencer = RubySequencer(
                version=sequence_base + index,
                in_ports=port,
                dcache=NULL,
                ruby_system=self.ruby_system,
            )
            controller.ruby_system = self.ruby_system
            controller.sequencer.ruby_system = self.ruby_system
            controller.downstream_destinations = self.hnfs
            requestors.append(controller)
        return requestors

    def _enable_arm_dvm(self, cores) -> None:
        for core in cores:
            cpu = core.get_simobject()
            for decoder in cpu.decoder:
                decoder.dvm_enabled = True

    @overrides(AbstractRubyCacheHierarchy)
    def _reset_version_numbers(self):
        AbstractNode._version = 0
        MemoryController._version = 0
        _MiscNode._version = 0
