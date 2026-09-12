# Copyright (c) 2026 The Regents of the University of California
# All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause

"""Save or restore a small CacheLens/Ruby data-correctness checkpoint."""

import argparse
from pathlib import Path

import m5

from gem5.coherence_protocol import CoherenceProtocol
from gem5.components.boards.simple_board import SimpleBoard
from gem5.components.cachehierarchies.chi.private_l1_cache_hierarchy import (
    PrivateL1CacheHierarchy,
)
from gem5.components.cachehierarchies.classic.no_cache import NoCache
from gem5.components.memory import SingleChannelDDR3_1600
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.isas import ISA
from gem5.prebuilt.cachelens.cache_hierarchy import CacheLensCHIHierarchy
from gem5.resources.resource import BinaryResource
from gem5.simulate.exit_event import ExitEvent
from gem5.simulate.simulator import Simulator
from gem5.utils.requires import requires

parser = argparse.ArgumentParser()
parser.add_argument("--isa", choices=("arm", "x86"), required=True)
parser.add_argument("--phase", choices=("save", "restore"), required=True)
parser.add_argument(
    "--hierarchy", choices=("cachelens", "generic", "none"), required=True
)
parser.add_argument(
    "--cpu-type", choices=("atomic", "timing"), required=True
)
parser.add_argument("--binary", type=Path, required=True)
parser.add_argument("--checkpoint", type=Path, required=True)
parser.add_argument("--max-ticks", type=int, default=m5.MaxTick)
args = parser.parse_args()

isa = ISA.ARM if args.isa == "arm" else ISA.X86
requires(
    isa_required=isa,
    coherence_protocol_required=CoherenceProtocol.CHI,
)

if args.hierarchy == "cachelens":
    cache_hierarchy = CacheLensCHIHierarchy(
        l1i_size="16KiB",
        l1d_size="16KiB",
        l2_size="32KiB",
        hnf_size="64KiB",
        hnf_assoc=4,
        model_profile=(
            "arm-generic" if isa == ISA.ARM else "x86-generic"
        ),
    )
elif args.hierarchy == "generic":
    cache_hierarchy = PrivateL1CacheHierarchy(size="32KiB", assoc=4)
else:
    cache_hierarchy = NoCache()

cpu_type = (
    CPUTypes.ATOMIC if args.cpu_type == "atomic" else CPUTypes.TIMING
)
processor = SimpleProcessor(cpu_type=cpu_type, isa=isa, num_cores=1)
board = SimpleBoard(
    clk_freq="2GHz",
    processor=processor,
    memory=SingleChannelDDR3_1600(size="64MiB"),
    cache_hierarchy=cache_hierarchy,
)
board.set_se_binary_workload(
    binary=BinaryResource(args.binary.as_posix(), architecture=isa),
    checkpoint=args.checkpoint if args.phase == "restore" else None,
)


def save_checkpoint():
    args.checkpoint.mkdir(parents=True, exist_ok=True)
    m5.checkpoint(args.checkpoint.as_posix())
    print(
        "CACHELENS_CHECKPOINT_SAVED "
        f"isa={args.isa} cpu={args.cpu_type} hierarchy={args.hierarchy}"
    )
    yield True


simulator = Simulator(
    board=board,
    on_exit_event=(
        {ExitEvent.CHECKPOINT: save_checkpoint()}
        if args.phase == "save"
        else None
    ),
    max_ticks=args.max_ticks,
)
simulator.run()

if args.phase == "restore":
    print(
        "CACHELENS_CHECKPOINT_RESTORED "
        f"isa={args.isa} cpu={args.cpu_type} hierarchy={args.hierarchy}"
    )
