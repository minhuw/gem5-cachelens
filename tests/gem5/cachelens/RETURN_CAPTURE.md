# LoadGeneratorPcap return capture

Configure the NIC's load-generator peer with:

```python
peer.return_dump = EtherDump(file="dedicated-nic-egress.pcap", maxlen=65535)
```

`LoadGeneratorPcap.return_dump` is `Param.EtherDump(NULL, ...)` by default.
The C++ `LoadGeneratorPcapParams::return_dump` is an `EtherDump *`; the
load-generator stores it as `EtherDump *const returnDump`. The existing
`processRxPkt(EthPacketPtr)` callback dumps once at entry, before the short-frame
and endpoint-MAC filters. Those filters, response counters, replay scheduling,
and checkpoint state are unchanged. The dump is output instrumentation and is
not part of deferred replay identity, so a pristine deferred checkpoint may be
restored with capture enabled. It uses the existing EtherDump output lifecycle;
it does not append/restore earlier capture records across checkpoint runs.

Use a dedicated EtherDump instance at this boundary. The processRxPkt hook and
host launch binding to its output establish NIC-to-peer direction. Comparing
PCAP bytes alone cannot establish direction for byte-preserving forwarding.
A production forwarding gate must fail/unavailable when this capture is disabled
or its output is missing; it must never substitute mixed-link or input captures.
Disabled capture remains valid legacy receive behavior.

EtherDump writes raw `EthPacketData::data[0:length]` with simulation timestamps
at microsecond precision. No FCS is added, inferred, or removed. Compare a logical
1472-byte input against the actual captured frame and length without silently
stripping four bytes. `EtherDump.maxlen` defaults to 96, which is insufficient:
zero is rejected at construction, and any received packet exceeding maxlen
causes a fatal error before dumping, rather than silent truncation. EtherDump's
native PCAP format and global header are unchanged.

## Focused native regression

After building a matching binary with `HAVE_PCAP=y`, run from the source root:

```sh
python3 tests/gem5/cachelens/run_pcap_return_tests.py --gem5 /path/to/matching/gem5.opt
```

This launches four small simulations using the existing EtherTapStub bound
directly to LoadGeneratorPcap. It injects an unexpected-MAC 1472-byte frame, a
7-byte frame, and one legacy-accepted 60-byte response. Enabled capture must
contain exactly these three unchanged frames in order, with native timestamps;
legacy response counters must remain one and replay sends zero. Disabled capture
must preserve the same filters/counters and produce no output. Two negative
cases require fatal errors for maxlen 96 and zero. These are callback-boundary
regressions, not proof of guest forwarding or production launch binding.

A bounded compile check, without linking a full simulator, is:

```sh
scons build/NULL/dev/net/load_generator_pcap.o build/NULL/dev/net/etherdump.o -j8
```

This generates parameter headers and compiles both changed native interfaces.
The runtime regression requires the subsequently integrated matching binary;
object compilation alone does not execute it. The existing replay regression
remains available as `tests/gem5/cachelens/run_pcap_replay_tests.py --gem5 ...`.
