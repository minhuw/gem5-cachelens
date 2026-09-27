# SPDX-License-Identifier: BSD-3-Clause
"""Asset-free policy/provenance checks; runtime scenarios cover coherence/data."""
import ast
import re
import unittest
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[3]
PROTOCOL = ROOT / "src/mem/ruby/protocol"


class DDIOEligibilityTest(unittest.TestCase):
    def test_board_selects_network_ports_not_storage(self):
        source = ast.parse((ROOT / "src/python/gem5/prebuilt/cachelens/board.py").read_text())
        methods = [n for n in ast.walk(source) if isinstance(n, ast.FunctionDef)
                   and n.name == "get_ddio_dma_ports"]
        self.assertEqual(len(methods), 2)  # ARM and x86
        ide, nic, rdma = object(), object(), object()
        board = SimpleNamespace(nics=[SimpleNamespace(dma=nic)],
                                rdmas=[SimpleNamespace(dma=rdma)],
                                pc=SimpleNamespace(south_bridge=SimpleNamespace(
                                    ide=SimpleNamespace(dma=ide))))
        for method in methods:
            ns = {}
            exec(compile(ast.Module(body=[method], type_ignores=[]), "board", "exec"), ns)
            self.assertEqual(ns[method.name](board), [nic, rdma])
            self.assertEqual(ns[method.name](SimpleNamespace()), [])

    def test_mesi_policy_never_classifies_tags(self):
        dma = (PROTOCOL / "MESI_Two_Level-dma.sm").read_text()
        l2 = (PROTOCOL / "MESI_Two_Level-L2cache.sm").read_text()
        self.assertIn('bool ddio_eligible := "False";', dma)
        self.assertNotIn("isNicDma", dma)
        ingress = l2.split("in_port(L1RequestL2Network_in", 1)[1].split("// ACTIONS", 1)[0]
        self.assertEqual(ingress.count("if (in_msg.ddioEligible)"), 2)
        self.assertNotIn("isNic", ingress)
        self.assertNotIn("DMA_DESC_READ", l2)
        for states in ("NP", "{SS, M}", "MT"):
            self.assertIn(f"transition({states}, DMA_READ_NOALLOC", l2)

    def test_chi_eligibility_survives_creation_forwarding_and_retry(self):
        funcs = (PROTOCOL / "chi/CHI-cache-funcs.sm").read_text()
        actions = (PROTOCOL / "chi/CHI-cache-actions.sm").read_text()
        creation = actions.split("action(AllocateTBE_SeqRequest", 1)[1].split("action(", 1)[0]
        self.assertIn("out_msg.ddioEligible := ddio_eligible;", creation)
        for func in ("allocateRequestTBE", "prepareRequest", "prepareRequestRetry"):
            body = re.split(rf"\b{func}\(", funcs, maxsplit=1)[1].split("\n}", 1)[0]
            self.assertIn("ddioEligible :=", body)
        policy = funcs.split("tbe.is_ddio_write :=", 1)[1].split("// Optional", 1)[0]
        self.assertNotIn("isNic", policy)
        self.assertNotIn("seqReq", policy)
        for opcode in ("WriteUniqueFull", "WriteUniquePtl", "ReadOnce", "ReadShared", "ReadUnique"):
            self.assertIn(f"CHIRequestType:{opcode}", policy)
        self.assertIn("else if (tbe.is_ddio_read)", actions)
        fill = actions.split("action(CheckCacheFill", 1)[1].split("\naction(", 1)[0]
        self.assertIn("tbe.is_ddio_read && is_invalid(cache_entry)", fill)
        merge = actions.split("action(Prepare_DDIOWrite", 1)[1].split("\naction(", 1)[0]
        self.assertIn("cache.isNicDdioWriteReq(tbe.seqReq)", merge)
        profiling = funcs.split("// Profile NIC DMA", 1)[1].split("tbe.ddio_access_profiled", 1)[0]
        self.assertNotIn("tbe.is_ddio_write", profiling)


if __name__ == "__main__":
    unittest.main()
