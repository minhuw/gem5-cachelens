# SPDX-License-Identifier: BSD-3-Clause
"""Device issuance accounting contracts, independent of Ruby fragmentation."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


class DeviceDmaAccountingTest(unittest.TestCase):
    def test_no_request_or_ruby_semantic_interface(self):
        paths = [ROOT / "src/mem/request.hh",
                 ROOT / "src/mem/ruby/structures/CacheMemory.hh",
                 ROOT / "src/mem/ruby/protocol/RubySlicc_Types.sm"]
        for path in paths:
            self.assertNotRegex(path.read_text(),
                                r"NIC_(?:RX|TX|PVRDMA)|isNic|profileNic|NicDmaSource")
        for path in (ROOT / "src/mem/ruby/protocol").rglob("*.sm"):
            self.assertNotIn("emit_rx_write_stash", path.read_text())

    def test_e1000_counts_at_each_issuance_wrapper(self):
        source = (ROOT / "src/dev/net/i8254xGBe.hh").read_text()
        for helper, op, count, byte_count in (
            ("descRead", "dmaRead", "descDmaReads", "descDmaRdBytes"),
            ("descWrite", "dmaWrite", "descDmaWrites", "descDmaWrBytes"),
            ("txPayloadRead", "dmaRead", "payloadDmaReads", "payloadDmaRdBytes"),
            ("rxPayloadWrite", "dmaWrite", "payloadDmaWrites", "payloadDmaWrBytes"),
        ):
            body = re.search(rf"\n    {helper}\(.*?\n    }}", source, re.S).group()
            self.assertEqual(body.count(count + "++"), 1)
            self.assertEqual(body.count(byte_count + " += size"), 1)
            self.assertEqual(body.count(op + "(addr, size, event, data, delay)"), 1)
            self.assertNotIn("Request::", body)
        impl = (ROOT / "src/dev/net/i8254xGBe.cc").read_text()
        # All ordinary DMA issuance is routed through the counted wrappers.
        self.assertNotRegex(impl, r"\b(?:dmaRead|dmaWrite|dmaAction)\(")
        self.assertIn("igbe->descWrite(", impl)  # Includes TX head writeback.

    def test_pvrdma_counts_before_dma_fragmentation(self):
        source = (ROOT / "src/dev/rdma/pvrdma.cc").read_text()
        body = source.split("Pvrdma::startTransportDma(", 1)[1].split(
            "\nbool\n", 1)[0]
        self.assertIn("bool payload)", body)
        self.assertNotIn("Request::", body)
        self.assertIn("dmaStats.payloadDmaRdBytes += size;", body)
        self.assertIn("dmaStats.payloadDmaWrBytes += size;", body)
        self.assertIn("dmaStats.descDmaRdBytes += size;", body)
        self.assertIn("dmaStats.descDmaWrBytes += size;", body)
        self.assertEqual(source.count("dmaPort.dmaAction("), 1)
        self.assertNotRegex(source, r"\b(?:dmaRead|dmaWrite)\(")
        self.assertNotIn("payloadDmaFlags", source)

    def test_simbricks_adapter_is_plain_dma(self):
        for name in ("pci.cc", "pci.hh", "SimBricks.py"):
            source = (ROOT / "src/simbricks" / name).read_text()
            self.assertNotRegex(source, r"semantic_dma|Semantic|semanticFlags|semanticStats")
        source = (ROOT / "src/simbricks/pci.cc").read_text()
        self.assertIn("initializedCheckpointBegin", source)
        self.assertIn("MemCmd::ReadReq, pciToDma(addr), len", source)
        self.assertIn("MemCmd::WriteReq, pciToDma(addr), len", source)


if __name__ == "__main__":
    unittest.main()
