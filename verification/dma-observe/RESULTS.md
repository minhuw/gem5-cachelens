# Directed native DMA observation results

**PASS: four actual SimBricks PCI peer runs (MESI off/on, CHI off/on), with
clean gem5 and peer exits, plus seven expected rejection cases.** No full guest,
production i40e packet-classification join, data-array hit, or CPU joint-state
claim follows from these tests.

| Protocol / case | Wire lifecycles | Prepared / accepted packets | Rejected send attempts | Matched / unmatched accepted lookups | Off/on result |
| --- | ---: | ---: | ---: | ---: | --- |
| MESI Three Level, actual PCI peer off/on | 6 per run | 22 / 22 | 18 | 22 / 2 | 820 modeled statistic values, exact ordered wire records/ticks, bytes and final tick equal |
| CHI, actual PCI peer off/on | 6 per run | 22 / 22 | 18 | 22 / 2 | 1,412 modeled statistic values, exact ordered wire records/ticks, bytes and final tick equal |
| Rejections through compiled MESI frontend | 7 cases | No success claimed | Not a traffic-equivalence test | No join claim | Missing NIC, missing epoch, existing file, atomic mode, checkpoint mode, multiple endpoints and active-ID reuse all rejected |

Each positive run checks 400 bytes returned to the actual PCI peer and a further
64-byte readback from an untagged test-only DMA source. Writes cover 480 bytes:
416 wire bytes plus 64 untagged bytes. The checks compare reads byte-for-byte
against expected patterns; they do not require backing DRAM to contain dirty
cached data.
All positive runs finish at tick **50,000,000**, with **10^12 ticks/second**.
Default off creates no trace. Host wall-clock/process measurements are excluded
from statistic comparison; all parsed modeled values are compared. The exact
comparison includes existing requests, hits/misses, native-state/flag marginals,
way/allocation/routing counters, network events and simulation ticks.

The known wire IDs are `0xfedcba9876543210`, `0x123456789abcdef0`, and
`0x9988776655443322`. The first ID is reused only after completion, yielding
native instances 1, 4, 5 and 6. The complete peer stream is matched by exact
per-ID lifecycle order, never nearest address or time. The raw PCI address
`0x38` is mapped through the nonzero host bridge base to `0x1038`; its 160-byte
operation produces actual packet sizes **8, 64, 64, 24**. Other unaligned
operations are 96 and 80 bytes. This fixture exercises lengths up to 160 bytes;
maximum wire payload capacity is not a tested claim.

MESI follows the actual DmaPort -> DMASequencer -> protocol request -> shared
LLC action path, including 22 observed Ruby fragment enqueues. CHI follows the
actual DmaPort -> RubySequencer -> CHI request/TBE -> HNF accepted-lookup path.
Both use two cache banks, no CPU and no guest. To avoid CPU-resource imports in
these deliberately ISA-free builds, the final fixture directly instantiates the
existing protocol node constructors; CHI uses the existing inclusive CacheLens
HNF parameter values and the standard 32-byte data channel. There is no CHI
functional or protocol patch to make this fixture pass.

For each protocol, all **24** emitted accepted lookup records equal the sum of
the existing eligible-DMA request counters, without deduplicating real modeled
lookups. The two untagged operations are retained with null wire identity and
`request_has_no_trace_extension`. MESI records 11 `MESI_NP` and 13 `MESI_M`
lookups; CHI records 11 `CHI_I` and 13 `CHI_UD`. These are existing tag/state
semantics, not global residency or data-array-hit assertions. Actual cache/bank
names and full records are retained in the evidence.

## Exact executions and artifacts

The completed commands are recorded as full argv in
[`evidence/manifest.json`](evidence/manifest.json), the per-protocol
`runtime-manifest.json`, and `rejections.json`. They execute inside the existing
`cachelens-gem5-build:2025` image, ID
`sha256:40b80589cec91a588022a7f2580780f2420cda3051bac3db153162d5b14fa029`,
with `--network=none`, user `1001:1002`, `PYTHONDONTWRITEBYTECODE=1`, and the
owned worktree mounted at its absolute path. The inner successful commands were:

