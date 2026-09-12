# Arm CHI write stashing: CacheLens implementation scope

## Specification basis

Primary source: Arm **AMBA 5 CHI Architecture Specification, IHI 0050E.b**,
ID081621, [official PDF](https://documentation-service.arm.com/static/6295e03db334256d9ea8ae86).
Relevant sections:

- **7.1, pp. 294–295:** stashing is a performance hint for snoopable memory;
  receivers may decline it. Write stashing and independent `StashOnce` requests
  are different operations.
- **7.2, p. 296:** `WriteUniqueFullStash` and `WriteUniquePtlStash` retain the
  coherence obligations of writes. Ignoring the hint must not discard the write.
- **7.3, pp. 297–298:** independent stashing has ordering requirements; a target's
  DataPull and the stash request must be treated atomically by Home. An ordinary
  asynchronously injected CPU prefetch is not a replacement for that protocol.
- **7.4.2, p. 299:** when no peer target is specified, Home may allocate full
  writes in the system cache; fetching untouched bytes and allocating partial
  writes is recommended when the line is not already cached.
- **7.5, pp. 300–301:** peer-target stashing additionally requires target
  identifiers, stash snoops, and DataPull-related fields.

Additional context: [Arm's CHI introduction](https://support.arm.com/documentation/102407/0102/Cache-stashing)
and [DynamIQ DSU cache stashing](https://developer.arm.com/documentation/100453/0401/L3-cache/Cache-stashing).
The DSU describes both L2 and L3 targets; this model does **not** implement that
complete set of hardware capabilities or model a particular DSU.

## Implemented subset

**Native Ruby CHI, write-with-stash, no peer target, cached HNF/system cache.**

The synthetic I/O bridge emits `WriteUniqueFullStash` or `WriteUniquePtlStash`
for classified NIC RX payload writes. It does not add hints to descriptor,
header, TX-read, or ordinary CPU requests. This is an explicit simulation
policy, not a claim that an E1000 has architectural Arm stash registers.

An accepting Home allocates through the normal coherent write/merge and cache
fill/replacement paths. Full writes need no memory read for untouched bytes.
Partial writes preserve bytes outside the write, including newer data held by
a dirty CPU owner. Existing coherence transactions invalidate conflicting
private copies. No direct tag insertion bypasses the protocol.

Stashed data uses **ordinary HNF cache ways**, not a DDIO-reserved subset. The
source categorization and existing NIC/RDMA counters remain intact, but they
do not turn Arm stashing into Intel DDIO. Acceptance is a placement policy,
not a guarantee that a line remains resident until a later CPU access.

A declining Home processes the write normally. Independent `StashOnce` handling
is unchanged; useful independent-stash allocation is **not** implemented.

## Configuration

For the CacheLens full-system runner, use:

```text
--isa arm --coherence-protocol chi --model-profile arm-generic \
    --cache-stashing hnf --ddio-way-part -1
```

Or construct `CacheLensCHIHierarchy(cache_stashing="hnf", ...)` with DDIO
partitioning disabled. The default is `cache_stashing="off"`. The prebuilt
rejects stashing with x86 or MESI, or with positive `ddio_way_part`.
Configuration metadata records the selected policy and supported scope.

At controller level, `emit_rx_write_stash` controls bridge emission and
`accept_write_stash` controls Home acceptance; both default to false. Setting
Home acceptance false while leaving emission enabled tests legal hint decline.

## Observability

HNF `RubyCache` statistics:

| Counter | Meaning |
| --- | --- |
| `writeStashRequests` | Stash writes admitted at Home, once per transaction |
| `writeStashAccepted` | Admitted hints accepted by the Home policy |
| `writeStashDeclined` | Admitted hints declined by the Home policy |
| `writeStashAllocations` | New cache entries allocated by accepted stash writes |

Requests equal accepted plus declined. Allocation is not incremented for a hit,
a later CPU demand fill, or a retried message that has not been admitted.
Existing NIC write-extent, merge-read, payload, and provenance counters provide
additional checks; their legacy DDIO-related names are not hardware claims.

## Validation

For an existing configured ARM/CHI build, enable the directed tester and run
an asset-free clean-owner regression:

```sh
scons setconfig build/ARM_CHI USE_TEST_OBJECTS=y
scons build/ARM_CHI/gem5.opt --without-tcmalloc -j16
build/ARM_CHI/gem5.opt -d m5out-stash \
  tests/gem5/cachelens/configs/chi_dma_directed.py \
  --scenario stash_clean_owner --cache-stashing hnf
```

The 23 registered stash cases in `tests/gem5/cachelens/test_cachelens.py`
passed with the production CacheLens hierarchy: full and partial writes,
cold and dirty/clean-owner merges, sharer invalidation, all-way replacement,
accepted/declined/off policies, excluded traffic, and synthetic RDMA provenance.
Both inclusive and noninclusive HNF settings have runtime coverage. The
clean-owner case enables the existing `fwd_unique_on_readshared` controller
option to exercise retained `UC_RU` state; default shared-read behavior would
not expose that corner case. Masks remain explicitly rejected, and hitting the
simulation time limit must not be reported as success.

These tests supplement, rather than replace, the existing ARM graph, MESI
three-level, and hierarchy/runner unit regressions. They use data-port adapters,
not instruction execution or a booted guest.

## Explicit limits

- No peer `StashNID`/`StashLPID` targets or private L1/L2 placement.
- No stash snoops, DataPull, independent-stash ordering implementation, or
  `StashOnceSep`/`StashDone` extension.
- No TLM adapter or CHI wire-level conformance claim. This is a bounded native
  Ruby protocol model, not merely a renamed DDIO setting.
- Native RubySequencer still rejects byte-enabled sparse/zero-mask writes;
  contiguous partial writes are supported.
- No new ARM guest image, NIC driver ABI, ARM-board PVRDMA attachment, or
  full-system accuracy result is implied. Synthetic RDMA-marked traffic can
  test provenance without establishing ARM guest RDMA support.
- Defaults preserve existing profiles. The implementation is opt-in; it does
  not retroactively change old experiment results or packaged binaries.
