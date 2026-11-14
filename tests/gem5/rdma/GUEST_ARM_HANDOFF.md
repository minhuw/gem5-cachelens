# Guest-owned ARM prerequisites: actionable handoff

This is a source-backed portability handoff, not a report of an ARM guest
failure. No Linux/provider/app source is modified by the native worktree.
Inspected provider: rdma-core 60.0, `providers/vmw_pvrdma/` from the guest
packaging worker's `rdma-arm-prerequisites-v2/rdma-source/rdma-core-60.0` export.
Read-only snapshots and hashes are retained under native `.pi/artifacts/`.

## Receive publication and MMIO

1. `providers/vmw_pvrdma/qp.c`, `pvrdma_post_recv`, lines 614–635 in that
   export: writes `wqe_hdr->wr_id`, `num_sge`, and SGE fields, calls
   `pvrdma_idx_ring_inc(&qp->rq.ring_state->prod_tail, ...)`, then calls
   `pvrdma_write_uar_qp(... PVRDMA_UAR_QP_RECV ...)`. There is no
   `udma_to_device_barrier()` before producer publication or the doorbell.
   SEND in `pvrdma_post_send` already has the two barriers at lines 539/551.
   Minimal receive change: copy the SEND publication pattern: device-release
   barrier after each completed WQE/SGE and before advancing RQ producer, and
   another barrier after the final producer write and before the receive
   doorbell. Preserve existing accepted-WR/error semantics.
2. `providers/vmw_pvrdma/pvrdma.h`, `pvrdma_write_uar_qp`,
   `pvrdma_write_uar_cq`, `pvrdma_write_uar_srq`, lines 231–244: each uses a
   plain nonvolatile `*(__le32 *)(uar + offset) = htole32(value)` store.
   Minimal change: include the existing rdma-core `<util/mmio.h>` and use
   `mmio_write32_le(uar + offset, htole32(value))` for the 32-bit UAR writes.
   This accessor guarantees a real, non-speculated MMIO access; it is relaxed
   with respect to DMA, so it does NOT replace the publication barriers above.
   Check the actual UAR mmap memory type: if WC, obey the mmio.h WC bracketing
   contract; do not assume UC without checking driver mapping.
3. `providers/vmw_pvrdma/pvrdma_ring.h`, comment above `struct pvrdma_ring`,
   lines 55–58, explicitly says "PVRDMA is x86 only" and relies on ordinary
   aligned 32-bit accesses being atomic. `pvrdma_idx_ring_inc` is a plain
   load/add/masked store. Review shared producer/consumer accesses for a
   single actual aligned load/store/compiler access discipline on AArch64;
   do not convert hardware-shared fields into arbitrary C atomic ABI layouts.
   Both intended guest targets are little endian; big-endian ARM is outside
   this slice. Preserve 32-bit field size/alignment and native ABI layout.
4. CQ acquisition already calls `udma_from_device_barrier()` after observing
   nonempty producer state in `cq.c:112`. `util/udma_barrier.h:95–96,133–134`
   supplies AArch64 `dmb oshst`/`dmb oshld`. Retain these; inspect CQ consumer
   publication before its MMIO poll/arm doorbell under the same ordering rule.

Classification: the missing receive barriers/plain UAR stores are verified
SOURCE facts and a concrete ARM PORTABILITY requirement. This work has not
observed a lost/stale ARM WQE at runtime, and does not label it a reproduced
ARM guest bug. The native public device test uses ordered tester MMIO and
cannot validate a guest CPU's weak-memory ordering. In contrast, shared INTx
line loss in the old native host is directly exercised by the new host test.

## Companion contract versus model restrictions

* Linux maintained patch in the parent repository:
  `gem5/buildroot/external/patches/linux/0003-rdma-pvrdma-pair-cachelens-e1000.patch`.
  Its `pvrdma_pci_probe`/`pvrdma_netdevice_event_handle` use
  `pci_get_slot(pdev->bus, PCI_DEVFN(PCI_SLOT(pdev->devfn), 0))`.
  `pvrdma_is_supported_netdev` accepts VMXNET3 or Intel E1000 8086:1075.
  E1000 must have probed and supply `pci_get_drvdata()` as a real net_device.
  Same bus/slot function0 companion plus function1 PVRDMA is the CURRENT
  DRIVER/BOARD CONTRACT, not a hardcoded requirement of the native transport.
  Supporting another companion or association policy requires a guest-driver
  change; the native model does not route payload through the companion.
