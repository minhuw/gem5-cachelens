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

Only explicit `WriteUniqueFullStash` and `WriteUniquePtlStash` requests carry
hints. Native NIC DMA and the SimBricks adapter issue ordinary reads/writes;
there is no automatic payload-only emission or Request semantic classification.

An accepting Home allocates through the normal coherent write/merge and cache
fill/replacement paths. Full writes need no memory read for untouched bytes.
Partial writes preserve bytes outside the write, including newer data held by
a dirty CPU owner. Existing coherence transactions invalidate conflicting
private copies. No direct tag insertion bypasses the protocol.

Stashed data uses **ordinary HNF cache ways**, not a DDIO-reserved subset.
No source classification or NIC/RDMA cache counters are retained. Acceptance
is a placement policy,
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

At controller level, `accept_write_stash` controls Home acceptance and defaults
to false. `cache_stashing="hnf"` enables acceptance only; it does not cause a
NIC to generate stash requests. `emit_rx_write_stash` has been removed.

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
## Validation

The native sequencer's historical synthetic auto-stash scenarios are retired.
They are **not** preserved by sending hints for all writes. CHI-TLM suites
`write_stash_explicit.py` and `write_stash_declined.py` inject actual full/partial
stash opcodes and check written/untouched bytes and all four stash counters.
They require the optional Arm TLM dependency; adding these suites is not a
claim that a TLM runtime or a full-system NIC stash path has been verified.
Ordinary native DMA regressions require zero stash requests.
