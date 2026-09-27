# Copyright (c) 2026 The Regents of the University of California
# All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause

"""Source-level wiring checks; runnable with Python without a gem5 build."""

import importlib.util
import sys
import types
import unittest
from pathlib import Path
from unittest.mock import Mock, patch


ROOT = Path(__file__).resolve().parents[3] / "src/python"
RUBY = "gem5.components.cachehierarchies.ruby"
PREBUILT = "gem5.prebuilt.cachelens"


class SimObjectStub:
    def __init__(self, **kwargs):
        self.in_ports = object()
        self.in_port = object()
        self.out_port = object()
        self.interrupt_out_port = object()
        self.__dict__.update(kwargs)

    def connectIOPorts(self, bus):
        self.io_bus = bus

    def setup_buffers(self):
        self.buffers_ready = True


class AbstractHierarchyStub:
    def __init__(self):
        pass

    def incorporate_cache(self, board):
        pass

    def get_coherence_protocol(self):
        pass

    def is_ruby(self):
        pass


def load_source(name):
    spec = importlib.util.spec_from_file_location(
        name, ROOT / (name.replace(".", "/") + ".py")
    )
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


class CacheLensMESIThreeLevelHierarchyTestSuite(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Isolate source imports, including in the gem5 pyunit runner. Only
        # SimObjects and board/core abstractions are stubs; controllers,
        # hierarchy, network, geometry conversion and metadata are real code.
        modules = patch.dict(sys.modules)
        modules.start()
        cls.addClassCleanup(modules.stop)
        names = [
            "m5.objects", "m5.util.convert", "gem5.coherence_protocol",
            "gem5.isas", "gem5.utils.override", "gem5.utils.requires",
            "gem5.components.boards.abstract_board",
            "gem5.components.processors.abstract_core",
            "gem5.components.cachehierarchies.abstract_cache_hierarchy",
            f"{RUBY}.caches.mesi_three_level.l3_cache",
            f"{RUBY}.topologies.simple_pt2pt", f"{PREBUILT}._model",
        ]
        for name in names:
            parts = name.split(".")
            for i in range(1, len(parts)):
                package = ".".join(parts[:i])
                module = types.ModuleType(package)
                module.__path__ = [str(ROOT / package.replace(".", "/"))]
                sys.modules[package] = module
        objects = types.ModuleType("m5.objects")
        for name in (
            "DMASequencer RubyPortProxy RubySequencer RubySystem SrcClockDomain "
            "LRURP TreePLRURP MessageBuffer RubyCache RubyPrefetcher ClockDomain "
            "RubyDirectoryMemory MESI_Three_Level_L0Cache_Controller "
            "MESI_Three_Level_L1Cache_Controller MESI_Three_Level_L2Cache_Controller "
            "MESI_Three_Level_DMA_Controller MESI_Three_Level_Directory_Controller "
            "SimpleExtLink SimpleIntLink SimpleNetwork Switch"
        ).split():
            setattr(objects, name, type(name, (SimObjectStub,), {}))
        sys.modules["m5.objects"] = objects
        for name, attrs in {
            "gem5.utils.requires": {"requires": lambda **kwargs: None},
            "gem5.components.boards.abstract_board": {"AbstractBoard": object},
            "gem5.components.processors.abstract_core": {"AbstractCore": object},
            "gem5.components.cachehierarchies.abstract_cache_hierarchy": {
                "AbstractCacheHierarchy": AbstractHierarchyStub
            },
        }.items():
            module = types.ModuleType(name)
            module.__dict__.update(attrs)
            sys.modules[name] = module
        for name in (
            "m5.util.convert", "gem5.utils.override", "gem5.coherence_protocol",
            "gem5.isas",
            "gem5.components.cachehierarchies.abstract_three_level_cache_hierarchy",
            f"{RUBY}.abstract_ruby_cache_hierarchy", f"{PREBUILT}._model",
            f"{RUBY}.topologies.simple_pt2pt",
        ):
            load_source(name)
        for name in ("l1_cache", "l2_cache", "l3_cache", "dma_controller", "directory"):
            load_source(f"{RUBY}.caches.mesi_three_level.{name}")
        cls.stock = load_source(
            f"{RUBY}.mesi_three_level_cache_hierarchy"
        ).MESIThreeLevelCacheHierarchy
        cls.hierarchy = load_source(
            f"{PREBUILT}.mesi_three_level_cache_hierarchy"
        ).CacheLensMESIThreeLevelHierarchy
        cls.isa = sys.modules["gem5.isas"].ISA

    def board(self, isa=None, line_size=64, dma=True):
        cores = [Mock(), Mock()]
        for core in cores:
            core.requires_send_evicts.return_value = True
        processor = Mock()
        processor.get_cores.return_value = cores
        processor.get_isa.return_value = isa or self.isa.X86
        board = Mock(processor=processor)
        board.get_processor.return_value = processor
        board.get_cache_line_size.return_value = line_size
        board.get_clock_domain.return_value = SimObjectStub(voltage_domain=object())
        board.has_io_bus.return_value = True
        board.has_dma_ports.return_value = dma
        board.get_ddio_dma_ports.return_value = []
        board.get_dma_ports.return_value = [object(), object()] if dma else []
        board.get_mem_ports.return_value = [(object(), object())]
        return board

    def test_dma_eligibility_is_explicit_and_defaults_off(self):
        for selected in (False, True):
            with self.subTest(selected=selected):
                board = self.board()
                ports = board.get_dma_ports()
                if selected:
                    board.get_ddio_dma_ports.return_value = [ports[1]]
                else:
                    del board.get_ddio_dma_ports
                hierarchy = self.hierarchy()
                hierarchy.incorporate_cache(board)
                self.assertEqual(
                    [False, selected],
                    [dma.ddio_eligible for dma in hierarchy.dma_controllers],
                )

    def test_native_wiring_and_callbacks(self):
        h = self.hierarchy(
            l1i_size="16KiB", l1d_size="64KiB", l1d_assoc=4,
            l2_size="512KiB", l2_assoc=8, hnf_size="2MiB", hnf_assoc=16,
            num_hnfs=4, ddio_way_part=2, indexing_policy="splitmix64",
            core_clock="3GHz", link_latency=3, router_latency=2,
            network_buffer_size=8,
        )
        board = self.board()
        h.incorporate_cache(board)
        ruby = h.ruby_system
        self.assertFalse(ruby.cache_trace_warmup)
        self.assertEqual(4, ruby.num_of_sequencers)
        self.assertEqual(3, ruby.number_of_virtual_networks)
        for level, count in ((1, 2), (2, 2), (3, 4)):
            controllers = getattr(h, f"l{level}_controllers")
            self.assertIs(controllers, getattr(ruby, f"l{level}_controllers"))
            self.assertEqual(count, len(controllers))
            self.assertEqual(list(range(count)), [c.version for c in controllers])
        for core, l1, l2 in zip(
            board.processor.get_cores(), h.l1_controllers, h.l2_controllers
        ):
            self.assertEqual("16KiB", l1.Icache.size)
            self.assertEqual("64KiB", l1.Dcache.size)
            self.assertEqual("512KiB", l2.cache.size)
            self.assertEqual(6, l1.Dcache.start_index_bit)
            self.assertEqual(6, l2.cache.start_index_bit)
            self.assertEqual(2, l2.l2_select_num_bits)
            self.assertIs(l2.bufferFromL0, l1.bufferToL1)
            self.assertIs(l2.bufferToL0, l1.bufferFromL1)
            self.assertIs(l1.clk_domain, h.core_clk_domain)
            self.assertIs(l2.clk_domain, h.core_clk_domain)
            self.assertIs(core.get_simobject().clk_domain, h.core_clk_domain)
            core.connect_icache.assert_called_once_with(l1.sequencer.in_ports)
            core.connect_dcache.assert_called_once_with(l1.sequencer.in_ports)
            core.connect_walker_ports.assert_called_once_with(
                l1.sequencer.in_ports, l1.sequencer.in_ports
            )
            core.connect_interrupt.assert_called_once_with(
                l1.sequencer.interrupt_out_port, l1.sequencer.in_ports
            )
            self.assertIs(l1.sequencer.io_bus, board.get_io_bus())
        for l3 in h.l3_controllers:
            cache = l3.L2cache
            self.assertEqual("2MiB", cache.size)
            self.assertEqual(16, cache.assoc)
            self.assertEqual(8, cache.start_index_bit)
            self.assertEqual(2, cache.ddio_way_part)
            self.assertTrue(cache.addr_hash)
            self.assertEqual("LRURP", type(cache.replacement_policy).__name__)
        for dma, port in zip(h.dma_controllers, board.get_dma_ports()):
            self.assertEqual(2, dma.l2_select_num_bits)
            self.assertTrue(dma.dma_sequencer.supports_masked_writes)
            self.assertIs(port, dma.dma_sequencer.in_ports)
        network = ruby.network
        self.assertTrue(network.buffers_ready)
        self.assertEqual(8, network.buffer_size)
        self.assertEqual(11, len(network.ext_links))
        self.assertTrue(all(
            link.latency == 3
            for link in network.ext_links + network.int_links
        ))
        self.assertTrue(all(
            router.int_routing_latency == 2 for router in network.routers
        ))
        board.connect_system_port.assert_called_once_with(ruby.sys_port_proxy.in_ports)

    def test_defaults_and_metadata(self):
        h = self.hierarchy()
        h.incorporate_cache(self.board(dma=False))
        self.assertEqual([], h.dma_controllers)
        self.assertEqual(
            "TreePLRURP",
            type(h.l3_controllers[0].L2cache.replacement_policy).__name__,
        )
        self.assertFalse(h.l3_controllers[0].L2cache.addr_hash)
        config = h.get_configuration()
        self.assertEqual("MESI_Three_Level", h.get_coherence_protocol().value)
        self.assertEqual("inclusive", config["hnf_inclusion"])
        self.assertTrue(config["topology_matches_chi_three_level"])
        self.assertIn("L1Cache", config["private_l2_mapping"])
        self.assertIn("L2Cache (L3)", config["llc_mapping"])
        self.assertEqual(1 << 20, config["private_l2_size_bytes"])
        self.assertEqual(32 << 10, config["private_data_size_bytes"])
        self.assertEqual("cold", config["cache_state_restore_policy"])
        self.assertTrue(config["nic_read_no_allocate"])
        self.assertEqual(-1, config["ddio_way_part"])
        self.assertEqual(2, self.hierarchy(model_profile="intel_ddio")._ddio_way_part)
        h = self.hierarchy(num_hnfs=4, indexing_policy="splitmix64")
        self.assertEqual(4 << 20, h.get_total_hnf_capacity_bytes())
        self.assertEqual(
            [i % 4 for i in range(16)],
            [h.hnf_index_for_address(i * 64) for i in range(16)],
        )
        with self.assertRaises(ValueError):
            h.hnf_index_for_address(-1)

    def test_validation_and_isa(self):
        for options in (
            {"num_hnfs": 0}, {"num_hnfs": 3}, {"ddio_way_part": 0},
            {"ddio_way_part": 9}, {"ddio_way_part": -2},
            {"model_profile": "arm-generic"}, {"model_profile": "unknown"},
            {"model_profile": "x86-generic", "ddio_way_part": 2},
            {"indexing_policy": "xor"}, {"network_buffer_size": 0},
            {"router_latency": 0}, {"link_latency": -1},
            {"l2_assoc": 1}, {"hnf_assoc": 1, "ddio_way_part": -1},
        ):
            with self.subTest(options=options), self.assertRaises(ValueError):
                self.hierarchy(**options)
        for prefix in ("l1i", "l1d", "l2", "hnf"):
            for size in ("0B", "192KiB", "65B", "64B", "invalid"):
                with self.subTest(prefix=prefix, size=size):
                    with self.assertRaises(ValueError):
                        self.hierarchy(**{f"{prefix}_size": size})
            with self.assertRaises(ValueError):
                self.hierarchy(**{f"{prefix}_assoc": 0})
        # Direct-mapped L1 uses LRU, as does a positive-DDIO LLC.
        self.hierarchy(l1i_assoc=1, l1d_assoc=1, hnf_assoc=1, ddio_way_part=1)
        with self.assertRaisesRegex(ValueError, "only on x86"):
            self.hierarchy().incorporate_cache(self.board(isa=self.isa.ARM))
        with self.assertRaisesRegex(ValueError, "64-byte"):
            self.hierarchy().incorporate_cache(self.board(line_size=128))

    def test_stock_defaults_and_validation(self):
        common = dict(
            l1i_size="32KiB", l1i_assoc=2, l1d_size="32KiB", l1d_assoc=2,
            l2_size="1MiB", l2_assoc=8, l3_size="2MiB", l3_assoc=8,
            num_l3_banks=2,
        )
        h = self.stock(**common)
        board = self.board()
        h.incorporate_cache(board)
        self.assertFalse(hasattr(h.ruby_system, "cache_trace_warmup"))
        self.assertIs(board.get_clock_domain(), h._l2_controllers[0].clk_domain)
        self.assertEqual(0, h.ruby_system.network.buffer_size)
        self.assertEqual(1, h._dma_controllers[0].l2_select_num_bits)
        self.assertEqual(-1, h._l3_controllers[0].L2cache.ddio_way_part)
        self.assertFalse(h._l3_controllers[0].L2cache.addr_hash)
        for options in ({"num_l3_banks": 0}, {"num_l3_banks": 3}, {"ddio_way_part": 9}):
            with self.assertRaises(ValueError):
                self.stock(**(common | options))
        with self.assertRaises(TypeError):
            self.stock(**common, l3_addr_hash=1)


if __name__ == "__main__":
    unittest.main()
