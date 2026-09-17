#!/bin/bash
set -euo pipefail
N=$(cd "$(dirname "$0")/../.." && pwd)
W=$(dirname "$N")
P=$(cd "$N/../../../../.." && pwd)
cp "$N/build_opts/X86_CHI" "$N/build_opts/X86_CHI_RSS"
# Never target X86_CHI: another probe may be executing that binary.
docker run --rm --name cachelens-gem5-rss-build --user "$(id -u):$(id -g)" \
    -v "$P:$P" -w "$N" -e CCFLAGS_EXTRA="-I$W/simbricks/lib" \
    -e LIBRARY_PATH="$W/simbricks/lib" cachelens-gem5-build:2025 \
    bash -lc 'scons build/X86_CHI_RSS/gem5.fast -j8'
