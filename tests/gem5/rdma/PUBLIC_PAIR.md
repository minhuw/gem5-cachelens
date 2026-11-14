# Public PVRDMA remote SEND/RECV proof and guest interface

The `public-pair` and `timing-public-pair` modes in
`configs/pvrdma_runtime.py` attach two distinct native `Pvrdma` PCI devices
through a normal `EtherLink`. They configure PCI BARs/bus mastering, DSR,
user context, PD, CQ, RC QP, MR, GID binding, and RESET→INIT→RTR→RTS through
public memory/MMIO commands. They never install private device table entries
or inject ACKs. Identical QPN numbers on different devices are separate QPs.

Each mode transfers eighteen 1537-byte plain SENDs (two RoCEv2 segments per
message), polls actual SEND and RECV CQEs, checks distinct local work-request
cookies, reclaims CQs, and reposts the same receiver payload buffer. Eight-entry
SQ/RQ/CQ rings cross slot and phase wrap. The receiver payload is initialized
before transfers and read once after the final SEND/RECV completion and a
quiescence interval. Between these points only WQE/ring/CQE metadata is touched
by the tester. Passive CommMonitor observation rejects receiver payload reads,
checks ordinary coherent request flags, and totals exactly 27666 receiver
payload DMA bytes. This is device functional coverage, **not Linux guest
end-to-end evidence or a throughput measurement**.

Run with a matching binary built with USE_TEST_OBJECTS:

```
build/X86/gem5.opt -d m5out/public-pair \
  tests/gem5/rdma/configs/pvrdma_runtime.py --mode public-pair
build/X86/gem5.opt -d m5out/timing-public-pair \
  tests/gem5/rdma/configs/pvrdma_runtime.py --mode timing-public-pair
```

## Guest-owned setup requirements

* Apply the maintained Linux companion patch
  `gem5/buildroot/external/patches/linux/0003-rdma-pvrdma-pair-cachelens-e1000.patch`
  from the parent repository. PVRDMA (15ad:0820) must be function 1 beside a
  function-0 Intel 82547GI E1000 (8086:1075) on the **same PCI bus and slot**.
  VMXNET3 is also accepted by that patch. E1000 must probe and supply a real
  net_device; the RDMA driver takes its reference and uses it for GID/network
  association. Linux's patched Kconfig requires NETDEVICES, ETHERNET, PCI, INET,
  and VMXNET3 or E1000; it does not impose an X86 dependency.
* Bring the companion interface up and assign distinct IPv6 addresses to the
  two endpoints. Choose an existing RoCEv2 GID/index from the real verbs GID
  table (indices 0–7). IPv4-mapped GIDs are rejected by the native model.
  Linux's `pvrdma_add_gid_at_index` issues CREATE_BIND with MTU 1024 and the
  16-byte GID. No application-private GID registration API is needed.
* Exchange actual QPN, IPv6 GID and initial SQ PSN out of band. Set each remote
  destination QPN, destination GID and receive PSN from its peer. Select the
  local source GID index, port 1, pkey index 0, and path MTU IBV_MTU_1024.
  AH creation/modify needs the peer companion's actual destination MAC.
  Link the companions for neighbor discovery/control traffic or install static
  IPv6 neighbor entries. The existing x86 board sets
  `companion_mac_word_swap=True` to match the modeled E1000 EEPROM word order;
  obtain MACs from the running netdev, not a guessed byte interpretation of
  `hardware_address`. Give each board distinct companion/native MACs; default
  board construction otherwise repeats the same MAC.
* The RDMA payload travels over `board.rdmas[0].interface`, which must connect
  to the other native PVRDMA interface through its own EtherLink. Connecting
  only the E1000 companion cannot transport RDMA payload. The current single
  board `cachelens-fs.py --enable-pvrdma` does not attach a remote endpoint.
* Supported slice: RC plain SEND/RECV; one SGE per WQE, no inline payload,
  power-of-two SQ/RQ depths up to 256, CQ depth up to 1024; 1024-byte path MTU;
  finite ACK/sequence/RNR retries; CQ polling. CQ arm doorbells store the
  requested mode, but notification delivery and RECV completion interrupts
  are not implemented. Control
  responses use INTx. Unsupported verbs (including SEND_WITH_IMM, RDMA
  read/write/atomics, UD and SRQ) must not be requested. MR access permissions
  advertised by the ABI do not imply transport implementation of those verbs.

## Passive identity and ordering

`ReceiveWqeHeader` is 16 bytes: 64-bit workRequestId, 32-bit numSge and
reservedLength. `SendWqeHeader` is 80 bytes and adds opcode/sendFlags and
operation fields. A 16-byte SGE supplies 64-bit address, 32-bit length and
lkey. All control, WQE, SGE and CQE scalars are little endian; RoCE headers
are encoded in network byte order. The inspected rdma-core 60.0 vmw_pvrdma provider copies application
`wr->wr_id` directly to SQ/RQ WQE `wr_id` (qp.c:487,615) and CQE `wr_id`
directly to `wc->wr_id` (cq.c:121). Thus the hardware WQE contains the
application's local cookie in this provider. It still carries no sender
sequence semantics and never enters the wire frame.

The model retains QP handle/generation/QPN, RQ consumer index, decoded local
cookie/SGE, accepted PSN/opcode/segment length, GIDs/MACs, and translated MR
DMA chunks. SEND wire frames contain destination QPN, PSN and route fields;
**neither sender nor receiver work-request ID appears on the wire**. Sender
identity therefore joins passively observed transport order/PSN with receiver
RQ/SGE selection and DMA destination. Local slot/generation cookies identify
buffer reuse only. No extra payload DMA is necessary for this join.

The provider must publish WQE/SGE bytes before producer index and doorbell
(release/device ordering), and observe CQ producer before CQE contents
(acquire ordering). Payload DMA finishes before receiver CQ publication and
ACK; sender completion follows remote ACK. The functional tester uses ordered
atomic MMIO writes; timing mode exercises asynchronous *device* DMA, not an ARM
CPU memory-ordering test. Existing x86 `get_dma_ports`/`get_ddio_dma_ports`
include native PVRDMA as a distinct coherent network requestor. All device DMA
uses that requestor, including control/ring metadata: eligibility is not a
payload-byte counter. The public test uses SystemXBar/SimpleMemory and does not
claim a CacheLens cache geometry or DDIO measurement.

## ARM attachment follow-up

The next native slice adds opt-in ARM same-slot function1 attachment and
shared INTx aggregation, preserving default x86 and PVRDMA-disabled routing.
See `ARM_PLATFORM.md` for its focused configuration/host proof boundary and
`GUEST_ARM_HANDOFF.md` for exact kernel/provider prerequisites. Native source
attachment support does not supply an ARM RDMA kernel, provider ordering fix,
normal network setup, or Linux driver/GIC runtime proof.
