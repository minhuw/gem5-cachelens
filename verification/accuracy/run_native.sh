#!/usr/bin/env bash
# Run only after the parent confirms the accuracy native build is finished.
# Does not build, launch guests, or contact a SimBricks peer.
set -euo pipefail
root=$(realpath "$(dirname "$0")/../..")
binary="$root/build/X86_CHI_ACCURACY/gem5.fast"
output=${1:?usage: bash verification/accuracy/run_native.sh NEW_OUTPUT_DIRECTORY}
test -x "$binary"
mkdir "$output" # Refuse to mix evidence with an existing run.
output=$(realpath "$output")
"$binary" -d "$output/configuration" "$root/configs/simbricks/i40e_accuracy.py" --mode configuration
for scenario in hit_update nic_cpu_nic eligible_untagged noneligible_dma; do
    "$binary" -d "$output/$scenario" "$root/tests/gem5/cachelens/configs/chi_dma_directed.py" --scenario "$scenario"
done
