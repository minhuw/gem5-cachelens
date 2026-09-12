# Copyright (c) 2026 The Regents of the University of California
# All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause

"""Modern ARM and x86 full-system boards for CacheLens experiments."""

from typing import (
    List,
    Optional,
    Sequence,
)

from m5.defines import buildEnv

from gem5.components.boards.abstract_board import AbstractBoard
from gem5.components.boards.kernel_disk_workload import KernelDiskWorkload
from gem5.components.cachehierarchies.abstract_cache_hierarchy import (
    AbstractCacheHierarchy,
)
from gem5.components.memory.abstract_memory_system import AbstractMemorySystem
from gem5.components.processors.abstract_processor import AbstractProcessor
from gem5.resources.resource import AbstractResource
from gem5.utils.override import overrides

from .network import build_cachelens_network


def _own_vector(owner, name: str, objects: list) -> None:
    if objects:
        setattr(owner, name, objects)
    else:
        owner.__dict__[name] = []


if buildEnv["USE_ARM_ISA"]:
    from m5.objects import (
        ArmDefaultRelease,
        ArmRelease,
        CowDiskImage,
        IdeController,
        IdeDisk,
        RawDiskImage,
        VExpress_GEM5_Base,
        VExpress_GEM5_V1,
    )

    from gem5.components.boards.arm_board import ArmBoard

    class CacheLensArmBoard(ArmBoard):
        """VExpress GEM5 V1 ARM board with CacheLens networking."""

        def __init__(
            self,
            *,
            clk_freq: str,
            processor: AbstractProcessor,
            memory: AbstractMemorySystem,
            cache_hierarchy: AbstractCacheHierarchy,
            network_options: Optional[dict] = None,
            platform: VExpress_GEM5_Base = VExpress_GEM5_V1(),
            release: ArmRelease = ArmDefaultRelease(),
        ) -> None:
            self._cachelens_network_options = network_options or {}
            if self._cachelens_network_options.get("num_nics", 0) > 3:
                raise ValueError(
                    "CacheLens ARM supports at most three NICs; a fourth "
                    "NIC would alias the IDE controller's INTx route."
                )
            super().__init__(
                clk_freq=clk_freq,
                processor=processor,
                memory=memory,
                cache_hierarchy=cache_hierarchy,
                platform=platform,
                release=release,
            )

        @overrides(KernelDiskWorkload)
        def _add_disk_to_board(self, disk_image: AbstractResource) -> None:
            # Match the CAL guest's proven IDE storage path. The controller
            # must be first so it remains PCI device 1 and the NIC is 2.
            ide_disk = IdeDisk()
            ide_disk.driveID = "device0"
            ide_disk.image = CowDiskImage(
                child=RawDiskImage(read_only=True),
                read_only=False,
            )
            ide_disk.image.child.image_file = disk_image.get_local_path()
            controller = IdeController()
            controller.disks = [ide_disk]
            self._add_pci_device(controller)
            self._attach_cachelens_network()

        def _attach_cachelens_network(self) -> None:
            network = build_cachelens_network(
                **self._cachelens_network_options
            )

            # NICs are ultimately owned by ArmBoard.pci_devices. Keep the
            # public Python view without creating a second SimObject parent.
            self.__dict__["nics"] = network.nics
            _own_vector(self, "loadgens", network.loadgens)
            _own_vector(self, "links", network.links)
            for nic in network.nics:
                self._add_pci_device(nic)

            expected_bdfs = [
                f"0000:00:{2 + index:02x}.0"
                for index in range(len(network.nics))
            ]
            if self.get_nic_bdfs() != expected_bdfs:
                raise RuntimeError(
                    "CacheLens ARM NICs must occupy consecutive BDFs "
                    "starting at 0000:00:02.0."
                )

        def get_nic_bdfs(self) -> List[str]:
            return [
                f"0000:{int(nic.pci_bus):02x}:{int(nic.pci_dev):02x}."
                f"{int(nic.pci_func)}"
                for nic in self.nics
            ]

        @overrides(KernelDiskWorkload)
        def get_disk_device(self):
            return "/dev/sda"


