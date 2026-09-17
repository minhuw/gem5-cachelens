# CacheLens integration

## Promotion provenance (new documentation)

Local source promotion for the 40g-mesi project; baseline `441786151ed90924b373d2f520ac870891b20b6e`.
Source: parent project `gem5/work/sources/simbricks-accuracy/cachelens-gem5`.
Branch: `cachelens-integration/native`. Implementation, build definitions, fixtures, and imported
documentation are preserved byte-for-byte except the explicitly requested gem5
configuration exception: `build_opts/X86_CHI` is restored to baseline so ordinary
builds do not require SimBricks; `X86_CHI_ACCURACY` remains opt-in and unchanged.
The parent owns the new MESI3 SimBricks defconfig. No remote-GDB changes.
Upstream README, copyright notices, and licenses remain unchanged.

Native SimBricks PCI/Ethernet adapters, semantic DMA checks, and prototype
configuration/verification inputs. Host checks:
`python3 verification/rxonly/test_rss_pcap.py` and
`python3 verification/accuracy/test_config_contract.py`.
The obsolete `accuracy/build.sh` is intentionally omitted. The parent project
owns the new portable build entry point. Existing verification shell scripts
are historical fixtures, not portable build entry points: some assume the old
`cachelens-gem5` sibling name, Docker image, or project/work directory layout.

Imported verification reports describe historical runs, not validation of this
checkout. Generated binaries, logs, run inputs, and results are not promoted.
The parent project's `gem5/results/source-promotion-40g-v1` records baseline
pins, tracked patches, inventories, source/promoted SHA-256 hashes, omissions,
and fresh checks. No full guest or MESI workload validation is claimed here.
