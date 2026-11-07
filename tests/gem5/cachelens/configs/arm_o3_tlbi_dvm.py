# Copyright (c) 2026 minhuw
# SPDX-License-Identifier: BSD-3-Clause

"""Bare-metal O3 TLBI/DVM checks on the CacheLens ARM CHI hierarchy.

Asset-free: the guest is a few hand-assembled AArch64 instructions written to
an ELF in the output directory. Cores start at EL3 with HCR_EL2.FB=0.

DVM decoding needs at least two cores, so every scenario uses two.

local-tlbi: core 0 executes local TLBIs while core 1 waits. With DVM
    decoding they are micro-architectural loads; FB=0 sends no DVM message,
    so O3 must complete them without a memory access instead of waiting at
    the ROB head forever.
sync-retry: core 0 repeats broadcast TLBI + DSB while core 1 keeps its L1D
    sequencer busy with loads behind unpredictable branches. Each DSB makes
    core 1 send TLBI_EXT_SYNC_COMP; a full sequencer must delay it with a
    retry, not panic. Squashed loads still occupy sequencer slots.
"""

import argparse
import re
import struct
import sys
from pathlib import Path

import m5
from m5.objects import ArmFsWorkload, IdeController, Root

from gem5.components.memory import DualChannelDDR4_2400
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.isas import ISA
from gem5.prebuilt.cachelens.board import CacheLensArmBoard
from gem5.prebuilt.cachelens.cache_hierarchy import CacheLensCHIHierarchy

M5_EXIT = 0xFF210110  # m5 exit (x0 = 0 delay)

LOCAL_TLBI = (
    0xD53800A1,  # mrs x1, mpidr_el1
    0x92405C21,  # and x1, x1, #0xffffff
    0xB50000E1,  # cbnz x1, park
    0xD508871F,  # tlbi vmalle1
    0xD508873F,  # tlbi vae1, xzr
    0xD5033F9F,  # dsb sy
    0xD5033FDF,  # isb
    0xD2800000,  # mov x0, #0
    M5_EXIT,
    0xD503207F,  # park: wfi
    0x17FFFFFF,  # b park
)

# Identity map with 1 GiB blocks at EL3 (TTBR0_EL3 at image offset 0x1000):
# device below 2 GiB, inner-shareable write-back DRAM from 2 GiB.
SYNC_RETRY = (
    0x10008001,  # adr x1, ttb
    0xD51E2001,  # msr ttbr0_el3, x1
    0xD29FE001,  # mov x1, #0xff00          (attr0 device, attr1 WB)
    0xD51EA201,  # msr mair_el3, x1
    0xD286A401,  # mov x1, #0x3520          (T0SZ=32, WBWA, inner)
    0xF2B01001,  # movk x1, #0x8080, lsl #16 (RES1 bits)
    0xD51E2041,  # msr tcr_el3, x1
    0xD5033FDF,  # isb
    0xD50E871F,  # tlbi alle3
    0xD5033F9F,  # dsb sy
    0xD5033FDF,  # isb
    0xD53E1001,  # mrs x1, sctlr_el3
    0xD28200A2,  # mov x2, #0x1005          (M, C, I)
    0xAA020021,  # orr x1, x1, x2
    0xD51E1001,  # msr sctlr_el3, x1
    0xD5033FDF,  # isb
    0xD53800A1,  # mrs x1, mpidr_el1
    0x92405C21,  # and x1, x1, #0xffffff
    0xB5000121,  # cbnz x1, reader
    # writer (core 0):
    0xD2802003,  # mov x3, #256
    0xD508831F,  # 1: tlbi vmalle1is
    0xD5033B9F,  # dsb ish
    0xF1000463,  # subs x3, x3, #1
    0x54FFFFA1,  # b.ne 1b
    0xD2800000,  # mov x0, #0
    M5_EXIT,
    0x14000000,  # b .
    # reader (core 1):
    0xD2B20006,  # mov x6, #0x90000000
    0xD2800007,  # mov x7, #0
    0xD289E369,  # mov x9, #0x4f1b
    0xD28FE5AA,  # mov x10, #0x7f2d
    0xF2A992AA,  # movk x10, #0x4c95, lsl #16
    0xF2DE85AA,  # movk x10, #0xf42d, lsl #32
    0xF2EB0A2A,  # movk x10, #0x5851, lsl #48
    0x9B0A7D29,  # 2: mul x9, x9, x10
    0x91000529,  # add x9, x9, #1
    0xF86768C8,  # ldr x8, [x6, x7]
    0x8B090108,  # add x8, x8, x9
    0xB6080048,  # tbz x8, #33, 3f       (resolves after the miss)
    0x910100E7,  # add x7, x7, #0x40
    0x8B0700CB,  # 3: add x11, x6, x7
    0xF948016C,  # ldr x12, [x11, #0x1000]
    0xF950016D,  # ldr x13, [x11, #0x2000]
    0xF958016E,  # ldr x14, [x11, #0x3000]
    0xF960016F,  # ldr x15, [x11, #0x4000]
    0xF9680170,  # ldr x16, [x11, #0x5000]
    0xF9700171,  # ldr x17, [x11, #0x6000]
    0xF9780172,  # ldr x18, [x11, #0x7000]
    0x910100E7,  # add x7, x7, #0x40
    0x924054E7,  # and x7, x7, #0x3fffff
    0x17FFFFF0,  # b 2b
)
TRANSLATION_TABLE = (
    0x0000000000000401,  # 0-1 GiB: AF, attr0, block
    0x0000000040000401,  # 1-2 GiB
    0x0000000080000705,  # 2-3 GiB: AF, inner shareable, attr1, block
    0x00000000C0000705,  # 3-4 GiB
)


