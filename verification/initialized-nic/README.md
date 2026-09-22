# Initialized i40e checkpoints (protocol/state v1)

Supported boundary: **after EAL/PMD probe, before ethdev configuration or data
queue allocation**. One synchronized PCI/Ethernet pair, one i40e process,
event queue zero, picosecond gem5 ticks, O3 CPUs (or the CPU-free test fixture).
All data queues must be disabled/unconfigured/empty, with zero programmed tails.
Admin work, DMA, MMIO completions, armed NIC timers/interrupts, cached data work,
and any packet history are rejected. NIC reset does not erase Runner history.
This is not support for initialized active rings or arbitrary live migration.

## Configuration and lifecycle

- Set `initialized_checkpoint=True` on both `SimBricksPci` and its
  `SimBricksEthernet`; set PCI's `checkpoint_ethernet` to that Ethernet object.
- Append `--initialized-checkpoint` to the model command line. Its initial wire
  tick must be zero. Multiple models in one process are not supported.
- Keep `LoadGeneratorPcap(deferred_start=True)` never started before saving.
- At the guest's post-EAL/pre-configuration exit, use ordinary `m5.checkpoint()`
  with a **new destination directory**. No parent orchestration is installed.
- Restore with the same object identities/configuration and freshly started
  peers, sockets and shared-memory rings. Both wire epochs start at zero. The
  saved device epoch preserves absolute device time, PTP, and future events.
  While frozen, device time is latched at the callback-free fence; only transport
  promises advance. The final epoch must be at least that latched device time,
  so transport lookahead cannot cause a backwards device clock on restore.
  The saved MAC is authoritative even if a fresh process has a different CLI MAC.
- Configure geometry-specific RX/TX queues after restore, then call
  `startReplay()`. EAL/admin/PMD probe must not run again.
- A saved source remains frozen and cannot resume. On any barrier failure,
  terminate the graph; do not retry/resume that source in place.

The global drain is maintained while **low-level** `_m5.event.simulate()` drives
both transports. Each host producer is gated before event advancement. FIFO
FREEZE markers and matching ACKs fence both channels; NIC producers are gated
at a callback-free Runner boundary. Ordinary sync continues until both ACKs.
Both peers scan even future ring slots before export, rejecting late semantic
messages instead of dropping them. Socket state transfer happens only after
that fence. Bootstrap completes both legacy introductions before READY/BOOT;
a per-process nonce verifies that both adapters connect to the same NIC.

Barrier advancement is bounded by 1 billion ticks (1 ms), in 1-million-tick
steps, and a 30-second wall deadline. Blocking adapter waits also check the wall
clock. Bootstrap/state socket transfers and initial introductions are bounded;
the single-NIC model uses a 30-second process alarm around the legacy blocking
introduction helper. No frozen-tick handshake or drain bypass is used.

State is bounded, versioned, explicitly little-endian and pointer-free. It
preserves registers, idle admin cursor/geometry, HMC mappings, RSS key/LUT,
interrupt enables and full PTP accumulator/offset/adjustment state. Derived RSS
cache is rebuilt, without replaying MMIO or enabling queues. Transport geometry,
PCI introduction/capabilities, token and final device epoch must match.

The NIC sidecar (`<object>.nic-state`) and `m5.cpt` have SHA-256 digests in
`initialized-nic.json`; C++ independently validates token, epoch, length and body
checksum. The entire checkpoint is written/fsynced in a sibling staging directory
and published with Linux `renameat2(RENAME_NOREPLACE)`, then parent fsync. Missing,
corrupt or mismatched state never falls back to reset/reprobe. A crash may leave
an unpublished `.initialized-nic-*` staging directory, which is not a restore path.

## Reproducible focused checks

All builds must live outside the source checkouts and `gem5/work`.

```sh
bash /path/to/simbricks/accuracy/test-checkpoint.sh /tmp/new-simbricks-build
SIMBRICKS_LIB_DIR=/tmp/new-simbricks-build/lib \
  bash /path/to/component-i40e/i40e_bm/test-initialized-checkpoint.sh /tmp/new-i40e-build
```

The Runner check covers codecs, gates, DMA/timer rejection and absolute/wire event
translation. The real i40e process test performs admin DMA, freezes both real
transports, saves initialized state, exits the original peer, and restores fresh
peers into all seven rings (64–4096). It checks retained admin/MAC/HMC/RSS/PTP,
fresh RX/TX queue setup, actual RX payload DMA, delayed MSI-X, and rejection of pending DMA, armed timers,
configured queues, traffic history, late future messages and invalid state.

For the gem5 plumbing probe, copy sources to an external build directory, enable
`USE_SIMBRICKS=y` and `USE_TEST_OBJECTS=y` in a minimal build option, and build
against the matching SimBricks include/library directories. Then:

```sh
python3 verification/initialized-nic/run.py \
  --gem5 /tmp/gem5-build/build/NIC_PROBE/gem5.opt \
  --nic /tmp/new-i40e-build/simb_i40e_bm --out /tmp/new-gem5-probe
python3 verification/o3/test_drained_wakeup.py -v
```

The gem5 fixture uses actual MMIO/transports, global drain, event advancement,
serialization, fresh-process restore and deferred PCAP continuation. It also
checks corruption/identity/geometry rejection, PCAP identity and lifetime rules,
no overwrite, frozen-source resume rejection, and stalled-peer initialization
and barrier deadlines. It deliberately has **no CPU, Linux or DPDK**. The O3
wakeup regression is separate. Full Linux post-EAL checkpoints and 500 ms sweeps
remain integration validation, not claims established by these focused probes.

### Drained timing-interrupt fence regression

A latched `DrainState::Drained` is not evidence that no new work was produced
while the FIFO barrier advanced. `PacketQueue::schedSendTiming` now invalidates
a sticky native packet-work fence whenever a timing request, response or snoop
is queued while globally drained. The initialized checkpoint path verifies it
before the barrier, after its final event step, and after memory writeback,
**before** finalizing the NIC epoch or serializing. Verification neither calls
`drain()` again nor resumes CPUs nor changes drain flags. It conservatively
rejects even packets delivered within the barrier. It is not a general
quiescence proof for work outside PacketQueue; device-specific validators
still apply.

Build a separate probe configuration with `USE_SIMBRICKS=y`,
`USE_TEST_OBJECTS=y`, `BUILD_ISA=y`, and `USE_X86_ISA=y`, then run:

```sh
python3 verification/initialized-nic/run.py --interrupts \
  --gem5 /tmp/irq-probe/build/NIC_IRQ_PROBE/gem5.opt \
  --nic /tmp/i40e-probe/simb_i40e_bm --out /tmp/new-irq-results
```

In addition to the existing cases, this runs a timing-mode idle save/restore,
a real PIT expiration through the real IOAPIC/IntRequestPort/PacketQueue during
the final barrier step (interrupt delivery delayed beyond that step), a delivery
within the step, and an IOAPIC pulse during memory writeback. All three active
cases must reject without publishing a checkpoint. Only the interrupt endpoint
is a fixture; there is still no CPU-present Linux/DPDK validation claim.
