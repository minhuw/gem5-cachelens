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
_DMA_MACHINE = (
    _REPO_ROOT / "src/mem/ruby/protocol/MESI_Two_Level-dma.sm"
)


def _action_body(source: str, action_name: str) -> str:
    action = re.search(rf"\baction\({re.escape(action_name)}\b", source)
    if action is None:
        raise AssertionError(f"missing SLICC action {action_name}")

    body_start = source.find("{", action.end())
    if body_start == -1:
        raise AssertionError(f"missing body for SLICC action {action_name}")

    depth = 0
    for index in range(body_start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[body_start + 1 : index]

    raise AssertionError(f"unterminated body for SLICC action {action_name}")


class MESIDMAProvenanceTest(unittest.TestCase):
    def test_read_and_write_forward_all_metadata_to_directory(self) -> None:
        source = _DMA_MACHINE.read_text(encoding="utf-8")
        expected_assignments = {
            "accessAddr": "in_msg.PhysicalAddress",
            "accessSize": "in_msg.Len",
            "accessMask": "in_msg.writeMask",
            "seqReq": "in_msg.seqReq",
            "isSeqReqValid": "in_msg.isSeqReqValid",
        }

        for action_name in ("s_sendReadRequest", "s_sendWriteRequest"):
            with self.subTest(action=action_name):
                body = _action_body(source, action_name)
                compact_body = re.sub(r"\s+", "", body)

                for field, value in expected_assignments.items():
                    assignment = f"out_msg.{field}:="
                    self.assertEqual(compact_body.count(assignment), 1)
                    self.assertIn(
                        f"{assignment}{value};",
                        compact_body,
                    )

                destination = (
                    "out_msg.Destination.add(mapAddressToMachine("
                    "address,MachineType:Directory));"
                )
                self.assertEqual(
                    compact_body.count("out_msg.Destination.add("), 1
                )
                self.assertEqual(compact_body.count(destination), 1)


if __name__ == "__main__":
    unittest.main()
