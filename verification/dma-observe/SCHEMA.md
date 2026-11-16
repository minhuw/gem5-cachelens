# gem5 DMA observation trace

Enabled per SimBricks PCI device by `SimBricksPci.dma_observe_file` (timing mode,
no checkpoints). JSONL, one object per line, buffered and flushed at exit. Every
record has `event`, `tick` (gem5 ticks) and `nic` (`dma_observe_nic`, default the
SimObject name).

| Event | Extra fields |
| --- | --- |
| `wire_accept` | operation fields |
| `wire_completion_publish` | operation fields, after the completion is on the H2D ring |
| `fragment_prepared` | operation + fragment fields |
| `fragment_rejected_wait_retry` | operation + fragment fields, per failed send |
| `fragment_accepted` | operation + fragment fields, after a successful `sendTimingReq` |
| `ruby_fragment_enqueued` | operation + fragment fields, `ruby_physical_address`, `ruby_line`, `ruby_size`, `ruby_request_offset` (MESI) |
| `accepted_cache_lookup` | operation + fragment fields, `counter`, `cache`, `lookup_line`, `tag_present`, `native_state`, `local_dirty`, `private_owner` |

Operation fields: `wire_req_id` (wire token), `observed_instance` (per-device
sequence at wire acceptance), `wire_address` (PCI address), `pci_to_dma_address`,
`length_bytes`, `direction` (`read` = host-to-device, `write` = device-to-host).
A wire ID that is reused while still active is fatal.

Fragment fields: `fragment_address`, `fragment_size`, `operation_offset`,
`fragment_line` (system cache-line size), taken from the DmaPort packet.

Lookups are emitted at the existing `dmaReadRequests`/`dmaWriteRequests` counter
points, only for requests carrying a trace extension; lookups of untraced
requests are counted by the stats but not logged.
