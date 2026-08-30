# SPDX-License-Identifier: BSD-3-Clause

import argparse
from pathlib import Path
import re

import m5
from m5.objects import (
    AddrRange,
    EtherLink,
    GenericPciHost,
    Pvrdma,
    PvrdmaTester,
    PvrdmaTestLink,
    Root,
    SimpleMemory,
    SrcClockDomain,
    System,
    SystemXBar,
    VoltageDomain,
)

parser = argparse.ArgumentParser()
parser.add_argument(
    "--mode",
    choices=(
        "timing-mr",
        "timing-queues",
        "timing-observation",
        "timing-completion",
        "transport-pair",
        "timing-transport-pair",
        "reliability-pair",
        "timing-reliability-pair",
        "reliability-rnr-pair",
        "timing-reliability-rnr-pair",
        "reliability-timeout-zero-pair",
        "timing-reliability-timeout-zero-pair",
        "reliability-invalid-pair",
        "timing-reliability-invalid-pair",
        "reliability-unrelated-pair",
        "timing-reliability-unrelated-pair",
        "reliability-cq-pair",
        "timing-reliability-cq-pair",
        "reliability-cq-abort-pair",
        "timing-reliability-cq-abort-pair",
        "timing-reliability-precommit-abort-pair",
        "timing-reliability-commit-pair",
        "timing-reliability-commit-boundary-pair",
        "fault-link",
        "timing-fault-link",
        "completion",
        "completion-errors",
        "stats-reset",
        "checkpoint-save",
        "checkpoint-restore",
        "checkpoint-observation-save",
        "checkpoint-observation-restore",
        "checkpoint-completion-save",
        "checkpoint-completion-restore",
    ),
    required=True,
)
parser.add_argument("--checkpoint", type=Path)
args = parser.parse_args()
if args.mode.startswith("checkpoint-") and args.checkpoint is None:
    parser.error("checkpoint modes require --checkpoint")

system = System(
    mem_mode="timing" if args.mode.startswith("timing-") else "atomic",
    mem_ranges=[AddrRange("64MiB")],
)
system.voltage_domain = VoltageDomain()
system.clk_domain = SrcClockDomain(
    clock="1GHz", voltage_domain=system.voltage_domain
)
system.bus = SystemXBar()
system.memory = SimpleMemory(range=system.mem_ranges[0], latency="5us")
system.tester = PvrdmaTester(test_mode=args.mode)
system.pci_host = GenericPciHost(
    platform=system.tester,
    conf_base=0x20000000,
    conf_size="64KiB",
    conf_device_bits=8,
)
system.rdma = Pvrdma(
    host=system.pci_host,
    pci_bus=0,
    pci_dev=2,
    pci_func=1,
    InterruptLine=18,
    InterruptPin=2,
    hardware_address="02:00:00:00:00:01",
)
if args.mode.endswith("transport-pair") or "reliability" in args.mode:
    system.peer_rdma = Pvrdma(
        host=system.pci_host,
        pci_bus=0,
        pci_dev=3,
        pci_func=1,
        InterruptLine=19,
        InterruptPin=2,
        hardware_address="02:00:00:00:00:02",
    )
    system.rdma_link = (
        PvrdmaTestLink()
        if "reliability" in args.mode
        else EtherLink(speed="10Gbps", delay="10ns", delay_var="0ns")
    )
    system.rdma.interface = system.rdma_link.int0
    system.peer_rdma.interface = system.rdma_link.int1
elif args.mode.endswith("fault-link"):
    system.rdma_link = PvrdmaTestLink()
    system.tester.fault0 = system.rdma_link.int0
    system.tester.fault1 = system.rdma_link.int1

system.system_port = system.bus.cpu_side_ports
system.memory.port = system.bus.mem_side_ports
system.tester.port = system.bus.cpu_side_ports
system.pci_host.pio = system.bus.mem_side_ports
system.rdma.pio = system.bus.mem_side_ports
system.rdma.dma = system.bus.cpu_side_ports
if hasattr(system, "peer_rdma"):
    system.peer_rdma.pio = system.bus.mem_side_ports
    system.peer_rdma.dma = system.bus.cpu_side_ports

root = Root(full_system=False, system=system)
m5.instantiate(
    args.checkpoint.as_posix() if args.mode.endswith("-restore") else None
)
exit_event = m5.simulate()

if args.mode in ("transport-pair", "timing-transport-pair"):
    assert exit_event.getCause() == "PVRDMA transport pair test passed"
    print("PVRDMA_TRANSPORT_PAIR_OK")
elif args.mode in ("reliability-pair", "timing-reliability-pair"):
    assert exit_event.getCause() == "PVRDMA reliability pair test passed"
    print("PVRDMA_RELIABILITY_PAIR_OK")
elif args.mode in ("reliability-rnr-pair", "timing-reliability-rnr-pair"):
    assert exit_event.getCause() == "PVRDMA reliability RNR pair test passed"
    print("PVRDMA_RELIABILITY_RNR_PAIR_OK")
elif args.mode in (
    "reliability-timeout-zero-pair",
    "timing-reliability-timeout-zero-pair",
):
    assert exit_event.getCause() == (
        "PVRDMA reliability timeout-zero pair test passed"
    )
    print("PVRDMA_RELIABILITY_TIMEOUT_ZERO_PAIR_OK")
