# Copyright (c) 2026 minhuw
# SPDX-License-Identifier: BSD-3-Clause

from m5.objects.ClockedObject import ClockedObject
from m5.params import Param, RequestPort, VectorRequestPort
from m5.proxy import Parent
from m5.SimObject import PyBindMethod


class SplitRubyDirectedTester(ClockedObject):
    type = "SplitRubyDirectedTester"
    cxx_header = "cpu/testers/split_ruby/split_ruby_tester.hh"
    cxx_class = "gem5::SplitRubyDirectedTester"
    cxx_exports = [PyBindMethod("assertComplete")]

    cpu_ports = VectorRequestPort("Exactly two CPU Ruby sequencer ports")
    dma_port = RequestPort("Eligible DMA Ruby sequencer port")
    role = Param.Int(-1, "-1: all requests; 0: CPU0; 1: CPU1 and DMA")
    system = Param.System(Parent.any, "System containing this tester")
    phase_ticks = Param.Tick(1000000, "Absolute interval between phases")
    stop_tick = Param.Tick(10000000, "Bridge's agreed global stop tick")