* The same patch changes Linux
  `drivers/infiniband/hw/vmw_pvrdma/Kconfig` to depend on
  `NETDEVICES && ETHERNET && PCI && INET` and `VMXNET3 || E1000`.
  There is NO X86 Kconfig dependency in this inspected source/patch. Upstream
  unpatched Kconfig requires VMXNET3 instead. The inspected provider's
  `providers/vmw_pvrdma/CMakeLists.txt` unconditionally invokes
  `rdma_provider(vmw_pvrdma cq.c pvrdma_main.c qp.c verbs.c)` and has no ISA
  gate; the ring header's x86 assumption is not an architecture build guard.
  Guest owner must enable INFINIBAND/USER_VERBS/VMWARE_PVRDMA/E1000 and ship the
  real provider. Source/build eligibility is not driver-boot proof.
* Native `src/dev/rdma/pvrdma.hh`, `detail::createBind`: MTU exactly 1024,
  RoCEv2 GID type, index < 8, and no IPv4-mapped GIDs. These ARE current model
  restrictions. Linux `pvrdma_add_gid_at_index` in `pvrdma_main.c` already
  submits 1024 and the netdev-associated GID. Bring the companion up with an
  IPv6 address; select a real nonzero verbs GID. Resolve the peer destination
  MAC through actual normal network neighbor setup or static neighbor entry.
  Exchanging QPN/GID/PSN alone does not configure the normal interface/AH MAC.
* `board.rdmas[0].interface` is a separate EtherInt and needs an actual remote
  native PVRDMA peer attachment. E1000 links provide neighbor/control traffic;
  they do not carry this model's RDMA payload. A normal IPv4 TCP control
  interface is required by the guest app and can use a configured companion.
* `companion_mac_word_swap=True` in CacheLens boards compensates for this
  E1000 model's EEPROM word ordering. It is an E1000/board compatibility
  policy, not a RoCE requirement. Use the running netdev's actual MAC.
* Current supported verbs slice: plain RC SEND/RECV, one SGE, zero inline,
  path MTU1024, power-of-two SQ/RQ <=256, CQ<=1024 and CQ polling. CQ arm
  writes record state but do not deliver notification interrupts. MR access
  permission bits do not imply RDMA read/write/atomic transport support.

## Evidence boundary and upcoming invocation

The native ARM attachment/host tests establish function decode, BAR/DMA maps,
shared interrupt level aggregation and coherent requestor wiring. They are NOT
Linux driver probe, real provider ordering, ARM GIC delivery to a guest CPU,
or Linux remote completion evidence. Those remain the coordinated runtime gate.

Application default work/thread IDs remain 0. The guest owner reports work-ID/thread-ID CLI follow-up
`ba1885dc2759b43aa8aada53cf2e10a8e9862242` ready, with legal defaults0 and
explicit decimal m5 arguments. Its host argument checks are not real pseudoop
evidence; no native test was restarted for these flags. Future host metadata selects receiver1,
sender2, thread0; receiver SYSTEM owns the primary interval independently of
IDs. Configure receiver exit_on_work_items true and sender false, because this
native revision's global work-event object exposes no System source identity.
Keep simulation alive after receiver workend for both cleanup/result gates.

## Disabled userspace neighbor resolver: exact RC setup path

Guest packaging reports `ENABLE_RESOLVE_NEIGH=0`, confirmed in its inspected
CMakeCache. Preserve this runtime constraint; userspace/provider compilation
is not neighbor resolution evidence. rdma-core root CMakeLists.txt:494–515
selects `NL_KIND=0` and netlink fixups when disabled. The provider
`verbs.c:189–236` `pvrdma_create_ah` can call
`ibv_resolve_eth_l2_from_gid` (`libibverbs/verbs.c:1091`); that userspace path
must not be assumed operational with these packages. It also restricts
explicit AH creation to link-local/multicast/IPv4-mapped destination GIDs.
The RC guest application does not create a standalone AH: it uses
`pvrdma_modify_qp` (`providers/vmw_pvrdma/qp.c:350`), which delegates to
`ibv_cmd_modify_qp` and the kernel uverbs path.

For this plain connected-RC path, inspected Linux 6.18.2
`drivers/infiniband/core/verbs.c::_ib_modify_qp` (1767–1801) fills SGID attrs
and calls `ib_resolve_eth_dmac` when userspace supplies IB_QP_AV for a connected
RoCE QP. Its `ib_resolve_unicast_gid_dmac` (752–775) calls
`rdma_addr_find_l2_eth_by_grh`; `drivers/infiniband/core/addr.c:802–836`
converts GIDs to IPs and calls `rdma_resolve_ip(... resolve_neigh=true ...)`.
The address path uses the kernel route/neighbor table; `addr.c:330–344` requires
NUD_VALID, otherwise it starts neighbor discovery. Then
`drivers/infiniband/hw/vmw_pvrdma/pvrdma_qp.c::pvrdma_modify_qp` around 612
copies the resolved address vector using `rdma_ah_attr_to_pvrdma`, whose
`pvrdma_misc.c:285` copies `src->roce.dmac` to native command destinationMac.
The native model matches this actual MAC; it cannot infer it from a QPN alone.