elif args.mode in (
    "reliability-invalid-pair",
    "timing-reliability-invalid-pair",
):
    assert exit_event.getCause() == (
        "PVRDMA reliability invalid pair test passed"
    )
    print("PVRDMA_RELIABILITY_INVALID_PAIR_OK")
elif args.mode in (
    "reliability-unrelated-pair",
    "timing-reliability-unrelated-pair",
):
    assert exit_event.getCause() == (
        "PVRDMA reliability unrelated pair test passed"
    )
    print("PVRDMA_RELIABILITY_UNRELATED_PAIR_OK")
elif args.mode in ("reliability-cq-pair", "timing-reliability-cq-pair"):
    assert exit_event.getCause() == "PVRDMA reliability CQ pair test passed"
    print("PVRDMA_RELIABILITY_CQ_PAIR_OK")
elif args.mode in (
    "reliability-cq-abort-pair",
    "timing-reliability-cq-abort-pair",
):
    assert exit_event.getCause() == (
        "PVRDMA reliability CQ abort pair test passed"
    )
    print("PVRDMA_RELIABILITY_CQ_ABORT_PAIR_OK")
elif args.mode == "timing-reliability-precommit-abort-pair":
    assert exit_event.getCause() == (
        "PVRDMA reliability precommit CQ abort test passed"
    )
    print("PVRDMA_RELIABILITY_PRECOMMIT_ABORT_PAIR_OK")
elif args.mode == "timing-reliability-commit-pair":
    assert exit_event.getCause() == (
        "PVRDMA reliability committed receive test passed"
    )
    print("PVRDMA_RELIABILITY_COMMIT_PAIR_OK")
elif args.mode == "timing-reliability-commit-boundary-pair":
    assert exit_event.getCause() == (
        "PVRDMA reliability receive commit-boundary test passed"
    )
    print("PVRDMA_RELIABILITY_COMMIT_BOUNDARY_PAIR_OK")
elif args.mode in ("fault-link", "timing-fault-link"):
    assert exit_event.getCause() == "PVRDMA fault-link test passed"
    print("PVRDMA_FAULT_LINK_OK")
elif args.mode == "timing-mr":
    assert exit_event.getCause() == "PVRDMA timing MR test passed"
    print("PVRDMA_TIMING_MR_OK")
elif args.mode == "timing-queues":
    assert exit_event.getCause() == "PVRDMA timing queue test passed"
    print("PVRDMA_TIMING_QUEUES_OK")
elif args.mode == "timing-observation":
    assert exit_event.getCause() == "PVRDMA timing observation test passed"
    print("PVRDMA_TIMING_OBSERVATION_OK")
elif args.mode == "timing-completion":
    assert exit_event.getCause() == "PVRDMA timing completion test passed"
    print("PVRDMA_TIMING_COMPLETION_OK")
elif args.mode == "completion":
    assert exit_event.getCause() == "PVRDMA completion publication test passed"
    print("PVRDMA_COMPLETION_OK")
elif args.mode == "completion-errors":
    assert exit_event.getCause() == "PVRDMA completion error test passed"
    print("PVRDMA_COMPLETION_ERRORS_OK")
elif args.mode == "stats-reset":
    assert exit_event.getCause() == "PVRDMA queue statistics test passed"
    m5.stats.dump()
    stats = (Path(m5.options.outdir) / "stats.txt").read_text()

    def stat(name):
        match = re.search(rf"^system\.rdma\.queues\.{name}\s+(\S+)",
                          stats, re.MULTILINE)
        assert match, name
        return float(match.group(1))

    assert abs(stat("sqOccupancy::mean") - 3.7499) < 0.00001
    assert stat("sqOccupancy::max_value") == 5
    assert abs(stat("rqOccupancy::mean") - 6.24965) < 0.00001
    assert stat("rqOccupancy::max_value") == 8
    print("PVRDMA_STATS_RESET_OK")
elif args.mode == "checkpoint-save":
    assert exit_event.getCause() == "PVRDMA checkpoint save ready"
    args.checkpoint.mkdir(parents=True, exist_ok=True)
    m5.checkpoint(args.checkpoint.as_posix())
    print("PVRDMA_CHECKPOINT_SAVED")
elif args.mode == "checkpoint-restore":
    assert exit_event.getCause() == "PVRDMA checkpoint restore test passed"
    print("PVRDMA_CHECKPOINT_RESTORED")
elif args.mode == "checkpoint-completion-save":
    assert exit_event.getCause() == "PVRDMA completion checkpoint save ready"
    args.checkpoint.mkdir(parents=True, exist_ok=True)
    m5.checkpoint(args.checkpoint.as_posix())
    print("PVRDMA_COMPLETION_CHECKPOINT_SAVED")
elif args.mode == "checkpoint-completion-restore":
    assert exit_event.getCause() == "PVRDMA completion checkpoint restored"
    print("PVRDMA_COMPLETION_CHECKPOINT_RESTORED")
elif args.mode == "checkpoint-observation-save":
    assert exit_event.getCause() == "PVRDMA observation checkpoint save ready"
    args.checkpoint.mkdir(parents=True, exist_ok=True)
    m5.checkpoint(args.checkpoint.as_posix())
    print("PVRDMA_OBSERVATION_CHECKPOINT_SAVED")
else:
    assert exit_event.getCause() == "PVRDMA observation checkpoint restored"
    print("PVRDMA_OBSERVATION_CHECKPOINT_RESTORED")
