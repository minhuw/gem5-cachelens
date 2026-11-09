# Copyright (c) 2026; SPDX-License-Identifier: BSD-3-Clause
"""Directed four-O3 ARM wire timing guest. Rebuild its image with
build_arm_split_timing.py (host aarch64-linux-gnu toolchain). At run time this
uses the committed sparse word image and checks its source digest.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

import m5
from m5.objects import (
    ArmFsWorkload, ArmPartitionTimingProbe, ArmPMU, ArmPPI, IdeController, Root,
)
from gem5.components.memory import DualChannelDDR4_2400
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.isas import ISA
from gem5.prebuilt.cachelens.board import CacheLensArmBoard
from gem5.prebuilt.cachelens.cache_hierarchy import CacheLensCHIHierarchy
from gem5.prebuilt.cachelens.split_partition import attach_chi

sys.path.insert(0, str(Path(__file__).resolve().parent))
from arm_split_timing_image import SIZE, SOURCE_SHA256, WORDS

parser = argparse.ArgumentParser()
parser.add_argument('--mode', choices=('native', 'fenced', 'thread'), required=True)
parser.add_argument('--partition-timing', action='store_true')
parser.add_argument('--layout', choices=('hnf1', 'hnf2', 'cpu3-control-hnf1', 'cpu3-control-hnf2'), default='hnf1')
parser.add_argument('--ticks', type=int, default=100_000_000)
parser.add_argument('--deactivate-at', type=int, default=0)
parser.add_argument('--quantum', type=int, default=1000)
parser.add_argument('--probe', action='store_true', help='exact-tick checks after the guest parks')
args = parser.parse_args()
out = Path(m5.options.outdir)
source = Path(__file__).with_suffix('.S')
assert hashlib.sha256(source.read_bytes()).hexdigest() == SOURCE_SHA256, 'rebuild guest image'
payload = bytearray(SIZE)
for offset, word in WORDS.items():
    struct.pack_into('<I', payload, offset, word)
header = b'\x7fELF' + bytes((2, 1, 1)) + bytes(9)
header += struct.pack('<HHIQQQIHHHHHH', 2, 183, 1, 0, 64, 0, 0, 64, 56, 1, 0, 0, 0)
program = struct.pack('<IIQQQQQQ', 1, 7, 0x1000, 0, 0, SIZE, SIZE, 0x1000)
elf = out / 'arm-split-timing.elf'
elf.write_bytes((header + program).ljust(0x1000, b'\0') + payload)

hierarchy = CacheLensCHIHierarchy(l1i_size='8KiB', l1d_size='8KiB', l2_size='32KiB',
                                  hnf_size='32KiB', num_hnfs=4, core_clock='2GHz')
board = CacheLensArmBoard(clk_freq='1GHz',
    processor=SimpleProcessor(cpu_type=CPUTypes.O3, isa=ISA.ARM, num_cores=4),
    memory=DualChannelDDR4_2400(size='1GiB'), cache_hierarchy=hierarchy)
board.partition_timing = args.partition_timing
board.partition_wire_latency = '1ns'
board._set_fullsystem(True)
board._bootloader = []
board._add_pci_device(IdeController(disks=[]))
board.workload = ArmFsWorkload(object_file=str(elf))
board._pre_instantiate()
board.realview.sys_counter.freqs = [2_000_000_000]
board.realview.generic_timer.cntfrq = 2_000_000_000
for core in board.get_processor().get_cores():
    cpu = core.get_simobject()
    cpu.isa[0].pmu = ArmPMU(interrupt=ArmPPI(num=23, platform=board.realview),
                           use64bitCounters=False)
    cpu.isa[0].pmu.addArchEvents(cpu=cpu, itb=cpu.mmu.itb, dtb=cpu.mmu.dtb)
root = Root.getInstance()
if args.probe:
    assert args.partition_timing and args.ticks >= 81_000_000
    board.timing_probe = ArmPartitionTimingProbe(
        system=board, gic=board.realview.gic, timer=board.realview.generic_timer)
meta = attach_chi(root, board, args.mode, args.layout, quantum=args.quantum)
(out / 'split.json').write_text(json.dumps(meta, indent=2) + '\n')
m5.instantiate()
board._post_instantiate()
root.split_hnf.activate()
if args.deactivate_at:
    event = m5.simulate(args.deactivate_at)
    assert event.getCause() == 'simulate() limit reached'
    root.split_hnf.deactivate()
event = m5.simulate(args.ticks - m5.curTick())
root.split_hnf.deactivate()
assert event.getCause() == 'simulate() limit reached' and m5.curTick() == args.ticks
m5.stats.dump()
results = []
for cpu in range(4):
    data = bytes(root.split_hnf.readGuest(0x80200000 + cpu * 0x10000, 0x1000))
    fields = struct.unpack_from('<12Q', data)
    trace = [struct.unpack_from('<3Q', data, 0x100 + 24 * i) for i in range(min(fields[2], 100))]
    record = dict(cpu=cpu, status=fields[0], timer_irqs=fields[4], pmu_irqs=fields[5],
                  sgi_irqs=fields[6], stage=fields[7], esr=fields[10], elr=fields[11], trace=trace)
    results.append(record)
(out / 'guest.json').write_text(json.dumps(results, indent=2) + '\n')
for record in results:
    expected_sgi = int(record['cpu'] != 0)
    assert record['status'] == 0x600d, f'guest did not complete: {record}'
    assert (record['timer_irqs'], record['pmu_irqs'], record['sgi_irqs']) == (8, 1, expected_sgi), record
    codes = [entry[0] for entry in record['trace']]
    assert codes.count(0x11b) == 8 and codes.count(0x117) == 1, record
    assert all(c in (0x11b, 0x117, 0x101, 0x4ff, 20, 21, 30, 31, 40, 41) for c in codes), record
    assert codes.count(41) == 1 and record['trace'][-1][2] == 0x222, record
print(f'CACHELENS_ARM_TIMING_OK mode={args.mode} timing={int(args.partition_timing)} tick={m5.curTick()}')