if buildEnv["USE_X86_ISA"]:
    from m5.objects import (
        Port,
        Pvrdma,
    )

    from gem5.components.boards.x86_board import X86Board

    class CacheLensX86Board(X86Board):
        """x86 PC board with direct CacheLens NIC DMA requestors."""

        def __init__(
            self,
            *,
            clk_freq: str,
            processor: AbstractProcessor,
            memory: AbstractMemorySystem,
            cache_hierarchy: AbstractCacheHierarchy,
            network_options: Optional[dict] = None,
        ) -> None:
            self._cachelens_network_options = network_options or {}
            num_nics = self._cachelens_network_options.get("num_nics", 0)
            self._enable_pvrdma = self._cachelens_network_options.get(
                "enable_pvrdma", False
            )
            if num_nics > 1:
                raise ValueError(
                    "The first CacheLens x86 board version supports one NIC."
                )
            if self._enable_pvrdma and num_nics != 1:
                raise ValueError("PVRDMA requires exactly one CacheLens NIC.")
            super().__init__(
                clk_freq=clk_freq,
                processor=processor,
                memory=memory,
                cache_hierarchy=cache_hierarchy,
            )

        @overrides(X86Board)
        def _setup_memory_ranges(self) -> None:
            super()._setup_memory_ranges()
            # X86Board includes a PCI MMIO aperture in mem_ranges for legacy
            # Ruby visibility. Keep that routing aperture, but classify only
            # guest RAM as physical memory so CacheLens PIO bypasses CHI.
            self.main_mem_ranges = list(self._ram_ranges)

        @overrides(KernelDiskWorkload)
        def _add_disk_to_board(self, disk_image: AbstractResource) -> None:
            super()._add_disk_to_board(disk_image)
            self._attach_cachelens_network()

        def _get_additional_pci_intx_routes(self):
            routes = [
                (2 + index, 0, 17 + index)
                for index in range(
                    self._cachelens_network_options.get("num_nics", 0)
                )
            ]
            if self._enable_pvrdma:
                routes.append((2, 1, 18))
            return routes

        def _attach_cachelens_network(self) -> None:
            network_options = dict(self._cachelens_network_options)
            network_options.pop("enable_pvrdma", None)
            network = build_cachelens_network(**network_options)
            if self._enable_pvrdma:
                nic = network.nics[0]
                nic.HeaderType = int(nic.HeaderType) | 0x80
            for index, nic in enumerate(network.nics):
                pci_device = 2 + index
                io_apic_intin = 17 + index
                nic.host = self.pc.pci_host
                nic.pci_bus = 0
                nic.pci_dev = pci_device
                nic.pci_func = 0
                nic.InterruptPin = 1
                nic.InterruptLine = io_apic_intin
                nic.pio = self.iobus.mem_side_ports
                if not self.cache_hierarchy.is_ruby():
                    nic.dma = self.iobus.cpu_side_ports

            rdmas = []
            if self._enable_pvrdma:
                rdma = Pvrdma(
                    hardware_address=str(network.nics[0].hardware_address),
                    companion_mac_word_swap=True,
                )
                rdma.host = self.pc.pci_host
                rdma.pci_bus = 0
                rdma.pci_dev = 2
                rdma.pci_func = 1
                rdma.InterruptPin = 2
                rdma.InterruptLine = 18
                rdma.pio = self.iobus.mem_side_ports
                if not self.cache_hierarchy.is_ruby():
                    rdma.dma = self.iobus.cpu_side_ports
                rdmas.append(rdma)

            _own_vector(self, "nics", network.nics)
            _own_vector(self, "rdmas", rdmas)
            _own_vector(self, "loadgens", network.loadgens)
            _own_vector(self, "links", network.links)

        @overrides(AbstractBoard)
        def get_dma_ports(self) -> Sequence[Port]:
            # Keep IDE and each NIC on distinct CHI requestors. CPU PIO reaches
            # the I/O bus through RubySequencer.connectIOPorts().
            return (
                [self.pc.south_bridge.ide.dma]
                + [nic.dma for nic in getattr(self, "nics", [])]
                + [rdma.dma for rdma in getattr(self, "rdmas", [])]
            )

        def get_nic_bdfs(self) -> List[str]:
            return [
                f"0000:{int(nic.pci_bus):02x}:{int(nic.pci_dev):02x}."
                f"{int(nic.pci_func)}"
                for nic in self.nics
            ]

        def get_rdma_bdfs(self) -> List[str]:
            return [
                f"0000:{int(rdma.pci_bus):02x}:{int(rdma.pci_dev):02x}."
                f"{int(rdma.pci_func)}"
                for rdma in getattr(self, "rdmas", [])
            ]

        @overrides(KernelDiskWorkload)
        def get_disk_device(self):
            return "/dev/sda"

        @overrides(KernelDiskWorkload)
        def get_default_kernel_args(self) -> List[str]:
            return [
                "earlyprintk=ttyS0",
                "console=ttyS0",
                "lpj=7999923",
                # CacheLens keeps the x86 MP-table PCI routes explicit; this
                # branch has no ACPI AML _PRT generator.
                "pci=noacpi",
                "root={root_value}",
                "rw",
            ]
