# Copyright (c) 2026 The Regents of the University of California
# All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause

"""The CacheLens x86 MESI_Two_Level hierarchy.

MESI_Two_Level has a private instruction cache and private data cache per core,
followed by an inclusive shared L2. It does not have CHI's private L2 plus HNF
three-level topology. For CacheLens comparisons, the existing ``l2_size`` and
``l2_assoc`` experiment inputs deliberately configure the MESI private data
cache, while ``hnf_size`` and ``hnf_assoc`` configure each shared L2 LLC bank.
"""

import math
from typing import (
    Dict,
    Optional,
)

from m5.objects import (
    RubySystem,
    SrcClockDomain,
)
from m5.util.convert import toMemorySize

from gem5.coherence_protocol import CoherenceProtocol
from gem5.components.boards.abstract_board import AbstractBoard
from gem5.components.cachehierarchies.ruby.mesi_two_level_cache_hierarchy import (
    MESITwoLevelCacheHierarchy,
)
from gem5.components.cachehierarchies.ruby.topologies.simple_pt2pt import (
    SimplePt2Pt,
)
from gem5.isas import ISA
from gem5.utils.override import overrides
from gem5.utils.requires import requires

from ._model import (
    MODEL_PROFILES,
    PROFILE_ALIASES,
)


