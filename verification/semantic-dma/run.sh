#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
root=$(realpath "$(dirname "$0")/../../..")
gem=$root/cachelens-gem5
peer=$root/simbricks/accuracy/dma-peer
out=${1:-$gem/accuracy/native-tests}
mkdir "$out" # Never overwrite evidence from a previous attempt.
cc -std=gnu11 -I"$root/simbricks/lib" "$root/simbricks/accuracy/dma-peer.c" \
  "$root/simbricks/lib/libnicif.a" "$root/simbricks/lib/libnetwork.a" \
  "$root/simbricks/lib/libpcie.a" "$root/simbricks/lib/libbase.a" -o "$peer"
for mode in legacy kind direction queue truncated atomic timing; do
  d=$out/$mode
  mkdir -p "$d"
  sock=$root/simbricks/accuracy/test.sock
  shm=$root/simbricks/accuracy/test.shm
  rm -f "$sock" "$shm"
  "$peer" "$sock" "$shm" "$mode" > "$d/peer.log" 2>&1 & pid=$!
  trap 'kill "$pid" 2>/dev/null || true' EXIT
  for ((i=0; i<1000; ++i)); do [[ -S "$sock" ]] && break; sleep .01; done
  # Container maps the accuracy root to the same path: UNIX sockets stay valid.
  mem=atomic; [[ $mode == timing ]] && mem=timing
  set +e
  timeout 120 docker run --rm --user "$(id -u):$(id -g)" -v "$root:$root" -w "$gem" \
    cachelens-gem5-build:2025 build/X86_CHI_ACCURACY/gem5.fast -d "$d" \
    verification/semantic-dma/dma-test.py "$sock" "$mem" > "$d/gem5.log" 2>&1
  rc=$?
  set -e
  if [[ $mode == atomic || $mode == timing ]]; then
    test "$rc" = 0
    wait "$pid"
    grep -q DMA_MEMORY_FLAGS_OK "$d/gem5.log"
    grep -q DMA_PEER_COMPLETIONS_OK "$d/peer.log"
    python3 - "$d" <<'PY'
import csv, pathlib, sys
p=pathlib.Path(sys.argv[1])
rows=list(csv.DictReader((p/'dma.csv').open()))
assert len(rows)==5
assert [int(r['kind']) for r in rows]==[1,2,2,0,0]
assert sum(int(r['length']) for r in rows if r['kind']=='2')==2944
assert [int(r['packet_id']) for r in rows[1:3]]==[42,43]
assert [int(r['physical_address']) % 64 for r in rows[1:3]]==[0,56]
s=(p/'stats.txt').read_text().splitlines()
values={line.split()[0]:line.split()[1] for line in s if line.startswith('system.dev.semanticDma.')}
for key,expected in {'descReadOps::7':1,'descReadBytes::7':160,'payloadWriteOps::7':2,
                     'payloadWriteBytes::7':2944,'untaggedReadBytes':16,'untaggedWriteBytes':16}.items():
    assert float(values['system.dev.semanticDma.'+key])==expected, (key,values)
PY
  else
    test "$rc" != 0 && test "$rc" != 124
    if [[ $mode == legacy ]]; then grep -q 'handshake mismatch' "$d/gem5.log"
    else grep -q 'Invalid PCI semantic DMA metadata' "$d/gem5.log"; fi
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  fi
  trap - EXIT
  echo "NATIVE_DMA_TEST_OK $mode"
done