```sh
python3 /home/minhuw/cachelens/.pi/workspaces/gem5-dma-observe/verification/dma-observe/run.py --gem5 /home/minhuw/cachelens/.pi/workspaces/gem5-dma-observe/build/DMA_OBSERVE_MESI/gem5.fast --protocol mesi --out /home/minhuw/cachelens/.pi/workspaces/gem5-dma-observe/.pi/e/m4
python3 /home/minhuw/cachelens/.pi/workspaces/gem5-dma-observe/verification/dma-observe/run.py --gem5 /home/minhuw/cachelens/.pi/workspaces/gem5-dma-observe/build/DMA_OBSERVE_CHI/gem5.fast --protocol chi --out /home/minhuw/cachelens/.pi/workspaces/gem5-dma-observe/.pi/e/c4
python3 /home/minhuw/cachelens/.pi/workspaces/gem5-dma-observe/verification/dma-observe/reject.py --gem5 /home/minhuw/cachelens/.pi/workspaces/gem5-dma-observe/build/DMA_OBSERVE_MESI/gem5.fast --peer /home/minhuw/cachelens/.pi/workspaces/gem5-dma-observe/.pi/e/m4/peer --protocol mesi --out /home/minhuw/cachelens/.pi/workspaces/gem5-dma-observe/.pi/e/n1
```

Output directories were fresh; do not reuse them. Committed evidence contains
both native JSONL streams, off/on peer wire JSONL, complete statistics and logs.
The complete original run directories, negative logs, compiled peers, build logs,
dependency snapshot and source witnesses remain under this workspace's `.pi`.
The evidence manifest indexes raw artifact SHA-256 values.

## Source and binary provenance

Base: `1f3eb7a3b2544eb86c57e9aed8b96a780d5a39fb`, isolated branch
`codex/stage2-dma-observe`. Binaries were built from an uncommitted tree. **No
compiled-commit identity is claimed.** The final native-input fingerprint is
`5b0439fb42b93681418cd714dfd2c26a9e12b3e98dc4520c531e4e7a9a341dc5`;
9,475 native inputs were rehashed against the saved in-flight witness with zero
changes. Final runtime configuration/peer/script hashes are separately recorded.

| Binary | SHA-256 |
| --- | --- |
| MESI final, used for m4 and seven rejections | `74c4f9d1cbdd983245cec7c97842f2023f9173bb79076981d92cb1c0a0a4cecc` |
| CHI final, used for c4 | `a7b545524d7047b654e02e9f90f18716564c6ca163c9c648af1301e55b9f230b` |

Original managed builds `p_d6e4` / `p_2cb2` exited zero at `-j2`. The first MESI
binary (`84e251d3...`) lacked the test object added after its SCons graph began;
actual import failure justified incremental own build `p_0745`, which exited
zero. Its saved build-start and post-build native-input witnesses agree. The
original in-flight snapshot cannot retroactively prove earlier build-start bytes;
that limitation remains explicit in the provenance records. No frozen 1f3
binaries were rebuilt or modified.

Initial configuration failures and their logs remain retained: missing MESI test
object; CPU-only `PcCountTrackerManager` import in the ISA-free high-level CHI
hierarchy; missing CHI channel size in direct wiring; missing direct MESI directory
version initialization. These were fixed by the justified MESI test-object build
and test-configuration wiring. An earlier test peer asserted on the transport's
timestamp-zero termination marker after completing the byte checks, and the old
driver failed to reject the peer exit. Final m4/c4 handle termination explicitly
and require peer exit zero; earlier m3/c3 are superseded, not final passing gates.
No observer C++ or cache-physics change was required after build completion.

## Remaining boundaries and retention

Native records stay unclassified. Exact production classification belongs to the
separate parser/producer owners and requires explicit NIC plus process/restore
epoch binding. Wire IDs can repeat across TX continuations; producer `op_seq`
restarts with a new process/restore and is not native `observed_instance`.
Producer's separately reported final `50ce8d4...` backpressure 72/72 proof is not
these native results and was not rerun here. The separate parser candidate
`24dccc95151d69218a233e2f8b136532e202925f` is not edited or executed here.

No runtime packet parsing, allocator generation, CPU joint state, data-array hit,
production i40e integration, checkpoint/restore support, parent source-pin update,
main integration, or old full-system campaign was performed. Physical LLC names
remain a parent contract; emitted cache names are actual Ruby objects.

Cleanup: retain this pinned workspace, branch, binaries, source witnesses and raw
evidence for review/consumption; no branch, worktree, pin or artifact was released.
