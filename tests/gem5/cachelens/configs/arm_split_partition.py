# Copyright (c) 2026 minhuw
# SPDX-License-Identifier: BSD-3-Clause

"""Bare-metal serial vs SplitSim thread partition check on CacheLens ARM CHI.

Asset-free: the guest is a few hand-assembled AArch64 instructions written
to an ELF in the output directory. All cores start at EL3, enable the MMU
with an identity map (inner-shareable write-back DRAM) and run the same
loop, which generates coherence traffic through the HNFs:

- true sharing: every core increments one shared counter (plain ldr/str);
- false sharing: core c increments word c of one shared line;
- read sharing with invalidation: all cores read a sliding window of four
  shared lines, then core c writes line c of that window;
- private streaming: read-modify-write of a private 64 KiB array per core,
  larger than the (small) L2, so lines are written back to the HNFs, whose
  total capacity is also smaller than the footprint.

Each core then writes a done flag and parks in WFI. The run always ends at
the fixed tick --ticks, independent of the guest.

--mode serial runs natively. codec and thread attach the
gem5.prebuilt.cachelens.split_partition layout before m5.instantiate and
activate it at tick 0 (cold). codec keeps one queue but sends every message
on a cut link through capture/value transport; thread runs the HNF queues as
host threads. Every mode must produce the same statistics, apart from the
declared physical-residence average (m_buf_msgs) of cut buffers in thread
mode; the split metadata is written to split.json for that comparison.
"""

import argparse
import json
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

DRAM = 0x80000000  # the image (code at 0, translation table at 0x1000)
SHARED = DRAM + 0x100000  # line 0: counter, line 1: false sharing
WINDOW = DRAM + 0x110000  # 16 KiB read-shared window ring
DONE = DRAM + 0x180000  # one done-flag line per core
PRIVATE = DRAM + 0x200000  # 64 KiB per core, 1 MiB apart

# Identity map with 1 GiB blocks at EL3 (TTBR0_EL3 at image offset 0x1000):
# device below 2 GiB, inner-shareable write-back DRAM from 2 GiB.
TRANSLATION_TABLE = (
    0x0000000000000401,  # 0-1 GiB: AF, attr0, block
    0x0000000040000401,  # 1-2 GiB
    0x0000000080000705,  # 2-3 GiB: AF, inner shareable, attr1, block
    0x00000000C0000705,  # 3-4 GiB
)


