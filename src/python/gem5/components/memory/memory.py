# Copyright (c) 2021 The Regents of the University of California
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

""" Channeled "generic" DDR memory controllers
"""

from math import log
from typing import (
    List,
    Optional,
    Sequence,
    Tuple,
    Type,
    Union,
)

from m5.objects import (
    AddrRange,
    DRAMInterface,
    MemCtrl,
    NoncoherentXBar,
    Port,
    RangeAddrMapper,
)
from m5.util.convert import toMemorySize

from ...utils.override import overrides
from ..boards.abstract_board import AbstractBoard
from .abstract_memory_system import AbstractMemorySystem


def _try_convert(val, cls):
    try:
        return cls(val)
    except:
        raise Exception(f"Could not convert {val} to {cls}")


def _isPow2(num):
    log_num = int(log(num, 2))
    if 2**log_num != num:
        return False
    else:
        return True


class ChanneledMemory(AbstractMemorySystem):
    """A class to implement multi-channel memory system

    This class can take a DRAM Interface as a parameter to model a multi
    channel DDR DRAM memory system.
    """

    def __init__(
        self,
        dram_interface_class: Type[DRAMInterface],
        num_channels: Union[int, str],
        interleaving_size: Union[int, str],
        size: Optional[str] = None,
        addr_mapping: Optional[str] = None,
    ) -> None:
        """
        :param dram_interface_class: The DRAM interface type to create with
                                     this memory controller.
        :param num_channels: The number of channels that needs to be
                             simulated.
        :param size: Optionally specify the size of the DRAM controller's
                     address space. By default, it starts at 0 and ends at
                     the size of the DRAM device specified.
        :param addr_mapping: Defines the address mapping scheme to be used.
                             If ``None``, it is defaulted to ``addr_mapping`` from
                             ``dram_interface_class``.
        :param interleaving_size: Defines the interleaving size of the multi-
                                  channel memory system. By default, it is
                                  equivalent to the atom size, i.e., 64.
        """
        num_channels = _try_convert(num_channels, int)
        interleaving_size = _try_convert(interleaving_size, int)

        if size:
            size = _try_convert(size, str)

        if addr_mapping:
            addr_mapping = _try_convert(addr_mapping, str)

        super().__init__()
        self._dram_class = dram_interface_class
        self._num_channels = num_channels

        if not _isPow2(interleaving_size):
            raise ValueError("Memory interleaving size should be a power of 2")
        self._intlv_size = interleaving_size

        if addr_mapping:
            self._addr_mapping = addr_mapping
        else:
            self._addr_mapping = self._dram_class.addr_mapping.value

        if size:
            self._size = toMemorySize(size)
        else:
            self._size = self._get_dram_size(num_channels, self._dram_class)

        self._create_mem_interfaces_controller()
        # These are populated only when a disjoint guest address map is
        # requested. Each guest range has one mapper; a common xbar routes the
        # packed addresses across the physical memory channels.
        self._range_mappers = []
        self._range_mapper_buses = []
        # Public vectors are created on demand below so these dynamically
        # created SimObjects remain reachable from the memory system tree.

    def _create_controller_group(self):
        dram = [
            self._dram_class(addr_mapping=self._addr_mapping)
            for _ in range(self._num_channels)
        ]
        controllers = [MemCtrl(dram=interface) for interface in dram]
        return dram, controllers

    def _create_mem_interfaces_controller(self):
        self._dram, self.mem_ctrl = self._create_controller_group()

    def _get_dram_size(self, num_channels: int, dram: DRAMInterface) -> int:
        return num_channels * (
            dram.device_size.value
            * dram.devices_per_rank.value
            * dram.ranks_per_channel.value
        )

    def _interleaved_range(self, memory_range, intlv_match):
        if self._addr_mapping == "RoRaBaChCo":
            rowbuffer_size = (
                self._dram_class.device_rowbuffer_size.value
                * self._dram_class.devices_per_rank.value
            )
            intlv_low_bit = log(rowbuffer_size, 2)
        elif self._addr_mapping in ["RoRaBaCoCh", "RoCoRaBaCh"]:
            intlv_low_bit = log(self._intlv_size, 2)
        else:
            raise ValueError(
                "Only these address mappings are supported: "
                "RoRaBaChCo, RoRaBaCoCh, RoCoRaBaCh"
            )

        intlv_bits = log(self._num_channels, 2)
        return AddrRange(
            start=memory_range.start,
            size=memory_range.size(),
            intlvHighBit=intlv_low_bit + intlv_bits - 1,
            xorHighBit=0,
            intlvBits=intlv_bits,
            intlvMatch=intlv_match,
        )

    def _interleave_range(self, memory_range, controllers):
        for i, ctrl in enumerate(controllers):
            ctrl.dram.range = self._interleaved_range(memory_range, i)

    def _interleave_addresses(self):
        self._interleave_range(self._mem_range, self.mem_ctrl)

    @overrides(AbstractMemorySystem)
    def incorporate_memory(self, board: AbstractBoard) -> None:
        if self._intlv_size < int(board.get_cache_line_size()):
            raise ValueError(
                "Memory interleaving size can not be smaller than"
                " board's cache line size.\nBoard's cache line size: "
                f"{board.get_cache_line_size()}\n, This memory's interleaving "
                f"size: {self._intlv_size}"
            )

    def _get_all_controllers(self) -> List[MemCtrl]:
        return list(self.mem_ctrl) + list(getattr(self, "extra_mem_ctrl", []))

    @overrides(AbstractMemorySystem)
    def get_mem_ports(self) -> Sequence[Tuple[AddrRange, Port]]:
        if self._range_mappers:
            # Each mapper advertises exactly one disjoint guest range. The
            # shared downstream xbar performs channel selection after the
            # guest address has been packed around holes in the address map.
            return [
                (guest_range, mapper.cpu_side_port)
                for guest_range, mapper in zip(
                    self._mem_ranges, self._range_mappers
                )
            ]

        return [
            (ctrl.dram.range, ctrl.port)
            for ctrl in self._get_all_controllers()
        ]

    @overrides(AbstractMemorySystem)
    def get_memory_controllers(self) -> List[MemCtrl]:
        return self._get_all_controllers()

    @overrides(AbstractMemorySystem)
    def get_mem_interfaces(self) -> List[DRAMInterface]:
        return [ctrl.dram for ctrl in self._get_all_controllers()]

    @overrides(AbstractMemorySystem)
    def get_size(self) -> int:
        return self._size

    @overrides(AbstractMemorySystem)
    def set_memory_range(self, ranges: List[AddrRange]) -> None:
        """Set the guest-visible ranges served by this memory system.

        Disjoint ranges are packed into one physical address space.  The
        channel controllers therefore remain one group, while one mapper per
        guest range presents the original address map to the cache hierarchy.
        This is important for x86's PCI hole: making a
        second controller group would incorrectly double the timing queues
        and bandwidth.
        """
        if not ranges:
            raise ValueError("At least one memory range must be provided.")

        total_size = 0
        previous_start = None
        previous_end = None
        for memory_range in ranges:
            start = int(memory_range.start)
            size = int(memory_range.size())
            end = start + size

            if size <= 0:
                raise ValueError("Memory ranges must not be empty.")
            if previous_start is not None and start < previous_start:
                raise ValueError("Memory ranges must be sorted by address.")
            if previous_end is not None and start < previous_end:
                raise ValueError("Memory ranges must not overlap.")

            total_size += size
            previous_start = start
            previous_end = end

        if total_size != self._size:
            raise ValueError(
                "The summed memory range size must match the memory system "
                f"size ({total_size} != {self._size})."
            )

        # Always tear down the Python-side routing state first.  In
        # particular, a caller may configure a split range and then return
        # to a conventional single range before the board is instantiated.
        self._range_mappers = []
        self._range_mapper_buses = []
        for child_name in ("range_mappers", "range_mapper_buses"):
            if child_name in self._children:
                self.clear_child(child_name)
        if len(ranges) == 1:
            self._mem_ranges = list(ranges)
            self._mem_range = ranges[0]
            self._interleave_addresses()
            return

        unsupported_overrides = (
            type(self)._create_mem_interfaces_controller
            is not ChanneledMemory._create_mem_interfaces_controller
            or type(self)._interleave_addresses
            is not ChanneledMemory._interleave_addresses
            or type(self).get_mem_ports is not ChanneledMemory.get_mem_ports
        )
        if unsupported_overrides:
            raise ValueError(
                f"{type(self).__name__} does not support multiple memory "
                "ranges."
            )

        self._mem_ranges = list(ranges)

        # Interleave the packed physical space across the original channel
        # controllers.  RangeAddrMapper only changes the address offset; the
        # controller's interleaved range consequently still selects channels
        # exactly as it does for a contiguous memory system.
        packed_range = AddrRange(self._size)
        self._mem_range = packed_range
        self._interleave_range(packed_range, self.mem_ctrl)

        packed_start = 0
        packed_ranges = []
        for guest_range in self._mem_ranges:
            packed_ranges.append(
                AddrRange(start=packed_start, size=guest_range.size())
            )
            packed_start += int(guest_range.size())

        # One common xbar merges the disjoint mapped ranges and routes each
        # packed address to its interleaved physical channel. Keeping channel
        # selection downstream of the mappers gives every guest range one
        # non-overlapping responder, which works for both Ruby/CHI and classic
        # NoCache configurations.
        range_xbar = NoncoherentXBar(
            frontend_latency=0,
            forward_latency=0,
            response_latency=0,
            header_latency=0,
            width=self._intlv_size,
            timing_transparent=True,
        )
        for ctrl in self.mem_ctrl:
            range_xbar.mem_side_ports = ctrl.port
        self._range_mapper_buses.append(range_xbar)

        for guest_range, packed in zip(self._mem_ranges, packed_ranges):
            mapper = RangeAddrMapper(
                original_ranges=[guest_range],
                remapped_ranges=[packed],
            )
            mapper.mem_side_port = range_xbar.cpu_side_ports
            self._range_mappers.append(mapper)

        self.range_mappers = self._range_mappers
        self.range_mapper_buses = self._range_mapper_buses

    @overrides(AbstractMemorySystem)
    def get_uninterleaved_range(self) -> List[AddrRange]:
        return list(self._mem_ranges)