Minimal deterministic guest setup BEFORE RTR (on both peers, using actual
interface names, chosen IPv6 addresses and runtime netdev MACs): bring the
paired E1000 up; assign distinct IPv6 addresses; ensure a source/interface-
correct IPv6 route to the remote GID; install a permanent IPv6 neighbor entry
for that remote address on the companion. For example, with placeholders:

```
ip link set dev <companion> up
ip -6 addr add <local-ipv6>/64 dev <companion> nodad
ip -6 route replace <remote-ipv6>/128 dev <companion> src <local-ipv6>
ip -6 neigh replace <remote-ipv6> lladdr <peer-actual-mac> nud permanent dev <companion>
```

Query the resulting verbs RoCEv2 SGID/index and use exactly that local GID;
check the route/neighbor entries and both return codes. If the packaged `ip`
utility lacks these operations, supplying it is a guest prerequisite.
Static kernel neighbor setup is expected to satisfy this RC kernel resolution
path despite disabled userspace resolution, but remains an UNVERIFIED guest
runtime assumption until actual RTR/remote SEND succeeds. It does not repair
standalone provider create_ah resolution generally. Normal IPv4 TCP control
setup remains separate. The packaging manifest still truthfully records ARM
`kernel_missing=true`, `runtime_ready=false`; native source enablement does
not change those guest artifact claims or supply a replacement kernel.

## Can i40e function0 replace the E1000 companion?

Checked the ACTUAL PATCHED Linux tree (not just upstream):
`/home/minhuw/cachelens/.pi/workspaces/ddio-linux-image-impl/repo/.pi/workspaces/linux-x86-build-v2/kernel-source/linux-6.18.2`,
including its vmw_pvrdma Kconfig and main source, against the maintained 0003
patch. Patched `pvrdma_is_supported_netdev` is at 768–774; probe same-slot
lookup/identity/assignment is at 953–976, and notifier comparison at 707–716.
Read-only source snapshots/hashes are preserved with this slice's artifacts.

Not with the current guest driver contract. This is a concrete ABI mismatch,
not a native RDMA payload dependency:

* Inspected Linux 6.18.2 `drivers/net/ethernet/intel/e1000/e1000_main.c:958`
  sets `pci_set_drvdata(pdev, netdev)`. This matches PVRDMA's cast/assignment.
* `drivers/net/ethernet/intel/i40e/i40e_main.c::i40e_probe`, line 15783, sets
  `pci_set_drvdata(pdev, pf)` with a `struct i40e_pf *`; its remove path at
  16137 reads that PF back. The net_device is owned by a VSI
  (`pf->vsi[pf->lan_vsi]->netdev` / main-VSI helper), not the PCI drvdata.
* patched `pvrdma_main.c::pvrdma_pci_probe` at 953–976 gets same-slot function0,
  checks accepted device identity, then assigns `pci_get_drvdata(pdev_net)`
  DIRECTLY to `dev->netdev` and calls `dev_hold`. Merely adding i40e's PCI ID
  would misinterpret a PF pointer as a net_device. That is invalid.
* Its `pvrdma_netdevice_event_handle` NETDEV_REGISTER path at 707–716 also
  compares `pci_get_drvdata(pdev_net) == ndev`; i40e can never satisfy that
  comparison. Both initial association and re-registration need a guest fix.

A possible guest-owned extension is a reference-safe generic netdev lookup by
PCI device parent for this same-slot function0 (select the physical main
interface, exclude representors/VFs, handle probe order, unregister, and device
removal), and the same association rule in the netdevice notifier. An i40e-
specific PF/VSI accessor would instead introduce a private-driver dependency.
Do not pass the PF pointer through or call it a compatible netdev ABI. Update
accepted identity/Kconfig only together with this association change.

The native `Pvrdma` transport does not inspect a companion PciDevice or require
IGbE. It gets its MAC through its own registers/parameter and accepts Ethernet
frames on its own EtherInt. Current `network.py` constructs IGbE_e1000 and
`board.py` attaches it as same-slot function0 with multifunction bit; this is
the tested native fixture and matches the existing guest patch. Changing to an
i40e companion additionally needs an explicit native i40e/function0 attachment,
multifunction advertisement, independent coherent DMA requestors, correct
shared IRQ routing, and correct actual MAC/register-byte policy (do NOT carry
over `companion_mac_word_swap=True` blindly). Its normal netdev supplies GID,
neighbor and IPv4 control setup only; PVRDMA payload still uses its separate
native EtherInt. Such a replacement is possible in principle after those
changes, but is neither implemented nor proven by this slice. Existing
DPDK/Linux i40e sources and live fixtures remain unchanged.
