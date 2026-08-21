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

import re
import unittest
from pathlib import Path


_REPO_ROOT = Path(__file__).resolve().parents[3]
_PROTOCOL = _REPO_ROOT / "src/mem/ruby/protocol"
_DMA = _PROTOCOL / "MESI_Two_Level-dma.sm"
_L1 = _PROTOCOL / "MESI_Two_Level-L1cache.sm"
_L2 = _PROTOCOL / "MESI_Two_Level-L2cache.sm"
_DIRECTORY = _PROTOCOL / "MESI_Two_Level-dir.sm"
_LEGACY = _REPO_ROOT / "configs/ruby/MESI_Two_Level.py"
_RUBY_HIERARCHIES = (
    _REPO_ROOT / "src/python/gem5/components/cachehierarchies/ruby"
)
_STDLIB = _RUBY_HIERARCHIES / "mesi_two_level_cache_hierarchy.py"
_STDLIB_CACHES = _RUBY_HIERARCHIES / "caches/mesi_two_level"
_STDLIB_L1 = _STDLIB_CACHES / "l1_cache.py"
_STDLIB_L2 = _STDLIB_CACHES / "l2_cache.py"
_STDLIB_DMA = _STDLIB_CACHES / "dma_controller.py"
_CACHE_MEMORY_HH = _REPO_ROOT / "src/mem/ruby/structures/CacheMemory.hh"
_CACHE_MEMORY_CC = _REPO_ROOT / "src/mem/ruby/structures/CacheMemory.cc"
_SLICC_TYPES = _PROTOCOL / "RubySlicc_Types.sm"
_MEMTEST = _REPO_ROOT / "src/cpu/testers/memtest/memtest.cc"


def _compact(source: str) -> str:
    return re.sub(r"\s+", "", source)


