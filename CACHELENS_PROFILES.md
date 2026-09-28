# CacheLens architecture profiles

Two explicit simulator configurations are provided for cross-architecture
experiments. They are synthetic, parameterized systems, not calibrated models
of commercial processors. Supporting both does not establish model accuracy;
that requires independent workload measurements.

| Profile | Physical cache levels | Coherence and I/O policy |
|---|---|---|
| x86 + `mesi-three-level` | private L1I/L1D → private L2 → inclusive shared L3/LLC | Directory MESI; optional experimental Intel-style DDIO allocation at L3 |
| ARM + `chi` | private L1I/L1D → private L2 → shared HNF/LLC | CHI; explicit inclusive/noninclusive HNF; optional abstract coherent-I/O allocation |

Existing x86 CHI and MESI two-level configurations remain available for
regression comparisons. ARM MESI and ARM PVRDMA attachment are not supported
by the CacheLens runner.

## Build

From the gem5 source root with the normal gem5 build dependencies installed:

```sh
scons defconfig build/X86_MESI_Three_Level build_opts/X86_MESI_Three_Level
scons build/X86_MESI_Three_Level/gem5.opt --without-tcmalloc -j16
scons build/ARM_CHI/gem5.opt --without-tcmalloc -j16
```

`build_opts/ARM_CHI` explicitly names the existing ARM/CHI protocol combination.
Use each binary with its matching ISA/protocol. These targets do not by
themselves rebuild the outer CacheLens guest images or packaged artifacts.

## Primary DDIO smoke (no guest resources)

From this source root, run the **native x86/MESI_Three_Level** binary:

```sh
python3 verification/ddio-smoke.py \
  --gem5 build/X86_MESI_Three_Level/gem5.opt \
  --output build/ddio-smoke-local
```

The output directory must not exist; omit `--output` for a fresh temporary
location. All 12 cases run by default, with a 120-second wall timeout per case
(`--timeout` overrides it). Each retains its command, elapsed time, combined
log, configuration and statistics; `summary.json` records pass/fail. Failure,
timeout, missing completion, topology mismatch or an incorrect exact counter
fails the command. The fixture requires x86 and MESI_Three_Level: NULL and
MESI_Two_Level are not substitutes.

