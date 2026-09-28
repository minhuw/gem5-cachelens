# Untagged DMA / device-local accounting verification

Development tree: `/home/minhuw/cachelens-dev/dma-ddio-gem5`.
The original untagged-DMA phase below used base `727f942d6e` and evidence in
`build/no-tags/`. Its boundaries are historical. The
[native x86 three-level follow-up](#native-x86-three-level-primary-smoke)
(2026-09-28, base `d7614d57f4`) closes the earlier native MESI_Three_Level gap;
its evidence is separately retained in `build/three-level-smoke/`.
No dependency-tree or outer-repository edits.

## Interface changes

- Request NIC category/provenance bits, masks, accessors and validation are gone.
  Native E1000/PVRDMA, synthetic testers and SimBricks issue ordinary requests.
- Explicit port/controller DDIO eligibility still controls coherent allocation,
  partial merges, no-allocation reads, subset replacement and ownership/retries.
- Retained cache statistics: `m_demand_hits`, `m_demand_misses`,
  `m_demand_accesses`, other ordinary cache/resource statistics,
  `dmaRoutingProxyRequests`, `dmaRoutingTransientRecycles`,
  `ddioReplacementStalls`, `ddioOwnershipRequests`, `ddioOwnershipAcks`, and
  the four `writeStash*` counters.
- `ddioWayAccess::wayN` and `ddioWayFill::wayN` now count **all eligible DMA**,
  with one entry per physical way, not source-by-way matrices.
  `ddioAllocWays::N` counts eligible fills inside the configured DDIO subset.
  All retain `::total`. `wayDeallocations::wayN` remains all-cause.
- Removed with **no source-specific cache replacement metric**: `rxPayload*`,
  `txPayload*`, `rxHeader*`, `rdmaRxPayload*`, `rdmaTxPayload*`, `nicDma*`,
  `nicDesc*`, semantic source rows, and remembered payload-address sets.
  Ordinary cache demand statistics are not payload/DMA-only hit rates.
- E1000 counts descriptors using EtherDeviceStats
  `descDmaReads/Writes/RdBytes/WrBytes`; its `rxq` group adds
  `payloadDmaReads/Writes/RdBytes/WrBytes`. Split headers count as packet bytes.
  PVRDMA exposes all eight under `dma` (descriptor includes control/queue DMA).
  Counters count device issuance, not packets or cache-line fragments.
- SimBricks adapter metadata decoding, statistics/CSV, semantic parameters and
  feature negotiation are removed. Ordinary DMA/range checks and initialized
  checkpoint control/state remain; only the removed telemetry sequence number
  is no longer serialized. Device-side SimBricks counters belong to the peer.
- Automatic payload-only stash emission and its parameter are removed.
  `cache_stashing="hnf"` enables **acceptance only** of explicit CHI opcodes.
  The no-op `nic_read_no_allocate` compatibility knob is retained, not policy.
  Legacy MemTest uses `--num-dmas` plus `--ddio-dma-ports`; no tagging option.

## Passed checks

Commands below run from the development tree. JSON result files record every
native regression command; per-case logs/configurations/statistics are retained.

| Check | Result / evidence |
| --- | --- |
| `python3 -m unittest discover -s tests/pyunit/mesi_ddio -p 'pyunit_*.py'` | 21 pass; `pyunit-final.log` |
| `python3 tests/pyunit/stdlib/pyunit_cachelens_mesi_three_level_hierarchy.py` | 5 pass; `three-level-pyunit.log` |
| `python3 verification/accuracy/test_config_contract.py` | pass; `config-contract.log` |
| Native CHI stdlib hierarchy tests | 12 run, 11 pass, ARM-core graph skipped; `chi-pyunit-final.log` |
| Native MESI stdlib hierarchy / network tests | 7 / 5 pass; `mesi-pyunit.log`, `network-pyunit.log` |
| SLICC MESI Two Level, Three Level, CHI | pass; `slicc-final.log` |
| Incremental NULL MESI / CHI builds | pass; `mesi_two_level-tmpfs.log`, `chi-tmpfs.log` |
| CHI build with `USE_TEST_OBJECTS=y` | pass; `test-objects-build.log` |
| `python3 build/no-tags/check_mesi.py` | 38/38 maintained registrations pass, including exact aggregate and protocol counters and removed-stat absence; `mesi-results.json` |
| `python3 build/no-tags/check_chi.py` | 27/27 cases pass: eligibility, merge/data, all inclusion policies, private-cache counters and no automatic stash; `chi-results.json` |
| Four-bank MESI directed routing | data and exact per-bank aggregate fill/access/ownership checks pass; `mesi-routing-four/`, `mesi-routing-four.log` |
| Generic and mixed eligible DMA MemTest | 100 DMA reads complete each; `mesi-generic.log`, `mesi-mixed.log` |
| `dma_sequencer.test.opt` | 10 pass: fragments, sparse masks, request-pointer/ordinary-flag transport and reservation overlap; `dma-unit.log` |
| `cache_memory.test.opt` | 11 pass: subset/replacement/tag handling, aggregate hooks and merge preparation; `cache-unit-final.log` |
| `python3 build/no-tags/check_pvrdma.py` | 5 modes pass; `pvrdma-results.json` |
| SimBricks PCI adapter object compile | pass against `/home/minhuw/cachelens-dev/no-tags-simbricks/lib`; `pci-object.log`, `.exit` |
| ARM/CHI TLM-enabled build | pass against installed Arm TLM 2.0, including HNF merge repair; `arm-tlm-config.log`, `arm-tlm-build.log`, `arm-tlm-merge-build.log`, `.exit` |
| CHI-TLM runtime registrations | **15/15 pass**, including accepted/declined sparse stash and ordinary noneligible sparse miss/hit; `tlm-results.json`, `tlm-check.log` |
| Additional CHI-TLM merge checks | **5/5 pass**: both stash suites with separate completion, plain sparse reproduction, dirty and clean/no-data owners; `tlm-merge-extra-results.json` |
| Native CHI after HNF merge repair, ARM binary | **27/27 pass**, all inclusion policies and data/counter checks; `chi-arm-merge-results.json`, `chi-arm-merge-check.log` |
| Python AST parsing / `git diff --check` | pass |

The PVRDMA observation test verifies flags are zero at memory and that a
129-byte transfer produces **one** device payload DMA read/write (129 bytes)
but **three** observed line fragments on each side. Both descriptor operation
and byte counters are checked. The E1000 issuance wrappers are compiled and
covered by source-level counter-placement checks, not an E1000 traffic run.

Cache unit tests now value-initialize their native parameter structs: Python
SimObject defaults do not initialize C++ fixtures. The old uninitialized bank
counts made test allocation nondeterministic. Provenance-only tests and their
checkpoint fixture were removed; ordinary cache tests were retained.

## Build/evidence boundaries

Root disk exhaustion interrupted initial builds. Only this task's regenerated
objects were moved to `/dev/shm`; `compiler-wrapper.py` subsequently redirected
new compiler/linker outputs there and left links in the existing build trees.
The wrapper changes output location, not compiler optimization/assertion flags.
All logs, result JSON, configs and stats are outside tmpfs. Final stripped
binaries and their hashes are retained in `build/no-tags/` as documented in
`artifacts.txt`. Rebuilding intermediate artifacts after a reboot is expected.
SCons dry-runs pass but request relinks/re-embedding after relocated output
signatures; this is not a claim that no work is scheduled.

Native NULL CHI tests use `build/chi_null_directed.py` for import-only placeholders
for ISA instruction-count annotations, never instantiated. The stdlib unittest
wrapper similarly does not instantiate fake processors; the ARM-core graph is
explicitly skipped.

In the original untagged-DMA phase, no ARM/x86 guest boot, live SimBricks
peer/checkpoint run, full SimBricks link, E1000 runtime, or native MESI Three
Level runtime was performed. Only the shared MESI machines, generated Three
Level SLICC and Python wiring were checked. The native three-level boundary
is superseded by the follow-up below; the other exclusions still apply.

The former native synthetic auto-stash tests are retired, **not equivalent** to
ordinary DMA. Historical payload-only accuracy and semantic-wire verification
documents are clearly marked retired. Explicit stash runtime evidence follows,
including the repaired declined sparse path.

## Explicit-stash TLM review fixes and HNF sparse merge repair

The bridge now constructs `WriteTransaction` for both explicit stash opcodes.
`WRITE_UNIQUE_PTL_STASH` also uses the existing partial-write address, extent,
and exact access-mask handling. The opcode audit found no other omitted stash
cases in the TLM bridge; data transmission is opcode-independent and already
uses the supplied per-flit byte enables.

Both stash suites retain their full/contiguous cases and now seed a third line
with nonzero bytes before writing offsets 5, 19, 34, and 58 across both flits.
Disabled payload bytes deliberately differ from the seed. Full-line readback
checks updated and untouched bytes. Explicit completed-read assertions prevent
an unreceived read from silently passing at the simulation tick limit.

### Reproduction and results

`build/no-tags/build-arm-tlm.sh` runs:

```sh
export TLM_PATH=/home/minhuw/.local/opt/arm-amba-tlm-2.0
export TMPDIR=/dev/shm/cachelens-no-tags-arm-tlm
scons defconfig build/ARM_TLM build_opts/ARM
scons build/ARM_TLM/gem5.opt -j32
python3 build/no-tags/check_tlm.py
```

After the HNF repair, `build/no-tags/build-merge.sh` incrementally rebuilds the
same ARM target. `build/no-tags/check-merge.sh` runs all maintained TLM cases,
27 native CHI cases using `check_chi_arm_merge.py`, and the five additional
TLM checks listed below. Both scripts keep output and exit statuses on disk.

`build/ARM_TLM` is a symlink to `/dev/shm/cachelens-no-tags-arm-tlm`; only
build products are there. ARM is required by the TLM SConscript source tags.
`build_opts/ARM` selects ARM and CHI through Kconfig; `TLM_PATH` enables
`BUILD_TLM` through SConsopts. The existing NULL builds and compiler wrapper
were not changed. The TLM runner sets `LD_LIBRARY_PATH` to the installed
library directory and records every command in `tlm-results.json`. It requires
zero exit status, one `Suite Success` per generator, and no `Suite Fail`.
Logs, configs, statistics, and result JSON remain on disk under `build/no-tags`.
The new ARM binary/intermediates are tmpfs-only and will need rebuilding after
reboot. The build succeeded with optional capstone/PNG/HDF5/SimBricks warnings.

- **Pass:** accepted and declined full/contiguous/sparse stash, including
  preservation of untouched bytes, all three reads completing, and exact
  stash counters. Both also pass with `--hnf-comp-wu` (separate DBIDResp/Comp).
- **Pass:** all 12 other original maintained registrations: untagged DDIO, DMA
  cross-slice partial write, both read-allocation suites, descriptor/full DDIO
  (including separate completion), no-DDIO miss/hit, dirty-owner partial write,
  sparse partial masks, and zero write.
- **Pass:** new maintained `write_unique_sparse_no_ddio.py`: an ineligible
  requestor with DDIO ways available performs a cold sparse write, then a
  sparse hit after an ordinary read allocates the line. Both full-line
  readbacks complete; exactly three misses and two hits prove no allocation
  on the cold write and unchanged retention on the local hit. DDIO fill/access,
  deallocation, and stash counters remain zero.
- **Pass:** original `sparse-no-stash-repro.py` and generated variants of the
  dirty-owner suite (`sparse-owner-merge.py`, `sparse-clean-owner-merge.py`).
  The variants use ineligible writers and sparse cross-flit masks; they check
  preservation of authoritative dirty-owner data or fetch memory after a
  clean owner's no-data response, with no write allocation. All generators
  report success. Exact commands/results are in `tlm-merge-extra-results.json`.
- **Pass:** `LD_LIBRARY_PATH=/home/minhuw/.local/opt/arm-amba-tlm-2.0/lib
  python3 build/no-tags/check_chi_arm_merge.py`: 27 native directed cases on
  the rebuilt ARM binary, without the NULL import wrapper. Checks cover
  eligibility, full/partial writes, owner/sharer handling, all three inclusion
  policies, retained aggregate counters, and removed-stat absence. This is
  data-port traffic, not an ARM guest boot.
- **Pass:** suite AST parsing and `git diff --check`.

### Root repair and original failure evidence

Before the HNF repair, 13/14 registrations passed; the declined sparse stash
read did not complete (`AssertionError: [3, 4]`, non-empty action queue).
`tlm-results-before-merge.json`, `tlm-write-stash-declined-before-merge.log`,
and `tlm-declined-debug/trace.log` preserve that failure. The trace shows four
enabled bytes reaching the SNF, which expected the 54-byte address extent.
The memory interface represents contiguous writes; changing only its expected
byte count would risk overwriting untouched bytes.

The bounded repair leaves `CHI-mem.sm`, `MemoryMsg`, and packet APIs unchanged.
`sparseWriteThrough()` recognizes a nonempty partial-write mask with holes
(`count() < accSize`), excluding accepted stash and positive-way eligible DDIO
fills. HNF partial-miss and owner-backed writeback paths route that condition
through the **existing** no-retention merge/write-through actions:

1. Drain invalidation snoops and retain authoritative dirty-owner data.
2. `PrepareDDIOWrite` fetches memory only when a complete authoritative line
   is unavailable, before accepting and merging the incoming write data.
3. Send a full `WriteNoSnp` plus `SendWBData`, then discard the HNF copy.

The preparation assertion explicitly allows this generic sparse condition
only with `!doCacheFill` and `dataToBeInvalid`. Its zero-directory-sharers
assertion and all existing coherence checks remain intact. Noneligible
contiguous writes keep their original path; neither DDIO eligibility nor
allocation policy is changed. The local-write path already merges into its
cached line and emits no partial downstream memory request, so it is unchanged
and covered by the new sparse-hit regression. Non-HNF forwarding preserves the
exact mask and reaches the repaired HNF path.

**The earlier declined/plain sparse failures are resolved.** Raw sparse writes
directly to the SNF are not added as a supported interface; these HNF paths
instead publish a merged full line using the existing protocol machinery.

## Native x86 three-level primary smoke

**2026-09-28 follow-up, base `d7614d57f4`: 12/12 native cases pass.** This is
`X86_MESI_Three_Level`, not the earlier NULL/MESI_Two_Level substitute. The
tracked runner is `verification/ddio-smoke.py`; testlib uses the same exact
expectations and completion checks in `tests/gem5/mesi_ddio/three_level_checks.py`.
The two-level suites remain unchanged, supplementary regressions.

Build commands (from the dev gem5 root):

```sh
scons defconfig build/X86_MESI_Three_Level build_opts/X86_MESI_Three_Level
scons build/X86_MESI_Three_Level/gem5.opt --without-tcmalloc -j32
```

Only the new `build/X86_MESI_Three_Level` products were placed behind a symlink
to `/dev/shm/cachelens-x86-three-level`; sources remain in this dev tree.
`TMPDIR` also pointed there. Existing ARM_TLM and NULL builds were untouched.
`build/three-level-smoke/build.sh`, `build.log`, `build.exit` and `build-config`
retain the exact commands, configuration and status. Defconfig took **3.67 s**;
compilation/link took **480.04 s**, exit 0. Optional capstone, PNG, HDF5 and
SimBricks support were unavailable; none is needed by this smoke.

A **58 MiB stripped-debug opt binary** (assertions retained) is persisted on
normal disk at `build/three-level-smoke/X86_MESI_Three_Level.gem5.opt`.
`validate.sh` records its creation; `binary.sha256` records SHA-256
`e6123886c3fea240aef7b9a862ea92126b74860f038fac2cd121374782d4a5a5`.
Unlike tmpfs intermediates, this executable remains usable after reboot:

```sh
python3 verification/ddio-smoke.py \
  --gem5 build/three-level-smoke/X86_MESI_Three_Level.gem5.opt \
  --output build/three-level-smoke-rerun
```

Choose a **new** output directory for every run; existing directories are
rejected, even if empty. Omitting `--output` creates a fresh temporary directory.
The final measured invocation used `--output build/three-level-smoke/native-final`:
**12/12 pass, 15.75 s wall time**, exit 0. `native-final.log`, `native-final.exit`
and `native-final/summary.json` retain results, elapsed times and every gem5
command; per-case `run.log`, `config.ini`, `config.json` and `stats.txt` retain
native evidence. The initial successful run (`native/`, 15.65 s) is preserved.

What passed:

- Actual native physical L1/L2/L3 controller types, private queue connections,
  capacities, distinct core/LLC clocks, bank indexing and `[eligible, generic]`
  DMA-port policy assertions in the production CacheLens hierarchy fixture.
- Untagged eligible and noneligible traffic; full/partial writes; authoritative
  dirty-owner bytes and sharer invalidations; clean/dirty inclusive subset
  replacement; read-miss no-allocation; sparse and zero masks. All original
  C++ byte/readback and exactly-once-completion assertions remain unchanged.
- CPU traversal through **both private levels**: `partial_ss_sharers` requires
  exactly two demand misses in each CPU's physical L1D **and** private L2
  (before and after DMA invalidation), alongside the data checks.
- Two-bank routing: exactly one fill, one access and one ownership request/ack
  **per L3 bank**; DMA ReadRequest/Data/WriteRequest/Ack totals each equal two.
- DDIO-off control: partition `-1`, zero DDIO fills/allocations/ownership claims,
  five directory proxies and one access to the previously CPU-filled line;
  write-through readbacks and repeated miss confirm no retention.
- The former three-level expected counters all matched natively; none was
  relaxed. New total fill/allocation checks explicitly require zero for
  CPU-line hits, read-only misses, zero masks, noneligible DMA and DDIO-off.
  Aggregate cache metrics were not relabeled as payload/source metrics.

Additional checks: 25 Python MESI/DDIO tests pass (including four runner/helper
regressions for missing/wrong stats, completion markers, process failures,
timeouts and evidence preservation); five hierarchy unit tests pass; config
contract passes; testlib discovers all 12 x86 opt registrations, and its native
`partial_ss_sharers` execution plus shared verifier both pass with `--skip-build`
(`testlib-runtime.log`, 1.83 s); `git diff --check` passes. Logs are in `build/three-level-smoke/`. The runner also rejects
the real NULL/MESI_Two_Level binary: **0/12 pass, exit 1** in
`rejected-null-two-level/`, rather than silently substituting that profile.

This closes native three-level directed **data-port** validation, not x86
instruction execution or a guest/application accuracy run. ARM/CHI evidence
remains the earlier native ARM/TLM results above; no ARM rebuild, live device
traffic, full-system boot or workload validation was added in this follow-up.

## Accepted DMA LLC tag metrics (`dma-llc-tag-v1`)

Follow-up from clean `46e590ee79`: six directional `dmaRead*`/`dmaWrite*`
request/hit/miss scalars now count eligible **accepted per-line tag lookups**,
including DDIO-off. No descriptor/payload/provenance metadata was added.
See [the exact metric contract](../../CACHELENS_PROFILES.md#dma-llc-tag-hit-metric-dma-llc-tag-v1).
MESI counts after any subset replacement/recycling, not first arrival. CHI
counts CacheMemory entries, not directory-only entries or local data validity.
Existing policy and way accounting remain unchanged.

New, separate evidence: `build/dma-llc-metrics/` (previous evidence preserved).
Incremental native X86_MESI_Three_Level and ARM_TLM builds both exit 0, with
intermediates in the existing `/dev/shm` trees. Persisted stripped-debug opt
binaries are **now** `build/dma-llc-metrics/X86_MESI_Three_Level.gem5.opt` and
`ARM_TLM.gem5.opt`; hashes are in `binary.sha256` and `arm-binary.sha256`.
The old three-level binary predates these counters and remains historical.

- **15/15 primary native Three-Level cases pass**: `native-final/summary.json`.
  Exact counters cover full-write miss/hit, owner-backed hits, no-allocation
  reads, partial/sparse merges, zero fragments, eligibility, DDIO-off,
  subset replacement and two-bank routing. `overlap_dirty_write` requires
  **two transient recycles but exactly two accepted writes (one hit/miss)**.
- **Eight-bank native routing passes**, one write miss and one read hit per
  bank: `routing_full_line-8/stats.txt`, command/assertions in
  `check-three-extra.py` and `three-extra-results.json`. Default smoke routing
  remains two-bank. The current outer analyzer also parsed those real eight-bank
  statistics: eight read hits out of eight reads, zero write hits out of eight
  writes (`outer-analyzer-integration.json`). This checks the reporting path,
  not a guest ROI. Three additional race runs retain exact denominators;
  they do not claim transient-recycle coverage (their recycle counters are 0).
- **32/32 native ARM/CHI cases pass**: `chi-arm-merge-final-results.json`.
  Exact HNF counters include all inclusion policies, dirty-owner resident tags
  versus directory-only `RU`, untagged traffic, repeated misses and DDIO-off.
  **15/15 ARM CHI-TLM registrations pass**: `tlm-final/tlm-results.json`, with
  added exact merge/no-allocation/noneligible/off counter checks. Existing
  runtime drivers were retained and copied to the new evidence destination.
- **12 CacheMemory unit tests**, **25 MESI Python tests**, **5 hierarchy tests**,
  config-contract check, Python parsing and `git diff --check` pass.

Initial CHI expectation failures are preserved in `chi-arm-merge-tests/`:
ordinary generic reads can allocate, and noninclusive is not synonymous with
absent tags. Corrected checks account for those existing policies; the new
read-miss fixture disables seed-write allocation. An exploratory, previously
unregistered MESI `generic_partial` scenario through CHI aborted on a cross-line
`WriteMask::setMask` bound; it is not advertised as supported or repaired.
Pure noneligible counters are checked by the maintained TLM sparse suite.
No guest/application run, forced CHI stale/retry test, or CHI multibank test is
claimed. MESI transient recycling is directly asserted as described above.
