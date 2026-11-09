# ARM partition timing directed test

`arm_split_timing.py --mode fenced --partition-timing --probe` runs four O3
cores with CHI+DVM and GICv2 for 100 us. `thread` uses the hnf1 partition by
default; `native` also supports the wire model, with native event ordering.
`--deactivate-at 80001000` stops the bridge while the probe's first GIC post
is pending, then continues to the same endpoint and checks its exact deadline.

`arm_split_timing.S` is the source of the committed sparse word image
`arm_split_timing_image.py`. Run `python3 -B build_arm_split_timing.py` with the
host `aarch64-linux-gnu-gcc` and `aarch64-linux-gnu-objcopy` tools to rebuild it.
The script links `.text` at zero without a build ID, extracts it, and records
its source SHA256. The configuration checks that digest and embeds the words
in a single-segment AArch64 ELF; gem5's workload adds the platform DRAM base.
No assembler or external guest assets are needed to run the test.

The guest runs at NS EL1 with a 2 GHz counter (one count = 0.5 ns). It checks
three common-CVAL interrupt/IAR/EOI/reprogram rounds on every core, zero and
negative TVAL, a CVAL one count ahead, mask/disable across expiry, SGIs to WFI
targets, remote SEV/WFE, local event-stream WFE, PMU cycle overflow, and a
VAE1IS + DSB ISH page-table change while peers repeatedly use the old mapping.
The final records require exactly eight timer interrupts and one PMU interrupt
per core, one SGI on each remote core, and the new mapping on all cores.
`guest.json` includes the per-core event order and CNTVCT timestamps.

`ArmPartitionTimingProbe` runs after the guest parks with IRQs masked. It
checks real GIC input and CPU raw interrupt state, including the first post,
clear, and re-post while the first post is still in flight. Same-generation
posts must deduplicate; the second generation must survive. It also changes
timer control one tick before, exactly at (before the timer event), and one
tick after expiry for both masking and disabling. It observes PPI delivery
at expiry + 1000 ticks and clear delivery at issue + 1000 ticks. An in-flight
GIC assert still lands at its original 10000-tick latency. The observer emits
`ARM_WIRE` issue/land records with endpoint, issue and delivery ticks.

Native/on is supported for serial execution with the same crossing latencies.
Native Minimum_Pri events retain gem5's same-tick event-bin ordering. Fenced/on
and thread/on instead share canonical `(tick, domain, endpoint, sequence)`
ordering, including re-entrant work. Fenced/on is the exact reference for
thread/on; native/on is not a cross-mode byte-equivalence reference.

The remote TLBI operation is copied and scheduled before the source's DVM
request (`src/arch/arm/isa/templates/misc64.isa` DvmInitiateAcc and
`src/arch/arm/isa/insts/data64.isa` tlbiCode/dvmCode). Both TLBI and DSB use
DvmCompleteAcc. Its fail-closed check rejects completion while any remote
operation from that source remains pending. The TLBI is NonSpeculative and
SerializeAfter; a following DSB therefore cannot send its sync before the
non-sync instruction has passed that check. CHI additionally blocks sync on
its pending non-sync list (`CHI-cache-actions.sm`, Send_DvmTlbi,
Try_Send_DvmSync, Try_Send_Pending_DvmSync). The remote LSQ flushes stale
translations on TLBI_EXT_SYNC before returning TLBI_EXT_SYNC_COMP. This means
neither that sync nor the source DSB can complete before these deferred
invalidations. Broadcast with the wire model fails closed outside AArch64
DVM; a longer wire that outruns protocol completion also fails closed.
