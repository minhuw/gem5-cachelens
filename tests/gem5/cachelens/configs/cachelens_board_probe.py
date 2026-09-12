# Copyright (c) 2026 The Regents of the University of California
# All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause

import argparse
from pathlib import Path

from gem5.components.cachehierarchies.classic.no_cache import NoCache
from gem5.components.memory import DualChannelDDR4_2400
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.isas import ISA

parser = argparse.ArgumentParser()
parser.add_argument("--isa", choices=("arm", "x86"), required=True)
parser.add_argument("--num-nics", type=int, required=True)
parser.add_argument("--expect-reject", action="store_true")
args = parser.parse_args()

isa = ISA.ARM if args.isa == "arm" else ISA.X86
processor = SimpleProcessor(
    cpu_type=CPUTypes.ATOMIC,
    isa=isa,
    num_cores=1,
)
memory = DualChannelDDR4_2400(size="2GiB")
network_options = {
    "num_nics": args.num_nics,
    "num_loadgens": args.num_nics,
}

board_type = None
if isa == ISA.ARM:
    from gem5.prebuilt.cachelens.board import CacheLensArmBoard

    board_type = CacheLensArmBoard
else:
    from gem5.prebuilt.cachelens.board import CacheLensX86Board

    board_type = CacheLensX86Board

try:
    board = board_type(
        clk_freq="1GHz",
        processor=processor,
        memory=memory,
        cache_hierarchy=NoCache(),
        network_options=network_options,
    )
except ValueError as error:
    if not args.expect_reject:
        raise
    message = str(error)
    if isa == ISA.ARM:
        assert "at most three NICs" in message
        assert "IDE controller's INTx route" in message
    else:
        assert "supports one NIC" in message
    print(
        f"CACHELENS_BOARD_REJECT_OK isa={args.isa} "
        f"nics={args.num_nics} error={message}"
    )
    raise SystemExit(0)

assert not args.expect_reject, "NIC count was expected to be rejected"

# The probe only needs Python-level PCI attachment; no workload image is read
# and no SimObject is instantiated.
class _DiskImage:
    def get_local_path(self) -> str:
        return Path(__file__).as_posix()


board._set_fullsystem(True)
board._add_disk_to_board(_DiskImage())

expected_bdfs = [
    f"0000:00:{2 + index:02x}.0" for index in range(args.num_nics)
]
assert board.get_nic_bdfs() == expected_bdfs
assert len(board.nics) == args.num_nics
assert len(board.loadgens) == args.num_nics
assert len(board.links) == args.num_nics
assert [str(nic.hardware_address) for nic in board.nics] == [
    f"02:90:00:00:00:{index + 1:02x}"
    for index in range(args.num_nics)
]
assert [int(loadgen.loadgen_id) for loadgen in board.loadgens] == list(
    range(args.num_nics)
)

if isa == ISA.ARM:
    pci_devices = [int(device.pci_dev) for device in board._pci_devices]
    assert pci_devices == list(range(1, args.num_nics + 2))
    int_count = int(board.realview.pci_host.int_count)
    assert len({device % int_count for device in pci_devices}) == len(
        pci_devices
    )

print(
    f"CACHELENS_BOARD_CONFIG_OK isa={args.isa} nics={args.num_nics} "
    f"bdfs={','.join(expected_bdfs)}"
)
