# CacheLens integration

This fork adds coherent DMA allocation and measurement to two gem5 targets:

| Target | Physical cache hierarchy | I/O allocation |
| --- | --- | --- |
| x86 / MESI_Three_Level | private L1I/L1D → private L2 → inclusive shared L3 | Configurable DDIO way subset |
| ARM / CHI | private L1I/L1D → private L2 → shared HNF cache/directory | Configurable coherent-I/O allocation; disabled by default |

DDIO eligibility comes from explicitly selected DMA ports/controllers. NICs
issue ordinary DMA without descriptor, header, payload or RDMA semantic tags.
Eligible writes allocate within the configured way subset; `ddio_way_part=-1`
disables new write allocation. Eligible read misses do not allocate. Coherence,
partial-write merging and existing-line updates remain active in either mode.

## Architecture and measurement

- [Architecture profiles and metric contracts](CACHELENS_PROFILES.md): topology,
  port eligibility, DMA tag-hit counters and line-state distributions.
- [Explicit CHI write stashing](CACHELENS_STASHING.md): optional acceptance of
  explicit stash opcodes, separate from ordinary NIC DMA.
- [Untagged DMA validation](verification/no-tags/README.md): retained native
  MESI, ARM/CHI and TLM evidence, with workload and coverage boundaries.

`dmaRead*` and `dmaWrite*` counters measure accepted eligible cache-line
lookups. A hit means an LLC CacheMemory tag was present before the request's
own update; it does not identify which component supplied authoritative data.
Device-local issuance counters have separate operation/byte denominators.

## Focused verification

The primary DDIO smoke requires an actual x86/MESI_Three_Level binary:

```sh
python3 verification/ddio-smoke.py \
  --gem5 build/X86_MESI_Three_Level/gem5.opt \
  --output build/ddio-smoke-new
```

Use a new output directory. The smoke checks native private L1/L2 traversal,
coherent DMA data, partial masks, allocation policy and exact per-bank counters;
it does not boot Linux or validate model accuracy. Build commands and target
requirements are documented in [CACHELENS_PROFILES.md](CACHELENS_PROFILES.md).

Host-only checker and wiring regressions can be run separately:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -B -m unittest discover \
  -s tests/pyunit/mesi_ddio -p 'pyunit_*.py'
```

The parent CacheLens repository owns pinned source builds, guest assets,
SimBricks/i40e workload launchers and experiment evidence. Imported historical
reports describe their recorded runs; they do not certify a new checkout or
replace fresh workload validation. Current initialized-NIC checkpoint support
and its restricted boundary are described in
[verification/initialized-nic/README.md](verification/initialized-nic/README.md).
