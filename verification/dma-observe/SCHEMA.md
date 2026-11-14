# `cachelens.gem5.dma` version 1

JSONL, one complete flushed object per line. All numeric identifiers, addresses,
lengths and ticks are lossless JSON integers; consumers must avoid floating-point
conversion. Every record carries `schema`, `version`, `event`, exact simulation
`tick`, `ticks_per_second`, explicit `nic` and `epoch`. A fresh absolute output
path is exclusively created. Default empty file parameter creates nothing.

| Event | Population | Additional fields |
| --- | --- | --- |
| `scope` | `all_wire_dma_and_existing_eligible_llc_counters` | `traffic_class="unknown"`, `producer_op_seq=null`, `packet_identity=null`, `cpu_joint_state=null`, `translation_stage` |
| `wire_accept` | `all_accepted_simbricks_pci_dma_requests` | operation fields, `translation_stage` |
| `wire_completion_publish` | `all_accepted_simbricks_pci_dma_requests` | operation fields; completion is published on the real H2D ring before this event |
| `fragment_prepared` | `actual_dma_port_packet_fragments` | operation + fragment fields; creation of an actual Packet, not successful admission |
| `fragment_rejected_wait_retry` | `actual_dma_port_packet_fragments` | operation + fragment fields; each failed send, retained across real retry |
| `fragment_accepted` | `actual_dma_port_packet_fragments` | operation + fragment fields; emitted only after successful `sendTimingReq` |
| `ruby_fragment_enqueued` | `enabled_ruby_dma_sequencer_fragments` | operation + fragment fields, `ruby_physical_address`, `ruby_line`, `ruby_size`, `ruby_request_offset`; actual enabled fragment enqueued by DMASequencer (MESI path) |
| `accepted_cache_lookup` | `existing_eligible_dma_llc_lookup_counter` | matched operation + fragment fields, or explicit missing-identity fields; lookup fields below |

Operation fields: `wire_req_id` (real unmodified uint64 wire token),
`observed_instance` (native sequence incremented at actual wire acceptance),
`wire_address` (raw PCI DMA address / IOVA), `pci_to_dma_address` (actual
`pciToDma` result), `length_bytes`, `direction` (`read`=host-to-device,
`write`=device-to-host), `traffic_class="unknown"`. Lifecycle key is
`(nic, epoch, wire_req_id, observed_instance)`. Instance is neither producer
`op_seq` nor allocator generation. Repeated active wire ID is rejected;
post-completion reuse creates a new instance. No payload or descriptor bytes are
parsed. Unknown/admin/descriptor/control requests remain present.

Fragment fields: `fragment_address`, `fragment_size` are taken from the actual
Packet as constructed by DmaPort; `operation_offset` is its actual generator
progress; `fragment_line` uses the system cache-line size. These describe the
DmaPort packet, before downstream translation. The `translation_stage` string
is `pciToDma_host_bridge_before_downstream_translation`. Neither raw IOVA nor
host-bridge target is claimed to be an application VA or final translated PA.
Ruby enqueue fields and `lookup_line` describe the subsequent actual protocol
sites. An extension follows Request copies; no address-based recovery is used.

Lookup fields: `counter` is the existing `dmaReadRequests` or
`dmaWriteRequests`; `cache` is the full actual `CacheMemory::name()` including
Ruby cache/bank ownership; `lookup_line`, `lookup_direction`, `tag_present`,
`native_state` (protocol-prefixed label from existing marginal counter labels),
`local_dirty`, `private_owner`, `ddio_eligible=true`, `data_array_hit=null`,
`any_cache_hit=null`. Existing local-dirty values may be false with no local tag;
interpret alongside `tag_present`, as the existing marginal counters do.
MESI includes the existing stable accepted lookup actions, including their real
replays. CHI includes the existing accepted normal/stale points with their
unchanged `dma_lookup_profiled` gate. No new deduplication is introduced.
The event population is eligible I/O at those counter points; it is not all
wire fragments, all cache accesses, CPU state, data-array hits, or global cache
residency. Noneligible DMA may have wire/fragment records without these counters.

If the protocol RequestPtr is null, lookup identity is explicitly
`wire_req_id=null`, `observed_instance=null`, `traffic_class="unknown"`,
`unmatched_reason="protocol_request_pointer_unavailable"`. A present Request
without the extension uses `unmatched_reason="request_has_no_trace_extension"`.
A non-null pointer whose existing protocol validity flag is false uses
`unmatched_reason="protocol_request_marked_invalid"` and is not dereferenced.
These affected counter events remain in the same stream; no line/time join is
attempted. All native wire records are unclassified until an exact external join.

## Producer association limits

Historical producer `ee3fd33` remains immutable. Accepted followup
`2ca471094ac5c94d84cf850ea70aeb3ad19bfa61` retains additive
`cachelens.i40e.dma` version 1. Its committed contract covers every current model
`Runner::IssueDma` source path: LAN descriptor/payload/head, ATQ admin
`admin_descriptor`/`admin_payload`, and HMC. `queue_id` is omitted for
`admin_tx` and generic `hmc`; LAN queue-context HMC retains actual queue ID and
adds pre-translation `hmc_address`, `hmc_segment_index`,
`hmc_segment_offset_bytes`. HMC `dma_address` remains the actual PCI host address.
`complete` now means entry to `DmaComplete`, before virtual `done()` mutates or
deletes the DMA object. The common Runner and wire format remain unchanged.

The retained producer `review-v2` records 17/17 real PCI-peer operations (34
issue/complete records), including TX continuation sharing one wire ID across
separate `op_seq` 11/12 and actual chunk ranges. These are producer-only results,
not executed native fragment/cache join proof. Admin `payload_read` has unit
coverage only; generic HMC operations remain unexecuted; Admin RX/ARQ is not
implemented. Producer restore evidence is only quiescent initialized save/restore
(pre=2/post=3 operations), not inflight or packet-runtime restore support.

NIC and epoch fields are still absent. A consumer must explicitly bind each
producer file to the exact single NIC and fresh process/restore epoch, then match
real wire lifecycles with complete per-ID coverage. Neither wire ID nor `op_seq`
is globally unique; process/restore restarts `op_seq`, and TX continuation reuses
the same ID. Preserve provided chunk association after lifecycle identification;
never select a match using time, address, length, or presumed allocation age.
A sparse/partial trace with reused IDs can remain ambiguous even under the newer
source contract; retain native wire requests and affected cache events as
unclassified/unmatched rather than assigning fabricated producer metadata.

The producer's later CLRX packet-observation slice is not consumed here. Only a subsequently reviewed committed observation schema can associate an
actual RX buffer or completed TX assembly with its explicit operation/chunk list.
Issue identity is never backfilled from later bytes. No native CLRX parsing,
allocator generation, future/stale buffer inspection, or post-ROI timestamp join
is implemented. Native tracing keeps all wire records unclassified.

## Unsupported runtime cases

Enabled tracing rejects multiple SimBricks PCI endpoints, another active sink,
missing NIC/epoch, nonabsolute/existing output, atomic/backdoor memory mode,
initialized checkpoint mode, serialization and restore. These restrictions are
observational guards; ordinary disabled operation retains existing semantics.
Trace file failures fail loudly. There is no quiescent restore support in v1;
use an independent fresh process/path/epoch. No logs are truncated or reused.
