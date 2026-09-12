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
_DMA_SEQUENCER_PY = _REPO_ROOT / "src/mem/ruby/system/Sequencer.py"
_DMA_SEQUENCER_CC = _REPO_ROOT / "src/mem/ruby/system/DMASequencer.cc"


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
                "self._l2_select_num_bits=int(math.log2(num_l2_banks))"
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
        self.assertNotIn("l2_select_num_bits=int(math.log", stdlib_l1)
        self.assertNotIn("l2_select_num_bits=int(math.log", stdlib_dma)

    def test_masked_dma_capability_is_default_off_and_mesi_only(self) -> None:
        sequencer = _compact(
            _DMA_SEQUENCER_PY.read_text(encoding="utf-8")
        )
        self.assertEqual(
            sequencer.count("supports_masked_writes=Param.Bool(False,"), 1
        )

        enabled = {}
        for root in (
            _REPO_ROOT / "configs",
            _REPO_ROOT / "src/python",
        ):
            for path in root.rglob("*.py"):
                count = _compact(path.read_text(encoding="utf-8")).count(
                    "supports_masked_writes=True"
                )
                if count:
                    enabled[path] = count

        self.assertEqual(enabled, {_LEGACY: 2, _STDLIB: 1})

    def test_masked_dma_rejection_precedes_protocol_admission(self) -> None:
        source = _DMA_SEQUENCER_CC.read_text(encoding="utf-8")
        body = _compact(
            _braced_body(source, "DMASequencer::makeRequest(PacketPtr pkt)")
        )
        rejection = (
            "panic_if(!isSupportedDMARequest(is_masked_write,"
            "m_supports_masked_writes)"
        )
        self.assertIn(rejection, body)
        self.assertIn("supports_masked_writesisfalse", body)
        self.assertLess(
            body.index(rejection), body.index("issueRequestFragment")
        )

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

    def test_l2_plan004_states_events_and_routes_are_complete(self) -> None:
        source = _L2.read_text(encoding="utf-8")
        state_body = _braced_body(source, "state_declaration(State")
        states = set(
            re.findall(r"^\s*([A-Z][A-Z0-9_]*)\s*,", state_body, re.M)
        )
        stable = {"NP", "SS", "M", "MT"}
        legacy_transient = {
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
        ddio_transient = {
            "DM_WF",
            "DM_WI",
            "DM_WS",
            "DM_WM",
            "DM_RT",
        }
        self.assertEqual(states, stable | legacy_transient | ddio_transient)

        event_body = _braced_body(source, "enumeration(Event")
        events = set(
            re.findall(r"^\s*([A-Za-z][A-Za-z0-9_]*)\s*,", event_body, re.M)
        )
        self.assertEqual(
            events,
            {
                "L1_GET_INSTR",
                "L1_GETS",
                "L1_GETX",
                "L1_UPGRADE",
                "L1_PUTX",
                "L1_PUTX_old",
                "DMA_WRITE_FULL",
                "DMA_WRITE_PARTIAL",
                "DMA_WRITE_NORETAIN",
                "DMA_TX_READ",
                "DMA_DESC_READ",
                "DMA_OTHER",
                "L2_Replacement",
                "L2_Replacement_clean",
                "DDIO_Replacement",
                "DDIO_Replacement_clean",
                "Mem_Data",
                "Mem_Ack",
                "Ddio_Ack",
                "WB_Data",
                "WB_Data_clean",
                "Ack",
                "Ack_all",
                "Unblock",
                "Exclusive_Unblock",
                "MEM_Inv",
            },
        )

        compact = _compact(source)
        for transition in (
            "transition(NP,DMA_WRITE_FULL,DM_WI)",
            "transition(M,DMA_WRITE_FULL)",
            "transition(SS,DMA_WRITE_FULL,DM_WS)",
            "transition(MT,DMA_WRITE_FULL,DM_WM)",
            "transition(NP,DMA_WRITE_PARTIAL,DM_WF)",
            "transition(DM_WF,Mem_Data,DM_WI)",
            "transition(M,DMA_WRITE_PARTIAL)",
            "transition(SS,DMA_WRITE_PARTIAL,DM_WS)",
            "transition(MT,DMA_WRITE_PARTIAL,DM_WM)",
            "transition(NP,DMA_TX_READ)",
            "transition({SS,M},DMA_TX_READ)",
            "transition(MT,DMA_TX_READ,DM_RT)",
            "transition({NP,SS,M,MT},DMA_WRITE_NORETAIN)",
            "transition({NP,SS,M,MT},{DMA_DESC_READ,DMA_OTHER})",
        ):
            self.assertIn(transition, compact)

        transient_transition = (
            "transition({M_I,MT_I,MCT_I,I_I,S_I,ISS,IS,IM,SS_MB,MT_MB,"
            "MT_IIB,MT_IB,MT_SB,DM_WF,DM_WI,DM_WS,DM_WM,DM_RT},"
            "{DMA_WRITE_FULL,DMA_WRITE_PARTIAL,DMA_WRITE_NORETAIN,"
            "DMA_TX_READ,DMA_DESC_READ,DMA_OTHER})"
            "{zdr_recycleDMARequestQueue;}"
        )
        self.assertIn(transient_transition, compact)

    def test_partial_writes_keep_request_and_authoritative_data_separate(
        self,
    ) -> None:
        source = _L2.read_text(encoding="utf-8")
        compact = _compact(source)
        ingress = _compact(
            _braced_body(source, "in_port(L1RequestL2Network_in")
        )
        allocate = _compact(_action_body(source, "di_allocateDmaTBE"))
        merge = _compact(_action_body(source, "dwt_mergeWriteFromTBE"))
        owner = _compact(_action_body(source, "dic_requireOwnerData"))
        memory = _compact(
            _action_body(source, "dmc_captureAuthoritativeMemoryLine")
        )

        self.assertNotIn("unsupporteduntilPlan004", ingress)
        self.assertIn("boolfull_line:=in_msg.accessMask.isFull();", ingress)
        self.assertIn("trigger(Event:DMA_WRITE_PARTIAL", ingress)
        self.assertIn("tbe.DataBlkValid.clear();", allocate)
        self.assertIn("tbe.DmaDataBlk:=in_msg.DataBlk;", allocate)
        self.assertIn("tbe.DmaAccessMask:=in_msg.accessMask;", allocate)
        self.assertNotIn("tbe.DataBlk:=in_msg.DataBlk;", allocate)
        self.assertIn(
            "assert(tbe.DataBlkValid.isFull()||"
            "tbe.DmaAccessMask.isFull());",
            merge,
        )
        self.assertIn(
            "tbe.DataBlk.copyPartial(tbe.DmaDataBlk,tbe.DmaAccessMask);",
            merge,
        )
        self.assertIn("tbe.DataBlkValid.clear();", owner)
        self.assertIn("tbe.DataBlk:=in_msg.DataBlk;", memory)
        self.assertIn("tbe.DataBlkValid.fillMask();", memory)
        self.assertIn(
            "transition(DM_WM,{WB_Data,WB_Data_clean},M)"
            "{qq_writeDataToTBE;dwt_mergeWriteFromTBE;",
            compact,
        )

    def test_proxy_and_recycle_telemetry_do_not_mutate_protocol_state(
        self,
    ) -> None:
        source = _L2.read_text(encoding="utf-8")
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
            "ddioReplacementStalls",
            "ddioOwnershipRequests",
            "ddioOwnershipAcks",
        ):
            self.assertIn(f"statistics::Scalar{stat};", header)
            self.assertIn(f"ADD_STAT({stat},", implementation)
            self.assertNotIn(f"{stat}.flags(", implementation)
        for method in (
            "profileDmaRoutingProxy",
            "profileDmaRoutingTransientRecycle",
            "profileDdioReplacementStall",
            "profileDdioOwnershipRequest",
            "profileDdioOwnershipAck",
        ):
            self.assertIn(f"void{method}();", slicc_types)

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
        self.assertIn(
            "L2cache.profileDdioReplacementStall();",
            _compact(_L2.read_text(encoding="utf-8")),
        )

    def test_ddio_zero_valued_runtime_stats_remain_printable(self) -> None:
        implementation = _compact(
            _CACHE_MEMORY_CC.read_text(encoding="utf-8")
        )
        for stat in (
            "rxPayloadRequests",
            "rxPayloadHits",
            "rxPayloadMisses",
            "rxHeaderRequests",
            "rxHeaderHits",
            "rxHeaderMisses",
            "txPayloadRequests",
            "txPayloadHits",
            "txPayloadMisses",
        ):
            self.assertNotIn(
                f"{stat}.flags(statistics::nozero);", implementation
            )

        vector_sizes = {
            "rxPayloadHitWays": "m_cache_assoc",
            "rxPayloadAllocWays": "m_cache_assoc",
            "ddioAllocWays": "m_cache_assoc",
            "ddioWayAccess": "5*m_cache_assoc",
            "ddioWayFill": "5*m_cache_assoc",
            "wayDeallocations": "m_cache_assoc",
        }
        for vector, size in vector_sizes.items():
            self.assertIn(
                f"{vector}.init({size}).flags(statistics::total);",
                implementation,
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

    def test_directory_masked_dma_races_merge_before_single_ack(
        self,
    ) -> None:
        source = _DIRECTORY.read_text(encoding="utf-8")
        compact = _compact(source)
        allocate = _compact(_action_body(source, "v_allocateTBE"))
        read_replay = _compact(
            _action_body(source, "qft_queueMemoryFetchRequestDMATBE")
        )
        full_write_replay = _compact(
            _action_body(source, "qwf_queueSavedFullDMAWrite")
        )
        owner_write = _compact(
            _action_body(source, "qw_mergeOwnerDataAndWrite")
        )
        memory_write = _compact(
            _action_body(source, "qmm_mergeMemoryDataAndWrite")
        )

        self.assertIn("tbe.DataBlkValid.clear();", allocate)
        self.assertIn("tbe.DmaDataBlk:=in_msg.DataBlk;", allocate)
        self.assertIn("tbe.DmaAccessMask:=in_msg.accessMask;", allocate)

        self.assertIn("assert(is_valid(tbe));", read_replay)
        self.assertIn("out_msg.addr:=address;", read_replay)
        self.assertIn(
            "out_msg.Type:=MemoryRequestType:MEMORY_READ;", read_replay
        )
        self.assertIn("out_msg.Sender:=tbe.Requestor;", read_replay)
        self.assertNotIn("requestNetwork_in", read_replay)

        self.assertIn("assert(tbe.DmaAccessMask.isFull());", full_write_replay)
        self.assertIn("out_msg.addr:=address;", full_write_replay)
        self.assertIn(
            "out_msg.Type:=MemoryRequestType:MEMORY_WB;", full_write_replay
        )
        self.assertIn("out_msg.DataBlk:=tbe.DmaDataBlk;", full_write_replay)

        for merge in (owner_write, memory_write):
            self.assertIn("tbe.DataBlkValid.fillMask();", merge)
            self.assertIn(
                "tbe.DataBlk.copyPartial(tbe.DmaDataBlk,"
                "tbe.DmaAccessMask);",
                merge,
            )
            self.assertIn("assert(tbe.DataBlkValid.isFull());", merge)
            self.assertIn("out_msg.DataBlk:=tbe.DataBlk;", merge)
            self.assertIn("out_msg.Len:=0;", merge)
        self.assertIn("tbe.DataBlk:=in_msg.DataBlk;", owner_write)
        self.assertIn("out_msg.Sender:=in_msg.Sender;", owner_write)
        self.assertIn("tbe.DataBlk:=in_msg.DataBlk;", memory_write)

        self.assertIn(
            "transition(M_DRD,CleanReplacement,ID){a_sendAck;"
            "qft_queueMemoryFetchRequestDMATBE;"
            "k_popIncomingResponseQueue;}",
            compact,
        )
        self.assertIn(
            "transition(M_DWR,CleanReplacement,ID_W){a_sendAck;"
            "qwf_queueSavedFullDMAWrite;"
            "k_popIncomingResponseQueue;}",
            compact,
        )
        self.assertIn(
            "transition(M_DWR,CleanReplacementPartial,ID_WF){a_sendAck;"
            "qft_queueMemoryFetchRequestDMATBE;"
            "k_popIncomingResponseQueue;}",
            compact,
        )

        for transition in (
            "transition(M_DRD,Data,M_DRDI){drp_sendDMAData;"
            "w_deallocateTBE;qw_queueMemoryWBRequest;"
            "k_popIncomingResponseQueue;}",
            "transition(M_DRDI,Memory_Ack,I){aa_sendAck;"
            "l_popMemQueue;kd_wakeUpDependents;}",
            "transition(ID,Memory_Data,I){dr_sendDMAData;"
            "w_deallocateTBE;l_popMemQueue;kd_wakeUpDependents;}",
            "transition(ID_WF,Memory_Data,ID_W){"
            "qmm_mergeMemoryDataAndWrite;l_popMemQueue;}",
            "transition(M_DWR,Data,M_DWRI){qw_mergeOwnerDataAndWrite;"
            "k_popIncomingResponseQueue;}",
            "transition(M_DWRI,Memory_Ack,I){aa_sendAck;da_sendDMAAck;"
            "w_deallocateTBE;l_popMemQueue;kd_wakeUpDependents;}",
            "transition(ID_W,Memory_Ack,I){da_sendDMAAck;"
            "w_deallocateTBE;l_popMemQueue;kd_wakeUpDependents;}",
        ):
            self.assertIn(transition, compact)

        l2 = _compact(_L2.read_text(encoding="utf-8"))
        self.assertIn(
            "transition(M,L2_Replacement_clean,M_I){i_allocateTBE;"
            "c_exclusiveCleanReplacement;rr_deallocateL2CacheBlock;}",
            l2,
        )
        self.assertIn(
            "transition({I_I,S_I,M_I,MT_I,MCT_I,NP},MEM_Inv){"
            "o_popIncomingResponseQueue;}",
            l2,
        )


if __name__ == "__main__":
    unittest.main()