def image(scenario):
    code = struct.pack(
        "<%dI" % len(LOCAL_TLBI if scenario == "local-tlbi" else SYNC_RETRY),
        *(LOCAL_TLBI if scenario == "local-tlbi" else SYNC_RETRY),
    )
    if scenario == "local-tlbi":
        return code
    return code.ljust(0x1000, b"\0") + struct.pack("<4Q", *TRANSLATION_TABLE)


def write_elf(path, payload):
    """One loadable segment at address 0; the platform adds the DRAM base."""
    offset = 0x1000
    header = b"\x7fELF" + bytes((2, 1, 1)) + bytes(9)
    header += struct.pack(
        "<HHIQQQIHHHHHH", 2, 183, 1, 0, 64, 0, 0, 64, 56, 1, 0, 0, 0
    )
    program = struct.pack(
        "<IIQQQQQQ", 1, 7, offset, 0, 0, len(payload), len(payload), 0x1000
    )
    path.write_bytes((header + program).ljust(offset, b"\0") + payload)


parser = argparse.ArgumentParser()
parser.add_argument(
    "--scenario", choices=("local-tlbi", "sync-retry"), required=True
)
parser.add_argument("--max-ticks", type=int, default=2_000_000_000)
parser.add_argument("--l1d-max-outstanding", type=int, default=4)
args = parser.parse_args()

hierarchy = CacheLensCHIHierarchy()
board = CacheLensArmBoard(
    clk_freq="2GHz",
    processor=SimpleProcessor(
        cpu_type=CPUTypes.O3, isa=ISA.ARM, num_cores=2
    ),
    memory=DualChannelDDR4_2400(size="1GiB"),
    cache_hierarchy=hierarchy,
)
elf = Path(m5.options.outdir) / f"{args.scenario}.elf"
write_elf(elf, image(args.scenario))

board._set_fullsystem(True)
board._bootloader = []
board._add_pci_device(IdeController(disks=[]))
board.workload = ArmFsWorkload(object_file=str(elf))
board._pre_instantiate()
assert Root.getInstance() is not None
for cluster in hierarchy.core_clusters:
    assert bool(cluster.l1d.sequencer.is_cpu_sequencer)
    cluster.l1d.sequencer.max_outstanding_requests = args.l1d_max_outstanding
for core in board.get_processor().get_cores():
    assert all(bool(d.dvm_enabled) for d in core.get_simobject().decoder)

m5.instantiate()
board._post_instantiate()
event = m5.simulate(args.max_ticks)
cause = event.getCause()
m5.stats.dump()
if cause != "m5_exit instruction encountered":
    print(f"CACHELENS_O3_TLBI_FAIL scenario={args.scenario} cause={cause!r}")
    sys.exit(1)

blocked = 0
if args.scenario == "sync-retry":
    stats = (Path(m5.options.outdir) / "stats.txt").read_text()
    blocked = sum(
        int(value)
        for value in re.findall(r"\.lsq\.tlbiSyncCompBlocked\s+(\d+)", stats)
    )
    if not blocked:
        print("CACHELENS_O3_TLBI_FAIL scenario=sync-retry no blocked completion")
        sys.exit(1)
print(
    f"CACHELENS_O3_TLBI_OK scenario={args.scenario} tick={m5.curTick()} "
    f"sync_comp_blocked={blocked}"
)
