# SPDX-License-Identifier: BSD-3-Clause
from m5.SimObject import SimObject
from m5.objects.ClockedObject import ClockedObject
from m5.params import Param, RequestPort
from m5.proxy import Parent


class DmaObserveTester(ClockedObject):
    type = 'DmaObserveTester'
    cxx_class = 'gem5::DmaObserveTester'
    cxx_header = 'test_objects/dma_observe_tester.hh'
    system = Param.System(Parent.any, 'System owning the directed DMA port')
    dma = RequestPort('Untagged directed DMA, preserving old dmaAction callers')
