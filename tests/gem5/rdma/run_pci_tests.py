# SPDX-License-Identifier: BSD-3-Clause
"""Check real shared INTx, the disabled-path failure, and level checkpointing."""
import argparse
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--gem5", type=Path, required=True)
parser.add_argument("--host", choices=("generic", "arm"), required=True)
parser.add_argument("--outdir", type=Path, required=True)
args = parser.parse_args()
args.outdir.mkdir(parents=True, exist_ok=True)
config = Path(__file__).parent / "configs/pvrdma_pci.py"
checkpoint = args.outdir / "checkpoint"
results = []
for name, options, success, marker in (
    ("shared", [], True, "PVRDMA_PCI_SHARED_INTX_PASSED"),
    ("disabled", ["--no-aggregate"], False,
     "E1000 cleared another function's shared INTx"),
    ("save", ["--phase", "save", "--checkpoint", str(checkpoint)], True,
     "PVRDMA_PCI_SHARED_INTX_SAVED"),
    ("restore", ["--phase", "restore", "--checkpoint", str(checkpoint)], True,
     "PVRDMA_PCI_SHARED_INTX_PASSED"),
    ("reject-mode-change", ["--phase", "restore", "--checkpoint", str(checkpoint),
                            "--no-aggregate"], False,
     "PCI INTx aggregation mode differs from checkpoint"),
):
    command = [str(args.gem5), "-d", str(args.outdir / name), str(config),
               "--host", args.host, *options]
    result = subprocess.run(command, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, timeout=120)
    (args.outdir / f"{name}.log").write_text(result.stdout)
    passed = (result.returncode == 0) == success and marker in result.stdout
    results.append(dict(case=name, command=command, returncode=result.returncode,
                        expected_success=success, marker=marker, passed=passed))
    (args.outdir / "checks.json").write_text(json.dumps(results, indent=2) + "\n")
    print(name, "PASS" if passed else "FAIL", flush=True)
    if not passed:
        print(result.stdout)
        raise SystemExit(1)
print("PVRDMA_PCI_TESTS_OK host=" + args.host)
