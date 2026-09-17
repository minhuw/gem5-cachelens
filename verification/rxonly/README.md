# Native RX-only traffic verification

Run from the native gem5 source root:

```sh
python3 verification/rxonly/test_rss_pcap.py
python3 verification/rxonly/make_rss_pcap.py /tmp/rss.pcap
bash verification/rxonly/build.sh > verification/rxonly/build.log 2>&1
python3 verification/rxonly/run_native_tests.py --gem5 build/X86_CHI_RSS/gem5.fast
python3 verification/rxonly/run_ethernet_test.py --gem5 build/X86_CHI_RSS/gem5.fast
```

The build copies the existing `build_opts/X86_CHI` into `X86_CHI_RSS`, uses
`cachelens-gem5-build:2025`, the sibling SimBricks library, and eight jobs.
It never targets the live `build/X86_CHI/gem5.fast` binary.

## Workload / config contract

`configs/simbricks/i40e_rxonly.py` retains the platform construction from
`i40e_timing.py`: eight TimingSimple cores, 16GiB, NoCache, synchronized PCI
and Ethernet adapters (500ns defaults), existing 10Gbps / 200us EtherLink.
It accepts the timing config's platform arguments plus `--pcap` (required)
and `--packet-rate` (default 400000; range 1..800000).

The default generated input has 4096 **1472-byte complete Ethernet frames**
(no FCS), 64 flows, exactly 64 packets per flow. UDP source ports are
10000..10063, destination 11211; addresses 10.10.10.11 -> 10.10.10.10.
The 16-byte payload header at offset 42 is network-order `4sIII`:
`CLRX`, flow ID, per-flow sequence, global index. Both checksums are valid,
including nonzero UDP checksums. Input nanosecond timestamps describe offered
spacing; replay uses the configured constant packet rate, not PCAP timestamps.

Guest main must issue `m5_work_begin(0, 0)` after all four receiver workers are
ready and `m5_work_end(0, 0)` after the 20ms **m5_rpns** receive window and worker
join. Actual gem5 exit causes are `workbegin`, `workend`, and
`m5_exit instruction encountered` (`src/sim/pseudo_inst.cc`). The config resets
stats at begin, starts replay at the next tick, dumps stats at end, and resumes
until final guest exit. It never uses KVM, CPU switching, Simulation.run, or a
checkpoint. Deferred PCAP checkpoint save/restore explicitly fails; legacy
non-deferred replay and checkpoint semantics are unchanged.

## Markers and accounting

- `RXONLY_START {json}`: `roi_start_tick`, `packet_rate`, `ticks_per_second`.
- `RXONLY_END {json}`: `roi_start_tick`, `roi_end_tick`, `elapsed_ticks`,
  `ticks_per_second`, `packet_rate`, `sent`, `attempts`, `retries`, `eof`,
  `eof_tick`, `eof_count`.
- `RXONLY_EXIT {json}`: `tick`, `cause`.

Stats under `probe_sink.LoadGeneratorPcap`:
`sentPackets`, `attemptedPackets`, `backpressureRetries`, `eofTick`, `eofPackets`.
Attempts count actual peer send calls; retries count rejected calls;
`attempts = sent + retries`. A rejected frame remains pending until accepted:
`sent` counts unique accepted PCAP records, not attempted deliveries or NIC RX.
EOF is observed on the next record read, not necessarily after the final frame
has finished serialization/propagation. Its flag/tick/count are available to
Python; the guest's remaining receive window provides drain time. Telemetry
counts/ticks belong to the current stats epoch, not a cross-checkpoint ledger.

Stats under `i40e_network.Ethernet`:
`incomingPackets`, `incomingBytes`, `acceptedPackets`, `acceptedBytes`,
`droppedPackets`, `droppedBytes`, `outgoingPackets`, `outgoingBytes`.
Incoming = external NIC -> gem5, and incoming = accepted + dropped (packets
and bytes). Outgoing = gem5 -> external NIC. The incoming busy-peer path is
an explicit counted drop, not an unbounded queue. Outgoing retains existing
SimBricks publication semantics.

`rxonly-link.pcap` in gem5's output directory captures full frames in both
directions at **serialization completion**, before 200us propagation.
EtherDump timestamps have microsecond resolution. Existing EtherLink timing
is `ceil(frame.simLength * ticksPerByte + 1)` ticks; it adds no preamble, FCS,
or IFG. Do not interpret this as an added physical Ethernet overhead model.

The directed Ethernet test uses a two-frame same-tick injection peer, not a
new NIC/network simulator. The real adapter and real EtherLink must produce
incoming=2, accepted=1, dropped=1, outgoing=64, with matching byte counts.
Guest/model RSS behavior and end-to-end RX acceptance remain parent-owned.