def _braced_body(source: str, declaration: str) -> str:
    start = source.find(declaration)
    if start == -1:
        raise AssertionError(f"missing declaration: {declaration}")
    body_start = source.find("{", start + len(declaration))
    if body_start == -1:
        raise AssertionError(f"missing body: {declaration}")

    depth = 0
    for index in range(body_start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[body_start + 1 : index]
    raise AssertionError(f"unterminated body: {declaration}")


def _action_body(source: str, name: str) -> str:
    match = re.search(rf"\baction\({re.escape(name)}\b", source)
    if match is None:
        raise AssertionError(f"missing action: {name}")
    return _braced_body(source, match.group(0))


class MESIDMARoutingTest(unittest.TestCase):
    def test_cpu_and_nic_use_the_same_bank_mapping_primitive(self) -> None:
        dma = _compact(_DMA.read_text(encoding="utf-8"))
        l1 = _compact(_L1.read_text(encoding="utf-8"))

        dma_mapping = (
            "mapAddressToRange(addr,MachineType:L2Cache,l2_select_low_bit,"
            "l2_select_num_bits,intToID(0))"
        )
        l1_mapping = (
            "mapAddressToRange(address,MachineType:L2Cache,l2_select_low_bit,"
            "l2_select_num_bits,intToID(0))"
        )
        self.assertEqual(dma.count(dma_mapping), 2)
        self.assertGreaterEqual(l1.count(l1_mapping), 1)

    def test_classification_uses_only_preserved_request_provenance(
        self,
    ) -> None:
        source = _DMA.read_text(encoding="utf-8")
        body = _compact(
            _braced_body(source, "MachineID mapDMARequestToMachine")
        )

        self.assertIn("if(is_seq_req_valid)", body)
        self.assertIn("isNicDmaRead(seq_req)", body)
        self.assertIn("isNicDmaWrite(seq_req)", body)
        self.assertIn(
            "returnmapAddressToMachine(addr,MachineType:Directory);", body
        )
        self.assertNotIn("machineIDToMachineType", body)

    def test_legacy_and_stdlib_share_each_topologys_bank_bits(self) -> None:
        legacy = _compact(_LEGACY.read_text(encoding="utf-8"))
        self.assertEqual(legacy.count("l2_select_num_bits=l2_bits"), 3)

        stdlib = _compact(_STDLIB.read_text(encoding="utf-8"))
        self.assertEqual(
            stdlib.count(
                "l2_select_num_bits=int(math.log(self._num_l2_banks,2))"
            ),
            1,
        )
        # The one computed value is passed to L1, L2, and DMA construction.
        self.assertGreaterEqual(stdlib.count("l2_select_num_bits"), 4)

        stdlib_l1 = _compact(_STDLIB_L1.read_text(encoding="utf-8"))
        stdlib_l2 = _compact(_STDLIB_L2.read_text(encoding="utf-8"))
        stdlib_dma = _compact(_STDLIB_DMA.read_text(encoding="utf-8"))
        self.assertIn(
            "self.l2_select_num_bits=l2_select_num_bits", stdlib_l1
        )
        self.assertIn("+l2_select_num_bits", stdlib_l2)
        self.assertIn(
            "self.l2_select_num_bits=l2_select_num_bits", stdlib_dma
        )
        self.assertNotIn("math.log", stdlib_l1)
        self.assertNotIn("math.log", stdlib_dma)

    def test_l2_proxy_preserves_the_complete_request_message(self) -> None:
        source = _L2.read_text(encoding="utf-8")
        body = _compact(_action_body(source, "ad_proxyDMARequestToDirectory"))

        self.assertIn("out_msg:=in_msg;", body)
        self.assertIn("out_msg.Destination.clear();", body)
        self.assertIn(
            "out_msg.Destination.add(mapAddressToMachine(address,"
            "MachineType:Directory));",
            body,
        )
        self.assertNotIn("L1RequestL2Network_out", body)

        for field in (
            "Requestor",
            "DataBlk",
            "Len",
            "accessAddr",
            "accessSize",
            "accessMask",
            "seqReq",
            "isSeqReqValid",
        ):
            self.assertNotIn(f"out_msg.{field}:=", body)

    def test_l2_handles_every_state_without_touching_cache_state(self) -> None:
        source = _L2.read_text(encoding="utf-8")
        state_body = _braced_body(source, "state_declaration(State")
        states = set(
            re.findall(r"^\s*([A-Z][A-Z0-9_]*)\s*,", state_body, re.M)
        )

        stable = {"NP", "SS", "M", "MT"}
        transient = {
            "M_I",
            "MT_I",
            "MCT_I",
            "I_I",
            "S_I",
            "ISS",
            "IS",
            "IM",
            "SS_MB",
            "MT_MB",
            "MT_IIB",
            "MT_IB",
            "MT_SB",
        }
        self.assertEqual(states, stable | transient)
        self.assertFalse(stable & transient)

        compact = _compact(source)
        stable_transition = (
            "transition({NP,SS,M,MT},{DMA_READ,DMA_WRITE})"
            "{ad_proxyDMARequestToDirectory;jj_popL1RequestQueue;}"
        )
        transient_transition = (
            "transition({M_I,MT_I,MCT_I,I_I,S_I,ISS,IS,IM,SS_MB,MT_MB,"
            "MT_IIB,MT_IB,MT_SB},{DMA_READ,DMA_WRITE})"
            "{zdr_recycleDMARequestQueue;}"
        )
        self.assertIn(stable_transition, compact)
        self.assertIn(transient_transition, compact)

        proxy = _compact(
            _action_body(source, "ad_proxyDMARequestToDirectory")
        )
        recycle = _compact(
            _action_body(source, "zdr_recycleDMARequestQueue")
        )
        proxy_hook = "L2cache.profileDmaRoutingProxy();"
        recycle_hook = "L2cache.profileDmaRoutingTransientRecycle();"
        self.assertIn(proxy_hook, proxy)
        self.assertIn(recycle_hook, recycle)
        proxy = proxy.replace(proxy_hook, "")
        recycle = recycle.replace(recycle_hook, "")
        for forbidden in (
            "L2cache.",
            "cache_entry.",
            "tbe.",
            "L1RequestL2Network_out",
        ):
            self.assertNotIn(forbidden, proxy)
            self.assertNotIn(forbidden, recycle)

    def test_routing_telemetry_is_separate_from_ddio_hit_statistics(
        self,
    ) -> None:
        header_source = _CACHE_MEMORY_HH.read_text(encoding="utf-8")
        header = _compact(header_source)
        implementation = _compact(
            _CACHE_MEMORY_CC.read_text(encoding="utf-8")
        )
        slicc_types = _compact(_SLICC_TYPES.read_text(encoding="utf-8"))

        for stat in (
            "dmaRoutingProxyRequests",
            "dmaRoutingTransientRecycles",
        ):
            self.assertIn(f"statistics::Scalar{stat};", header)
            self.assertIn(f"ADD_STAT({stat},", implementation)
            self.assertNotIn(f"{stat}.flags(", implementation)
        self.assertIn("voidprofileDmaRoutingProxy();", slicc_types)
        self.assertIn("voidprofileDmaRoutingTransientRecycle();", slicc_types)

        proxy = _compact(
            _braced_body(header_source, "void profileDmaRoutingProxy()")
        )
        recycle = _compact(
            _braced_body(
                header_source, "void profileDmaRoutingTransientRecycle()"
            )
        )
        self.assertEqual(
            proxy, "cacheMemoryStats.dmaRoutingProxyRequests++;"
        )
        self.assertEqual(
            recycle, "cacheMemoryStats.dmaRoutingTransientRecycles++;"
        )

    def test_nic_dma_memtest_rejects_atomic_system_mode(self) -> None:
        memtest = _compact(_MEMTEST.read_text(encoding="utf-8"))
        self.assertIn(
            "fatal_if(nicDma&&(atomic||percentFunctional!=0||"
            "percentUncacheable!=0||percentAtomic!=0)",
            memtest,
        )

    def test_directory_responses_target_original_dma_and_line_address(
        self,
    ) -> None:
        source = _DIRECTORY.read_text(encoding="utf-8")
        memory_ingress = _compact(
            _braced_body(source, "in_port(memQueue_in")
        )
        allocate = _compact(_action_body(source, "v_allocateTBE"))
        read_response = _compact(_action_body(source, "dr_sendDMAData"))
        owner_read_response = _compact(
            _action_body(source, "drp_sendDMAData")
        )
        write_response = _compact(_action_body(source, "da_sendDMAAck"))

        self.assertIn(
            "Addrline_addr:=makeLineAddress(in_msg.addr);"
            "trigger(Event:Memory_Ack,line_addr,TBEs[line_addr]);",
            memory_ingress,
        )
        self.assertIn("tbe.Requestor:=in_msg.Requestor;", allocate)
        for response in (read_response, owner_read_response, write_response):
            self.assertIn("out_msg.Destination.add(tbe.Requestor);", response)


if __name__ == "__main__":
    unittest.main()
