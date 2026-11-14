# Native ARM PVRDMA PCI attachment and shared INTx

This slice enables source-level ARM attachment of the same native Pvrdma
PciDevice/EtherInt used on x86. It does not supply a guest kernel/provider
port or claim Linux/GIC/remote-verbs runtime readiness.

## Attachment and address contract

`CacheLensArmBoard(... network_options={"num_nics": 1,
"enable_pvrdma": True, ...})` attaches IDE at 00:01.0, native E1000 at
00:02.0 and PVRDMA at 00:02.1. Function0 advertises multifunction header bit
0x80. PVRDMA is appended to ArmBoard's PCI ownership list without allocating
a new slot. Its BARs retain 16KiB/256B/2MiB sizes. The VExpress host retains
ECAM function decoding (12 offset bits), PCI memory bus base0x40000000,
DMA base0 and its existing ARM_PCI_INT_DEV routing. Thus both functions of
slot2 map to SPI102, consistent with the existing DT interrupt-map mask.

`--enable-pvrdma` is accepted by cachelens-fs.py for ARM and x86 and still
requires exactly one E1000 companion. Native RDMA payload EtherInt peer
attachment must be supplied separately; this single-board configuration
cannot create a remote endpoint automatically.

On classic NoCache, PVRDMA DMA connects to the existing I/O/DMA bridge path.
On CHI it is added as a distinct DMA port/requestor, independently from E1000
and IDE. `get_ddio_dma_ports` marks E1000 and PVRDMA as eligible network
requestors; IDE and other platform requestors are excluded. Eligibility
includes control/queue metadata and is not a payload byte counter. The
arm-generic graph check keeps stashing off and ddio_way_part=-1.
The PCI host DT already advertises dma-coherent. No private DMA address
translation or IOMMU bypass is added.

Both intended CPU targets are little endian. Public PCI/MMIO/DSR/WQE/SGE/CQE
fields retain explicit little-endian encoding and 64-bit DMA addresses; RoCE
wire headers retain network order. Guest release/acquire/device barriers
remain a separate provider prerequisite; host attachment does not insert
validation DMA or CPU receive-payload accesses.

## Shared INTx behavior

`GenericPciHost.aggregate_intx` defaults False. Existing x86 configurations
and PVRDMA-disabled ARM keep their old routing/behavior. ARM PVRDMA explicitly
enables it. The host tracks asserted sources by BDF AND interrupt pin, caching
each source's mapped platform line. Only the first source asserts the line;
only clearing the last source deasserts it. Repeated posts from a still-
asserted source do not add references. Clearing a nonasserted source has no
effect. A route change during assertion fails rather than stranding a line.

Aggregation-enabled checkpoints serialize the source-to-line map and mode.
Restoration validates source shape/registration and validates routes after
all device configuration state is restored. A checkpoint aggregation-mode
change is rejected. The platform restores its own interrupt-controller input
state; the host does not synthesize additional interrupt edges on restore.
Aggregation-disabled old checkpoints have no new required fields.

## Focused checks

Build a dedicated ARM binary with USE_TEST_OBJECTS=y (the existing frozen
ARM build omits these objects), using at most j2. For example:

```
scons setconfig build/ARM_CHI_SIMBRICKS USE_TEST_OBJECTS=y
scons build/ARM_CHI_SIMBRICKS/gem5.fast -j2 --ignore-style
python3 tests/gem5/rdma/run_pci_tests.py \
  --gem5 build/ARM_CHI_SIMBRICKS/gem5.fast --host arm --outdir <dedicated-output>
```

`pvrdma_pci.py` attaches real E1000/PVRDMA functions to GenericArmPciHost.
The tester drives public ECAM/MMIO only, checks distinct vendor/device IDs,
missing function2, multifunction advertisement, PVRDMA BAR sizing, translated
BAR accesses, and DSR/command/response DMA. Its nonzero synthetic DMA base
0x80000000 ensures bus addresses translate exactly once (the board's actual
host base is0). IRQs come from real E1000 ICS/IMS writes and PVRDMA command
response completion; the test checks both source-clear orders and repeated
E1000 posts. The disabled-aggregation case must reproduce the original shared
line loss. Additional cases save/restore with BOTH functions asserted and
reject checkpoint mode changes.

The tester records platform input line102 directly. This is native
host/device interrupt behavior, NOT GIC delivery to an ARM guest CPU or Linux
interrupt-handler execution. PCI graph checks in cachelens_board_probe.py
cover classic/CHI, distinct coherent sequencers/eligibility, disabled ARM,
legacy x86, and invalid companion counts without booting/instantiating a
Linux guest. Existing public RC pair coverage remains device completion proof.

Exact executed commands, results, source archive, build configuration,
binary/source hashes, and read-only patched-kernel evidence are retained in
`.pi/artifacts/arm-platform/` in the implementation worktree. See
GUEST_ARM_HANDOFF.md for guest ownership, actual patched companion ABI,
provider ordering, disabled userspace neighbor resolution, and static normal
network setup requirements. Parent pins/frozen binaries are deliberately
unchanged; a coordinated guest runtime gate and later pin update are required.