class CacheLensMESITwoLevelHierarchy(MESITwoLevelCacheHierarchy):
    """A CacheLens-compatible x86 MESI_Two_Level hierarchy.

    This class intentionally wraps the stdlib MESI controllers and point-to-
    point network. The CacheLens ``l2_*`` inputs map to MESI's private L1D
    capacity because this protocol has no separate private L2. The ``hnf_*``
    inputs map to the inclusive shared MESI L2 banks used as the LLC.
    """

    def __init__(
        self,
        *,
        l1i_size: str = "32KiB",
        l1i_assoc: int = 2,
        l2_size: str = "1MiB",
        l2_assoc: int = 8,
        hnf_size: str = "1MiB",
        hnf_assoc: int = 8,
        num_hnfs: int = 1,
        ddio_way_part: Optional[int] = None,
        model_profile: str = "abstract",
        core_clock: Optional[str] = None,
        link_latency: int = 1,
        router_latency: int = 1,
        network_buffer_size: int = 4,
    ) -> None:
        requires(coherence_protocol_required=CoherenceProtocol.MESI_TWO_LEVEL)

        model_profile = PROFILE_ALIASES.get(model_profile, model_profile)
        if model_profile not in MODEL_PROFILES:
            choices = ", ".join(sorted(MODEL_PROFILES))
            raise ValueError(
                f"Unknown model_profile '{model_profile}'; use {choices}."
            )
        if model_profile == "arm-generic":
            raise ValueError(
                "CacheLensMESITwoLevelHierarchy is x86-only; "
                "arm-generic is not supported."
            )
        if num_hnfs <= 0 or num_hnfs & (num_hnfs - 1):
            raise ValueError("num_hnfs must be a positive power of two.")
        for name, assoc in (
            ("l1i_assoc", l1i_assoc),
            ("l2_assoc", l2_assoc),
            ("hnf_assoc", hnf_assoc),
        ):
            if assoc <= 0:
                raise ValueError(f"{name} must be positive.")
        if link_latency <= 0 or router_latency <= 0:
            raise ValueError("Network latencies must be positive.")
        if network_buffer_size <= 0:
            raise ValueError("network_buffer_size must be positive.")

        if ddio_way_part is None:
            ddio_way_part = MODEL_PROFILES[model_profile][
                "default_ddio_way_part"
            ]
        if ddio_way_part != -1 and not 1 <= ddio_way_part <= hnf_assoc:
            raise ValueError(
                "ddio_way_part must be -1 or between 1 and hnf_assoc."
            )
        if ddio_way_part > 0 and model_profile == "x86-generic":
            raise ValueError(
                "Positive ddio_way_part requires the intel-ddio or abstract "
                "model profile."
            )

        cache_line_size = 64
        l1i_size_bytes = self._validate_cache_geometry(
            "l1i_size", l1i_size, l1i_assoc, cache_line_size
        )
        private_data_size_bytes = self._validate_cache_geometry(
            "l2_size", l2_size, l2_assoc, cache_line_size
        )
        hnf_size_bytes = self._validate_cache_geometry(
            "hnf_size", hnf_size, hnf_assoc, cache_line_size
        )

        # MESI_Two_Level naming is one level shallower than CHI. The stdlib
        # L1D parameters below are intentionally sourced from CacheLens l2_*;
        # the stdlib L2 parameters are the shared inclusive LLC geometry.
        super().__init__(
            l1i_size=l1i_size,
            l1i_assoc=l1i_assoc,
            l1d_size=l2_size,
            l1d_assoc=l2_assoc,
            l2_size=hnf_size,
            l2_assoc=hnf_assoc,
            num_l2_banks=num_hnfs,
            ddio_way_part=ddio_way_part,
        )

        self._cachelens_l1i_size = l1i_size
        self._cachelens_l1i_assoc = l1i_assoc
        self._cachelens_l1i_size_bytes = l1i_size_bytes
        self._private_data_size = l2_size
        self._private_data_assoc = l2_assoc
        self._private_data_size_bytes = private_data_size_bytes
        self._hnf_size = hnf_size
        self._hnf_assoc = hnf_assoc
        self._hnf_size_bytes = hnf_size_bytes
        self._num_hnfs = num_hnfs
        self._model_profile = model_profile
        self._core_clock = core_clock
        self._link_latency = link_latency
        self._router_latency = router_latency
        self._network_buffer_size = network_buffer_size
        self._cache_line_size = cache_line_size

    @staticmethod
    def _validate_cache_geometry(
        name: str, size: str, assoc: int, cache_line_size: int
    ) -> int:
        try:
            size_bytes = toMemorySize(size)
        except (TypeError, ValueError) as error:
            raise ValueError(f"{name} must be a valid memory size.") from error
        if size_bytes <= 0:
            raise ValueError(f"{name} must be positive.")
        way_bytes = assoc * cache_line_size
        if size_bytes % way_bytes:
            raise ValueError(
                f"{name} must be divisible by assoc * {cache_line_size} bytes."
            )
        sets = size_bytes // way_bytes
        if sets <= 1 or sets & (sets - 1):
            raise ValueError(
                f"{name} must provide a power-of-two number of cache sets "
                "greater than one."
            )
        return size_bytes

    @overrides(MESITwoLevelCacheHierarchy)
    def incorporate_cache(self, board: AbstractBoard) -> None:
        self.validate_architecture(board.get_processor().get_isa())
        if int(board.get_cache_line_size()) != self._cache_line_size:
            raise ValueError(
                "CacheLens MESI_Two_Level requires 64-byte cache lines."
            )
        super().incorporate_cache(board)

        # Public, non-owning Python views. Keep the stdlib-owned SimObject
        # names under ruby_system deterministic for config/stat aggregation.
        self.__dict__["l1_controllers"] = self._l1_controllers
        self.__dict__["l2_controllers"] = self._l2_controllers
        self.__dict__["directory_controllers"] = self._directory_controllers
        self.__dict__["dma_controllers"] = self._dma_controllers

    def _create_ruby_system(self) -> RubySystem:
        # CacheLens checkpoints publish authoritative cache data to backing
        # memory, validate the trace on restore, and intentionally start cold.
        return RubySystem(cache_trace_warmup=False)

    def _create_network(self) -> SimplePt2Pt:
        return SimplePt2Pt(
            self.ruby_system,
            link_latency=self._link_latency,
            router_latency=self._router_latency,
            buffer_size=self._network_buffer_size,
        )

    def _get_core_clock_domain(self, board: AbstractBoard):
        if self._core_clock is None:
            return board.get_clock_domain()
        self.core_clk_domain = SrcClockDomain(
            clock=self._core_clock,
            voltage_domain=board.get_clock_domain().voltage_domain,
        )
        for core in board.get_processor().get_cores():
            core.get_simobject().clk_domain = self.core_clk_domain
        return self.core_clk_domain

    def validate_architecture(self, isa: ISA) -> None:
        if isa != ISA.X86:
            raise ValueError(
                "CacheLens MESI_Two_Level is currently supported only on x86."
            )

    def get_model_profile(self) -> str:
        return self._model_profile

    def get_indexing_policy(self) -> str:
        return "linear"

    def get_cache_state_restore_policy(self) -> str:
        return "cold"

    def get_hnf_size_per_hnf_bytes(self) -> int:
        return self._hnf_size_bytes

    def get_total_hnf_capacity_bytes(self) -> int:
        return self._hnf_size_bytes * self._num_hnfs

    def hnf_index_for_address(self, address: int) -> int:
        if address < 0:
            raise ValueError("address must be non-negative.")
        block_bits = int(math.log2(self._cache_line_size))
        return (address >> block_bits) & (self._num_hnfs - 1)

    def get_configuration(self) -> Dict[str, object]:
        """Return explicit MESI topology and experiment-input provenance."""
        return {
            "coherence_protocol": "MESI_Two_Level",
            "model_profile": self._model_profile,
            "model_description": MODEL_PROFILES[self._model_profile][
                "description"
            ],
            "topology": "private L1I/private data + inclusive shared L2 LLC",
            "topology_matches_chi_three_level": False,
            "private_data_mapping": (
                "CacheLens l2_size/l2_assoc -> MESI_Two_Level L1D"
            ),
            "private_data_size": self._private_data_size,
            "private_data_size_bytes": self._private_data_size_bytes,
            "private_data_assoc": self._private_data_assoc,
            "instruction_cache_size": self._cachelens_l1i_size,
            "instruction_cache_size_bytes": self._cachelens_l1i_size_bytes,
            "instruction_cache_assoc": self._cachelens_l1i_assoc,
            "cache_state_restore_policy": (
                self.get_cache_state_restore_policy()
            ),
            "cache_state_continuity": False,
            "indexing_policy": self.get_indexing_policy(),
            "ddio_way_part": self._ddio_way_part,
            # Classified CacheLens TX payload reads are no-allocate in the
            # MESI L2 state machine, independently of write retention.
            "nic_read_no_allocate": True,
            "hnf_inclusion": "inclusive",
            "llc_inclusion_source": "MESI_Two_Level protocol",
            "num_hnfs": self._num_hnfs,
            "num_llc_banks": self._num_hnfs,
            "hnf_size_per_hnf": self._hnf_size,
            "hnf_size_per_hnf_bytes": self._hnf_size_bytes,
            "total_hnf_capacity_bytes": self.get_total_hnf_capacity_bytes(),
            "hnf_assoc": self._hnf_assoc,
            "llc_replacement_policy": (
                "LRU" if self._ddio_way_part > 0 else "TreePLRU"
            ),
            "cache_line_size": self._cache_line_size,
            "bank_select_low_bit": int(math.log2(self._cache_line_size)),
            "bank_select_bits": int(math.log2(self._num_hnfs)),
            "link_latency": self._link_latency,
            "router_latency": self._router_latency,
            "network_buffer_size": self._network_buffer_size,
            "core_clock": self._core_clock or "board",
            "hnf_clock": "board",
        }
