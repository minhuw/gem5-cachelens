# Stage 2 DMA observation: inspected API and ownership

Owner: c16 / worker d9aa9279. Branch `codex/stage2-dma-observe` in
`/home/minhuw/cachelens/.pi/workspaces/gem5-dma-observe` (pinned independent Git
clone), base `1f3eb7a3b2544eb86c57e9aed8b96a780d5a39fb`.
Only this fork's files and build outputs are writable. No parent hardware layer,
PVRDMA functional code, host runner, producer, canonical sources, frozen binaries,
source A, or common NIC runner is owned or modified here.

## Runtime interface implemented

`SimBricksPci` ordinary SimObject parameters, available in `gem5.fast`:

- `dma_observe_file`: empty by default; enable with a fresh absolute JSONL path.
  Exclusive creation fails if the file exists, including a previous epoch.
- `dma_observe_nic`: explicit endpoint identifier, required when enabled.
- `dma_observe_epoch`: explicit fresh lifecycle epoch, required when enabled.

Enabled scope is one SimBricks PCI endpoint, timing mode, no checkpoint/restore.
Unsupported cases fail explicitly. Wire validation/payload limits stay unchanged.
No environment variable, DPRINTF, producer wire change, payload parse, extra DMA,
probe, or modeled event implements tracing.

## Exact inspected/owned source points

- `src/simbricks/SConscript` compiles `src/simbricks/pci.cc`; legacy
  `src/dev/simbricks` is not the active frontend.
- `src/simbricks/pci.{cc,hh}`, `src/simbricks/SimBricks.py`: actual wire acceptance,
  explicit endpoint/epoch, native observed acceptance instance, active-ID collision
  detection, completion publication. Native sequence is neither producer `op_seq`
  nor allocator generation. All READ/WRITE requests remain unknown traffic class.
- `src/mem/dma_trace.{hh,cc}`: `DmaTraceOperationPtr` is
  `shared_ptr<const DmaTraceOperation>`; `DmaTraceExtension` derives from the
  existing `Extension<Request, DmaTraceExtension>`. Its fragment fields are const;
  Request cloning preserves them through the existing extension clone interface.
- `src/dev/dma_device.{hh,cc}`: both `DmaPort::dmaAction` signatures receive a
  trailing default-null `DmaTraceOperationPtr` after existing flags. DmaReqState
  owns the optional immutable operation. `createPacket()` attaches an extension
  using actual Packet address/size and ChunkGenerator offset. Rejected sends and
  successful timing sends are separate events; retries retain the same extension.
- `src/mem/ruby/system/DMASequencer.cc`: existing fragment `seqReq` retains the
  carrier; enqueue observation reports actual Ruby fragment address/line/size.
- `src/mem/ruby/protocol/MESI_Three_Level.slicc` includes the shared
  `MESI_Two_Level-L2cache.sm`. Its `dp_profileDmaAcceptance` existing
  `profileDmaLookup` call receives `in_msg.seqReq`, `address`, and `in_msg.isSeqReqValid`.
- `src/mem/ruby/protocol/chi/CHI-cache-actions.sm`: existing `Initiate_Request`
  and `Initiate_Request_Stale` counter points pass `tbe.seqReq`, `address`, and `tbe.isSeqReqValid`.
  CHI's existing `dma_lookup_profiled` gate is unchanged.
- `src/mem/ruby/structures/CacheMemory.{hh,cc}` and
  `src/mem/ruby/protocol/RubySlicc_Types.sm`: counter method accepts the pointer
  and line; observation reuses its existing hit/state/dirty/owner inputs. No new
  lookup, state, counter, eligibility, allocation, replacement or network bits.
  Actual cache/bank identity is `CacheMemory::name()`, not physical LLC identity.

## Consumer boundary

JSONL schema `cachelens.gem5.dma`, version 1: exact integer `tick`,
`ticks_per_second`, `nic`, `epoch`; lifecycle key is
`(nic, epoch, wire_req_id, observed_instance)`. Events include `scope`,
`wire_accept`, `fragment_prepared`, `fragment_rejected_wait_retry`,
`fragment_accepted`, `ruby_fragment_enqueued`, `accepted_cache_lookup`, and
`wire_completion_publish`. Counter population is explicitly
`existing_eligible_dma_llc_lookup_counter`, with `dmaReadRequests` or
`dmaWriteRequests`, tag presence and protocol-prefixed native state. Data-array
hit, any-cache hit and CPU joint state are unavailable. Missing extension or
RequestPtr emits an explicit unmatched reason; no address/time correlation.
Wire address maps through `pciToDma` host bridge before any downstream
translation; this is not a claim that IOVA equals PA.

Historical producer `ee3fd33` stays immutable. Accepted followup
`2ca471094ac5c94d84cf850ea70aeb3ad19bfa61` adds Admin/HMC coverage within
`cachelens.i40e.dma` v1, still without NIC/epoch. Its retained review records
17/17 producer PCI-peer operations, not native join proof. Explicit external
one-NIC/process-or-restore-epoch binding and complete per-ID lifecycle coverage
remain necessary. TX continuation reuses wire ID; `op_seq` restarts per process.
Ambiguous sparse lifecycle data stays unmatched; provided chunk ranges cannot be
used to guess lifecycle identity. Admin payload reads are unit-only, generic HMC
unexecuted, ARQ unimplemented, restore quiescent-only. CLRX work remains with the
producer owner and is not consumed before committed-schema review. Physical LLC
contract and parent hardware changes remain with coordinator/helper 01a1db74.

## Build scope

Separate own `build/DMA_OBSERVE_MESI` and `build/DMA_OBSERVE_CHI` outputs,
resource checked and `-j2`. These directed no-guest builds omit a CPU ISA; they
execute actual SimBricks PCI transport, DmaPort, and Ruby protocols. No claim of
CPU/guest or full i40e workload validation follows from these directed checks.
This artifact describes implemented API; actual passing directed execution and
source/binary provenance are recorded in RESULTS.md and evidence/manifest.json.