Traffic uses two synthetic CPU data ports through private L1 and L2 to the
shared L3, plus opted-in and generic DMA ports and real SimpleMemory. Cases
cover full/partial writes, dirty owners and sharers, inclusive replacement,
no-allocation reads, sparse/zero masks, two-bank routing, and DDIO disabled
(`--ddio-way-part=-1`). C++ byte/readback assertions remain enabled. Exact
three-level expectations are shared with testlib in
`tests/gem5/mesi_ddio/three_level_checks.py`; existing MESI_Two_Level tests are
supplementary regressions, not the primary profile smoke. See
[validation evidence](verification/no-tags/README.md#native-x86-three-level-primary-smoke)
for the measured native run and persisted binary.

## Configure

The common runner is `configs/example/gem5_library/cachelens-fs.py`. Supply
compatible local kernels, disks, and (for ARM) boot firmware; none are downloaded
by these examples.

```sh
build/X86_MESI_Three_Level/gem5.opt \
  configs/example/gem5_library/cachelens-fs.py \
  --isa x86 --coherence-protocol mesi-three-level \
  --kernel /path/to/x86/vmlinux --disk-image /path/to/x86/rootfs.ext2 \
  --model-profile intel-ddio --ddio-way-part 2 \
  --l1d-size 64KiB --l2-size 1MiB --hnf-size 1MiB \
  --hnf-inclusion inclusive

build/ARM_CHI/gem5.opt \
  configs/example/gem5_library/cachelens-fs.py \
  --isa arm --coherence-protocol chi \
  --kernel /path/to/arm/vmlinux --disk-image /path/to/arm/rootfs.ext2 \
  --bootloader /path/to/arm/boot.arm64 \
  --model-profile abstract --ddio-way-part 2 \
  --l1d-size 64KiB --l2-size 1MiB --hnf-size 1MiB \
  --hnf-inclusion inclusive
```

The ARM example deliberately requests an abstract I/O allocation policy; it
is not Intel DDIO or a claim about a particular ARM SoC. `arm-generic` and
`x86-generic` disable DDIO by default and reject positive DDIO partitions.
`intel-ddio` is x86-only. `abstract` permits explicit experimental allocation.

For specification-based Arm **HNF write stashing**, instead use
`--model-profile arm-generic --cache-stashing hnf --ddio-way-part -1`.
This mode enables HNF acceptance of explicit CHI write-stash opcodes using
ordinary LLC ways, not a DDIO partition. Native NIC DMA does not emit hints. See [the supported subset and primary
specification references](CACHELENS_STASHING.md); private-target and independent
stashing are not implemented.

For both three-level profiles, `l1d_*` controls private L1D, `l2_*` controls
private L2, and `hnf_*` controls each shared LLC bank. `num_hnfs` is the number
of banks, so total shared capacity is per-bank size times bank count. The old
MESI two-level profile retains its historical, different parameter mapping.

Three-level MESI backend names are one level lower: Ruby `L0Cache`, `L1Cache`,
and `L2Cache` correspond to physical L1, L2, and L3. Its shared controller
reuses the fork's MESI DMA routing, partial writes, ownership, replacement,
without payload/descriptor attribution. Eligible coherent DMA goes to the shared L3,
not a private L2. Shared-LLC evictions invalidate private copies through both
private levels. CPU caches do not receive the DDIO way partition.

LLC indexing can be `linear` or experimental `splitmix64`; bank selection
remains explicit low address bits. Eligible DMA reads (including descriptors and untagged reads) use coherent
no-allocation behavior in both MESI and CHI. Matching capacities does not imply that
CHI and MESI have identical transaction latency or replacement behavior.

## Validation boundaries

- Build and protocol tests are distinct from full guest boot and application
  validation. ARM guest/DPDK packaging and full workload accuracy runs are
  separate work in the outer CacheLens repository.
- New profile tests use local object graphs and directed CPU/DMA traffic,
  without downloading guest resources. ARM multicore probes check DVM and
  coherent IDE/E1000 wiring; CHI TLM suites exercise protocol data paths.
- Use fresh checkpoints for the new source/profile. Atomic preparation uses
  NoCache; timing/O3 restore intentionally starts with cold caches. Do not
  claim warm cache-state continuity across restores.
- Ruby exposes aggregate cache/DDIO counters only. Descriptor/payload DMA
  operation and byte counts are device-local; no source hit-rate is measured.
- Native CHI RubySequencer traffic supports contiguous full/partial writes,
  not byte-enabled sparse/zero-mask writes. Unsupported masked writes are
  rejected before coherence state changes. MESI DMASequencer supports masks;
  its sparse/zero-mask scenarios are tested. CHI TLM mask tests use a separate
  external interface and require the optional `libarmtlmchi.so` dependency.
- Do not claim ARM RDMA support from the presence of a generic PVRDMA device
  model. The CacheLens full-system attachment remains explicitly x86-only.
- Freeze physical/model inputs before scoring accuracy; do not fit separate
  architecture-specific coefficients to the evaluation hit rates.

## Explicit DMA eligibility

DDIO is a requestor policy, not a NIC semantic-tag policy. The default-false
`ddio_eligible` controller parameter is carried in Ruby requests (including CHI
forwarding and retries). Eligible full/partial writes use the DDIO way subset;
`ddio_way_part=-1` instead uses coherent write-through/no-retention. Eligible
reads never allocate on misses and may use an existing LLC line on hits.
All eligible DMA follows the same path. Requests carry no descriptor/payload
or RDMA classification. `ddioWayAccess` and `ddioWayFill` are one entry per
physical way (`wayN`), counting eligible DMA only. `ddioAllocWays` counts every
eligible DMA fill within the configured subset, including descriptor writes.
`wayDeallocations` still counts all cache-entry deallocations. No CPU/source
occupancy, payload-only hit rates, or provenance history is retained.
Noneligible traffic keeps the generic protocol path.

CacheLens ARM/x86 boards expose `get_ddio_dma_ports()`: NIC and RDMA ports,
never IDE. Both stdlib MESI hierarchies and CacheLens CHI consume this opt-in;
boards without the method select no ports. The maintained SimBricks MESI
`AccuracyBoard` (outer `gem5/experiments/i40e-rxonly/board.py`) inherits
`CacheLensX86Board`, populates `nics` with its `SimBricksPci`, and uses
`CacheLensMESIThreeLevelHierarchy`; it therefore selects only that NIC port,
without any semantic DMA wire metadata. ARM CacheLens subclasses inherit the same
network-only selection. Legacy MESI/CHI configs accept `--ddio-dma-ports` with
explicit zero-based DMA port indices (default empty); legacy three-level MESI
DDIO requires one shared LLC cluster.

CHI's public `nic_read_no_allocate` parameter remains accepted for configuration
compatibility. It neither enables DDIO nor overrides eligibility: eligible
reads always no-allocate, noneligible reads use generic CHI policy. Historical
profile metadata still reports that knob. Accepted write-stash hints retain
their independent ordinary-way allocation behavior. Automatic payload-only
stash emission and its parameter were removed, not broadened to all writes.

For synthetic legacy MemTest traffic use normal DMA and explicit policy:
`--num-dmas=1 --ddio-dma-ports 0`. The old tagging option is removed.

## Device DMA counters

E1000 reuses `descDmaReads/Writes/RdBytes/WrBytes` in EtherDeviceStats and adds
`rxq.payloadDmaReads/Writes/RdBytes/WrBytes` (RX split headers included as packet
bytes). PVRDMA exposes the same eight names under `dma`; descriptor counters
include queue, completion and control DMA. Each operation/byte count is updated
at issuance, before DMA-port cache-line fragmentation; it is neither a packet
count nor a cache hit measurement. SimBricks component-side counters live in
the producer, not the gem5 PCI adapter.
