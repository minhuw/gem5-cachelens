# Independent Admin same-run plan — source-bound, not executed

Native base `37842903c66eaefcc6a7b7f4fefaad448488ec48` (parent da29). Historical
CLRX source/binary/witness remain immutable. New private test host adds only
`admin_only=False` plus a guarded Admin program; default CLRX behavior remains.
No production observer/PCI/DmaPort/CacheMemory/protocol/common Runner edits.
The old ELF771a3ca7... cannot express Admin-only actions and is forbidden as this
case's runtime binary. A private `-j2` incremental TEST build is necessary.

## Actual sequence provenance

Producer source `ae7d288dc85654591d5ce50c8656bd7a1f9a0960`, immutable git-show of
`i40e_bm/initialized_checkpoint_test.cc`, function `dma_trace_control_runner`,
lines559 onward. Supporting sources `i40e_adminq.cc`, `base/i40e_adminq_cmd.h`,
`test-control-intent.sh` and source/accuracy handoffs are copied/hash-bound under
`.pi/source-inputs` and `.pi/accuracy-input`; no source fixture was executed.
The accepted v5 historical15/15DMA+8control corpus is not this case's evidence.

Legal bounded subset: ring_len4 at wire0x10000, descriptor0 get_version opcode1,
descriptor1 driver_version opcode2 with RD|BUF0x1400, datalen16, external address
wire0x18000. Source fixture's RAM vector is zero-initialized; this host initializes
those exact16 bytes to zero through real Ruby DmaPort requests. The deliberately
unassigned0xffff descriptor, wrap/reset sequence and LAN paths are omitted.
Admin ring and payload memory are native PA=`wire+(1<<20)` using the actual
GenericPciHost mapping. Real timing PCI vendor/BAR/config path is reused.
Program ATQBAL, ATQBAH0, ATQLEN(enable|4), ATQT2 and poll actual ATQH2. Then settle,
read64B descriptors and16B RD source through Ruby. Assert actual DD|CMP flags,
noERR, retval0, original opcodes1/2, preserved driver RD|BUF/datalen16 and unchanged
zero RD source. Save all80 readback bytes for on/off equality. No fake completion,
PIO callback substitute, checkpoint, RX setup or packet injection.

get_version is one of21 actual named model decoders. driver_version is the real
hardware opcode0x0002 exercised with RD in v5; the model completes it via its
source-supported generic success branch, with producer `opcode_name=null`.
This does not invent a named decoder or use arbitrary unknown opcode contents.
RD bytes/real payload DMA can be observed, but no parser control-to-payload
reference exists; control attribution is only to the historical descriptor-fetch
`(op_seq,wire_req_id)` tuple, not automatically to the RD payload operation.

## Actual accuracy and parser inputs

Actual accuracy binary:
`/home/minhuw/cachelens/.pi/workspaces/i40e-control-intent/accuracy-ae7-v2/component-i40e/i40e_bm/simb_i40e_bm_accuracy`,
SHAa4a9241b627f5edfbff91396e32e3f71cc5b97daeea69ea3d950e7429a9e15d2.
Handoff `/home/minhuw/cachelens/.pi/artifacts/i40e-control-intent-accuracy-handoff-v1.json`
binds componentae7, SDK32ffdd340bb30d3ec25bbb27bdbe13bbc5b35515, maintained accuracy
profile and six libraries. All actual artifact hashes and16 consumed header
bytes agree with the old locked native SDK. Native recipe-reference6b,
source378+test delta and producerae7 are separate associations. No old721 fallback,
NIC/SDK rebuild or global dependency installation.

Parserc21cd518d4e35e0fd3481b79f53ab1c9ee25a4ab is copied via immutable git-show;
SHAe5c8b83b07401d8125af8a778cb36be4bd027b2955843e1687bc07a506ee3d6d.
It has not been imported or executed in prep. Optional register fields are not
required for these non-register opcodes; no guard is relaxed. No old47/80/64/v5
suite or corpus replay.

## Future one-case process contract

After the parent consumes BUILD exit and explicitly resumes this owner, verify
fresh ELF/test-host symbol, source/input hashes and accuracy receipt. Start ONE
managed ON task and immediately return its ID/PIDs/UTC/monotonic/deadline; no
child-only watch or autonomous OFF. Ordinary noncheckpoint accuracy Runner:

```
SIMBRICKS_I40E_DMA_TRACE=<fresh producer.jsonl> <actual ae7 accuracy binary>
  listen:<PCI>:<SHM>:sync=true:latency=1000:sync_interval=1000
  listen:<NET>:<SHM>:sync=true:latency=1000:sync_interval=1000
  0 029000000001 /dev/null
```

Native PCI and unchanged delivered `clrx_net_peer <NET> <SHM> 1000 1000` introduce
concurrently. The NET peer services synchronization while idle, receives no SEND,
and receives STOP only after native evidence collection; empty packet stream is
required. Fresh short socket paths, fresh native/producer epochs and exclusive
trace/readback files. Private hashed Boost1.74 copies are read-only Runner-child
LD_LIBRARY_PATH only. Native/NET/global environment is unchanged.

`admin_config.py --pci-socket ... --protocol mesi --native-trace ... --nic ...
--epoch ... --host-log ... --admin-output ... --final-tick 50000000` selects the
guarded host Admin mode and requires actual I40E_ADMIN_OBSERVE_PASS before fixed
final tick. No CLRX parser/frame file, no RX-ready or SEND phase.

Then analyze ONLY these new producer/native files with parserc21 and complete
endpoint ledgers. Match exact wire-ID lifecycle ordinal only after all producer
issue/complete and native accept/publish ledgers close; explicit external NIC and
producer/native epoch binding plus hash-bound verified map is mandatory. Values
may validate a key match, never select it. Producer op_seq, native instance,
control observation sequence and wire token remain distinct. For both actual
controls, verify exact descriptor_fetch_ref resolved to completed native fetch
instance; report unresolved controls exactly, not assumed100% coverage. Unknown,
retries and all setup/host lookups remain in their native denominator. Controls,
DMA operations, packet events and accepted lookups have separate counts.

After parent consumes actual ON exit/result and resumes, ONE OFF task is allowed
only if ON's real completions/full ledgers/control fetch resolution/data all pass.
Compare80readback bytes, actual ordered DMA issue/complete timestamps excluding
process-local pointer values, producer counters, native modeled stats/final ticks,
and equally empty packets. Native off has no trace or native identity claim.
No second ON analysis/rerun; preserve first failure and stop at ambiguity.