class Assembler:
    """Just the A64 encodings this guest needs, with backward labels."""

    def __init__(self):
        self.words, self.labels = [], {}

    def label(self, name):
        self.labels[name] = len(self.words)

    def emit(self, word):
        self.words.append(word)

    def offset(self, name):
        return self.labels[name] - len(self.words)

    def movz(self, rd, imm, shift=0):
        self.emit(0xD2800000 | (shift // 16) << 21 | imm << 5 | rd)

    def movk(self, rd, imm, shift):
        self.emit(0xF2800000 | (shift // 16) << 21 | imm << 5 | rd)

    def mov64(self, rd, value):
        self.movz(rd, value & 0xFFFF)
        for shift in (16, 32, 48):
            if (value >> shift) & 0xFFFF:
                self.movk(rd, (value >> shift) & 0xFFFF, shift)

    def add_imm(self, rd, rn, imm):
        assert 0 <= imm < 4096
        self.emit(0x91000000 | imm << 10 | rn << 5 | rd)

    def subs_imm(self, rd, rn, imm):
        assert 0 <= imm < 4096
        self.emit(0xF1000000 | imm << 10 | rn << 5 | rd)

    def add_lsl(self, rd, rn, rm, amount=0):
        self.emit(0x8B000000 | rm << 16 | amount << 10 | rn << 5 | rd)

    def and_low(self, rd, rn, bits):
        """rd = rn & ((1 << bits) - 1), a bitmask immediate (N=1)."""
        assert 1 <= bits < 64
        self.emit(0x92400000 | (bits - 1) << 10 | rn << 5 | rd)

    def ldr(self, rt, rn, offset=0):
        assert offset % 8 == 0 and 0 <= offset < 32768
        self.emit(0xF9400000 | (offset // 8) << 10 | rn << 5 | rt)

    def str(self, rt, rn, offset=0):
        assert offset % 8 == 0 and 0 <= offset < 32768
        self.emit(0xF9000000 | (offset // 8) << 10 | rn << 5 | rt)

    def ldr_index(self, rt, rn, rm):
        """ldr rt, [rn, rm, lsl #3]"""
        self.emit(0xF8607800 | rm << 16 | rn << 5 | rt)

    def str_index(self, rt, rn, rm):
        """str rt, [rn, rm, lsl #3]"""
        self.emit(0xF8207800 | rm << 16 | rn << 5 | rt)

    def b_ne(self, name):
        self.emit(0x54000001 | (self.offset(name) & 0x7FFFF) << 5)

    def b(self, name):
        self.emit(0x14000000 | (self.offset(name) & 0x3FFFFFF))


def image(iterations):
    """Return the image and the instructions each core commits to WFI."""
    asm = Assembler()
    # MMU and caches on, exactly as the TLBI sync-retry guest; adr is
    # patched below once the code size is known.
    for word in (
        0x10000001,  # adr x1, ttb
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
    ):
        asm.emit(word)
    asm.and_low(19, 1, 8)  # x19 = core (Aff0)
    asm.mov64(21, SHARED)
    asm.mov64(24, WINDOW)
    asm.mov64(22, PRIVATE)
    asm.add_lsl(22, 22, 19, 20)  # + core MiB
    asm.movz(23, 0)  # private offset
    asm.movz(25, 0)  # window offset
    asm.mov64(20, iterations)
    asm.label("loop")
    asm.ldr(1, 21)  # true sharing: shared counter += 1
    asm.add_imm(1, 1, 1)
    asm.str(1, 21)
    asm.add_imm(2, 21, 0x40)  # false sharing: word[core] += 1
    asm.ldr_index(3, 2, 19)
    asm.add_imm(3, 3, 1)
    asm.str_index(3, 2, 19)
    asm.add_lsl(4, 24, 25)  # read the four window lines
    asm.ldr(5, 4)
    asm.ldr(6, 4, 0x40)
    asm.ldr(7, 4, 0x80)
    asm.ldr(8, 4, 0xC0)
    asm.add_lsl(9, 4, 19, 6)  # then write window line[core]
    asm.str(20, 9)
    asm.add_lsl(10, 22, 23)  # private streaming read-modify-write
    asm.ldr(11, 10)
    asm.add_lsl(11, 11, 5)
    asm.str(11, 10)
    asm.add_imm(25, 25, 0x100)
    asm.and_low(25, 25, 14)  # 16 KiB window ring
    asm.add_imm(23, 23, 0x40)
    asm.and_low(23, 23, 16)  # 64 KiB private ring
    asm.subs_imm(20, 20, 1)
    asm.b_ne("loop")
    loop = len(asm.words) - asm.labels["loop"]
    asm.mov64(12, DONE)
    asm.add_lsl(12, 12, 19, 6)
    asm.movz(1, 1)
    asm.str(1, 12)
    asm.emit(0xD5033F9F)  # dsb sy
    asm.label("park")
    asm.emit(0xD503207F)  # wfi
    asm.b("park")
    assert len(asm.words) * 4 <= 0x1000
    asm.words[0] |= (0x1000 >> 2) << 5  # adr x1, ttb (image offset 0x1000)
    code = struct.pack("<%dI" % len(asm.words), *asm.words)
    committed = asm.labels["park"] + (iterations - 1) * loop
    table = struct.pack("<4Q", *TRANSLATION_TABLE)
    return code.ljust(0x1000, b"\0") + table, committed


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


def stat_sum(stats, pattern):
    return sum(int(float(v)) for v in re.findall(pattern, stats, re.M))


parser = argparse.ArgumentParser()
parser.add_argument(
    "--mode", choices=("serial", "native", "codec", "fenced", "thread"), required=True
)
parser.add_argument("--layout", choices=("hnf1", "hnf2"), default="hnf1")
parser.add_argument("--cores", type=int, choices=(2, 3, 4), default=4)
parser.add_argument("--iterations", type=int, default=2000)
parser.add_argument("--ticks", type=int, default=200_000_000)
parser.add_argument("--quantum", type=int, default=1000)
parser.add_argument("--spin-wait", action="store_true")
parser.add_argument("--image-only", action="store_true")
parser.add_argument("--exercise-io", action="store_true")
parser.add_argument("--deactivate-at", type=int, default=0)
args = parser.parse_args()

outdir = Path(m5.options.outdir)
elf = outdir / "split-partition.elf"
payload, committed = image(args.iterations)
write_elf(elf, payload)
if args.image_only:
    print(f"CACHELENS_SPLIT_IMAGE {elf}")
    sys.exit(0)

# The experiment's clocks: 1 GHz board/Ruby (int links 1000 ticks), 2 GHz
# cores and private caches. Small caches make the HNFs see write-backs and
# capacity evictions; four HNFs allow the two-queue layout.
hierarchy = CacheLensCHIHierarchy(
    l1i_size="8KiB",
    l1d_size="8KiB",
    l2_size="32KiB",
    hnf_size="32KiB",
    num_hnfs=4,
    core_clock="2GHz",
)
board = CacheLensArmBoard(
    clk_freq="1GHz",
    processor=SimpleProcessor(
        cpu_type=CPUTypes.O3, isa=ISA.ARM, num_cores=args.cores
    ),
    memory=DualChannelDDR4_2400(size="1GiB"),
    cache_hierarchy=hierarchy,
)
board._set_fullsystem(True)
board._bootloader = []
board._add_pci_device(IdeController(disks=[]))
board.workload = ArmFsWorkload(object_file=str(elf))
board._pre_instantiate()
root = Root.getInstance()
assert root is not None

split = None
if args.mode != "serial":
    from gem5.prebuilt.cachelens.split_partition import attach_chi

    split = attach_chi(
        root,
        board,
        args.mode,
        args.layout,
        quantum=args.quantum,
        spin_wait=args.spin_wait,
    )
    root.split_hnf.exercise_io = args.exercise_io
    (outdir / "split.json").write_text(json.dumps(split, indent=2) + "\n")
    print(
        f"CACHELENS_SPLIT_LAYOUT layout={args.layout} "
        f"queues={split['queue_count']} cut_links={len(split['links'])} "
        f"cut_buffers={split['cut_buffers']}"
    )

m5.instantiate()
board._post_instantiate()
if split:
    root.split_hnf.activate()
if args.deactivate_at:
    assert args.exercise_io and 1_001_000 <= args.deactivate_at < args.ticks
    early = m5.simulate(args.deactivate_at)
    assert early.getCause() == "simulate() limit reached"
    root.split_hnf.deactivate()
event = m5.simulate(args.ticks - m5.curTick())
cause = event.getCause()
if split:
    root.split_hnf.deactivate()
m5.stats.dump()
tick = m5.curTick()
if cause != "simulate() limit reached" or tick != args.ticks:
    print(f"CACHELENS_SPLIT_FAIL mode={args.mode} cause={cause!r} tick={tick}")
    sys.exit(1)

# The guest must have finished and produced HNF traffic and snoops.
stats = (outdir / "stats.txt").read_text()
hnf = r"^board\.cache_hierarchy\.hnfs\d+"
requests = stat_sum(stats, hnf + r"\.reqIn\.m_msg_count\s+(\S+)")
snoops = stat_sum(stats, hnf + r"\.snpOut\.m_msg_count\s+(\S+)")
parked = len(
    [
        count
        for count in re.findall(
            r"^board\.processor\.cores\d+\.core\.thread_0\.numInsts\s+(\d+)",
            stats,
            re.M,
        )
        if int(count) == committed
    ]
)
if parked != args.cores:
    print(
        f"CACHELENS_SPLIT_FAIL mode={args.mode} {parked}/{args.cores} cores "
        f"committed the {committed} instructions to WFI"
    )
    sys.exit(1)
if requests < 5000 or snoops < 1000:
    print(
        f"CACHELENS_SPLIT_FAIL mode={args.mode} too little HNF traffic "
        f"requests={requests} snoops={snoops}"
    )
    sys.exit(1)
print(
    f"CACHELENS_SPLIT_OK mode={args.mode} layout={args.layout} tick={tick} "
    f"hnf_requests={requests} hnf_snoops={snoops} parked={parked}"
)
