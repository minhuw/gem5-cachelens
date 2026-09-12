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
scons build/X86_MESI_Three_Level/gem5.opt --without-tcmalloc -j16
scons build/ARM_CHI/gem5.opt --without-tcmalloc -j16
```

`build_opts/ARM_CHI` explicitly names the existing ARM/CHI protocol combination.
Use each binary with its matching ISA/protocol. These targets do not by
themselves rebuild the outer CacheLens guest images or packaged artifacts.

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
This opt-in mode emits no-target CHI write-stash hints for RX payloads and uses
ordinary LLC ways, not a DDIO partition. See [the supported subset and primary
specification references](CACHELENS_STASHING.md); private-target and independent
stashing are not implemented.

For both three-level profiles, `l1d_*` controls private L1D, `l2_*` controls
private L2, and `hnf_*` controls each shared LLC bank. `num_hnfs` is the number
of banks, so total shared capacity is per-bank size times bank count. The old
MESI two-level profile retains its historical, different parameter mapping.

Three-level MESI backend names are one level lower: Ruby `L0Cache`, `L1Cache`,
and `L2Cache` correspond to physical L1, L2, and L3. Its shared controller
reuses the fork's MESI DMA routing, partial writes, ownership, replacement,
and PVRDMA payload attribution. Classified payload DMA goes to the shared L3,
not a private L2. Shared-LLC evictions invalidate private copies through both
private levels. CPU caches do not receive the DDIO way partition.

LLC indexing can be `linear` or experimental `splitmix64`; bank selection
remains explicit low address bits. MESI classified TX payload reads use the
existing fixed no-allocation behavior. Matching capacities does not imply that
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
- Generic source/write-extent vectors remain CHI-only. The MESI paths provide
  their existing payload/provenance counters, not equivalent extent coverage.
- Native CHI RubySequencer traffic supports contiguous full/partial writes,
  not byte-enabled sparse/zero-mask writes. Unsupported masked writes are
  rejected before coherence state changes. MESI DMASequencer supports masks;
  its sparse/zero-mask scenarios are tested. CHI TLM mask tests use a separate
  external interface and require the optional `libarmtlmchi.so` dependency.
- Do not claim ARM RDMA support from the presence of a generic PVRDMA device
  model. The CacheLens full-system attachment remains explicitly x86-only.
- Freeze physical/model inputs before scoring accuracy; do not fit separate
  architecture-specific coefficients to the evaluation hit rates.
