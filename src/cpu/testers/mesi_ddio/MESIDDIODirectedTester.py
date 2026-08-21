# Copyright (c) 2026 minhuw
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

from m5.objects.ClockedObject import ClockedObject
from m5.params import *
from m5.proxy import *


class MESIDDIODirectedTester(ClockedObject):
    type = "MESIDDIODirectedTester"
    cxx_header = "cpu/testers/mesi_ddio/mesi_ddio_tester.hh"
    cxx_class = "gem5::MESIDDIODirectedTester"

    cpu_ports = VectorRequestPort("CPU-side Ruby sequencer ports")
    dma_port = RequestPort("DMA-side Ruby sequencer port")

    scenario = Param.String("Directed MESI DDIO scenario")
    base_addr = Param.Addr(0x10000, "Base address of the test region")
    set_stride = Param.Addr(128, "Stride between lines in one L2 set")
    block_size = Param.Unsigned(64, "Cache-line size")
    routing_banks = Param.Unsigned(1, "Number of MESI L2 routing banks")
    response_timeout = Param.Cycles(100000, "Per-phase progress timeout")
    completion_quiet_period = Param.Cycles(
        32, "Quiet period used to detect duplicate completions"
    )
    system = Param.System(Parent.any, "System this tester belongs to")
