`SimBricksPci`: `dma_observe_file` (default empty/off; exclusive fresh absolute path), `dma_observe_nic`, `dma_observe_epoch` (required when enabled).

JSONL `schema="cachelens.gem5.dma"`, `version=1`; keys `event,tick,ticks_per_second,nic,epoch`. Lifecycle: `wire_req_id,observed_instance`; mapping: `wire_address,pci_to_dma_address,length_bytes,direction,translation_stage`. Fragment: `fragment_address,fragment_size,operation_offset,fragment_line`. Lookup: `cache,lookup_line,lookup_direction,counter,tag_present,native_state,local_dirty,private_owner,ddio_eligible,unmatched_reason`. `population` labels semantics; `traffic_class="unknown"` preserves all wire requests.

Events: `scope`, `wire_accept`, `wire_completion_publish`, `fragment_prepared`, `fragment_rejected_wait_retry`, `fragment_accepted`, `ruby_fragment_enqueued`, `accepted_cache_lookup`.

Carrier: Request `DmaTraceExtension`; immutable `DmaTraceOperationPtr`; trailing default-null context after flags in both `DmaPort::dmaAction` overloads; existing Ruby `seqReq` propagation.

Owned solely under `/home/minhuw/cachelens/.pi/workspaces/gem5-dma-observe`: `src/simbricks/{pci.*,SimBricks.py}`, `src/dev/dma_device.*`, `src/mem/dma_trace.*`, Ruby DMASequencer/CacheMemory/protocol sites, test objects/build-options/`verification/dma-observe`.

One SimBricks endpoint, timing only; checkpoints/restore rejected. Cache names are Ruby objects; data-array/any-cache/CPU-joint hits unavailable. Producer `2ca4710` adds Admin/HMC; NIC/epoch still absent. Require explicit external NIC/process-or-restore-epoch binding; reused-ID/sparse ambiguity remains unmatched. Producer 17/17 is not native join proof. Admin-read unit-only; generic-HMC unexecuted; no ARQ; restore quiescent-only. CLRX pending committed schema. Common runner unchanged. Directed native runtime verified; production classification remains external.
