> **Historical evidence only; semantic API tests retired.** The metadata/CSV
> runner has been removed. The remaining `dma-test.py` accepts a plain DMA
> peer socket and checks fragmentation, direction and data via DmaChecker;
> it no longer requests or validates semantic metadata or adapter statistics.
> Prior results below do not verify the cleaned wire protocol.

# Accuracy transport verification — completed (not workload accuracy)

All paths below are relative to `gem5/work/sources/simbricks-accuracy`.
No full guest was launched. No hierarchy/coherence policy was changed.

## Final gates

- Existing build container `808323caba81`: `docker wait` returned 0.
- Incremental/final native builds: PASS, including the late-added DMA checker.
  Final log: `cachelens-gem5/accuracy/build-final.log`.
- Production i40e issue/completion tests: PASS. RX q7 descriptor fetch 64 bytes;
  payload 1472 bytes split 1024+448 with identity/offset preserved; TX/admin and
  writebacks untagged; per-port IDs, RSS and poll-mode regressions passed.
  `simbricks/accuracy/model-test-final-2.log` also proves the actual executable
  rejects a legacy peer with nonzero exit and accepts an accuracy peer.
- Real UNIX/SHM protocol tests: PASS. Positive accuracy, legacy, both mismatches,
  bad version/magic, truncated intro, and invalid metadata covered.
  `simbricks/accuracy/protocol-test-final.log`.
- Descriptor-completion CSV: PASS, two actual q7 descriptor buffer addresses
  0x20000/0x21000. `component-i40e/i40e_bm/accuracy/descriptor-test-final.csv`.
- Native PCI/DmaPort tests: PASS, negative legacy/kind/direction/queue/truncation
  cases first, then atomic and timing. Both modes observed **53 actual memory-side
  requests: 4 descriptor, 47 payload, 2 untagged; 3136 bytes**. All fragments carried
  the expected Request category. Payload data, peer completions, CSV identities/
  addresses, and per-queue native stats were checked.
  `cachelens-gem5/accuracy/native-gate-final.log` and `accuracy/native-tests-final/`.
- Topology: PASS, configuration-only geometry/routing assertions. CHI directed
  `hit_update`, `nic_cpu_nic`, `stash_unclassified`: all PASS on the same final
  binary. `cachelens-gem5/accuracy/topology-gate-final.log` and its output directory.
- Legacy PCI port/MSI-X regression: PASS after checker fix; log
  `cachelens-gem5/accuracy/legacy-port-final.log` (before final identical-source relink).
- `git diff --check`: PASS in all three worktrees.

## Preserved initial failures and fixes

1. `accuracy/native-gate-1.log`, `accuracy/native-tests/`: fixture used integer
   `fromSeconds(1)`; API requires float. Changed to `1.0`.
2. `accuracy/native-gate-2.log`, `accuracy/native-tests-2/timing/`: checker assumed
   immediate response acceptance. Reused `QueuedResponsePort`/`RespPacketQueue`
   to obey normal timing response retries. No production transport/policy change.
3. `accuracy/topology-gate-1.log`: configuration JSON could not serialize gem5's
   `Unsigned` cache-line-size parameter. Converted that evidence field to `int`
   in the new accuracy configuration only.
4. Early protocol test compilation needed the repository's existing
   `-Wno-unused-parameter` convention for inline base-interface headers.

## Pins and limits

`simbricks/accuracy/final-pins.json` records source, library, model and native
binary SHA-256 hashes and baseline commits. Native SHA-256:
`f8208641d1b3d4efe1d9ac643505e9bf7d88eeebfbec3847206715adc1a5a71d`.

The native DMA test uses a directed real PCI peer, not the i40e executable;
i40e classification and CHI policy are separately exercised. These gates do NOT
establish full i40e→native→CHI guest integration, workload buffer alignment,
private-cache modeling accuracy, or a CacheLens accuracy verdict. No count fitting
or policy tuning was performed. See `simbricks/accuracy/INTERFACE.md` for launch
parameters, metadata format, trace bounds, and counter semantics.
