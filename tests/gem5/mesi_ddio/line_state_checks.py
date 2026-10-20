# Copyright (c) 2026 minhuw
# SPDX-License-Identifier: BSD-3-Clause
"""dma-llc-state-v1 labels and independent per-bank marginal checks."""

import math
from collections import Counter
from pathlib import Path


DIMENSIONS = {
    "State": [
        "MESI_NP", "MESI_SS", "MESI_M", "MESI_MT",
        "CHI_I", "CHI_SC", "CHI_UC", "CHI_SD", "CHI_UD", "CHI_UD_T",
        "CHI_RU", "CHI_RSC", "CHI_RSD", "CHI_RUSC", "CHI_RUSD",
        "CHI_SC_RSC", "CHI_SD_RSC", "CHI_SD_RSD", "CHI_UC_RSC",
        "CHI_UC_RU", "CHI_UD_RU", "CHI_UD_RSD", "CHI_UD_RSC",
        "CHI_DvmTlbi_Unconfirmed", "CHI_DvmSync_Unsent",
        "CHI_DvmSync_Unconfirmed", "CHI_DvmTlbi_Waiting",
        "CHI_DvmSync_Waiting", "CHI_DvmOp_Finished",
        "CHI_DvmExtTlbi_Partial", "CHI_DvmExtTlbi_Executing",
        "CHI_DvmExtSync_Partial", "CHI_DvmExtSync_Executing",
        "CHI_DvmExtOp_Finished", "CHI_BUSY_INTR", "CHI_BUSY_BLKD", "CHI_null",
    ],
    "TagPresence": ["absent", "present"],
    "LocalDirty": ["absent", "clean", "dirty"],
    "PrivateOwner": ["absent", "present"],
}


def read_stats(path):
    stats = {}
    for line in Path(path).read_text().splitlines():
        fields = line.split()
        if len(fields) >= 2:
            try:
                stats[fields[0]] = float(fields[1])
            except ValueError:
                pass
    return stats


def expected_marginals(read=(), write=()):
    """Aggregate fixture lookups (state, tag, local dirty, explicit owner).

    These tuples describe the controlled scenario, not a joint exported stat.
    Empty directions still require every bucket to be zero.
    """
    expected = {}
    for direction, lookups in (("Read", read), ("Write", write)):
        counts = {dimension: Counter() for dimension in DIMENSIONS}
        for state, tag, dirty, owner in lookups:
            counts["State"][state] += 1
            counts["TagPresence"]["present" if tag else "absent"] += 1
            counts["LocalDirty"][
                ("dirty" if dirty else "clean") if tag else "absent"
            ] += 1
            counts["PrivateOwner"]["present" if owner else "absent"] += 1
        expected.update({(direction, name): values for name, values in counts.items()})
    return expected


def _valid_count(value):
    return (isinstance(value, (int, float)) and math.isfinite(value)
            and value >= 0 and float(value).is_integer())


def check_marginals(stats, prefix, protocol, expected=None):
    """Require static buckets, per-direction totals and tag identities.

    expected maps (direction, dimension) to nonzero label counts. Supplying a
    dimension requires exact counts for ALL its labels, including zeros.
    """
    expected = expected or {}
    errors = []
    for direction in ("Read", "Write"):
        stem = prefix + "dma" + direction
        requests, hits, misses = (
            stats.get(stem + outcome) for outcome in ("Requests", "Hits", "Misses")
        )
        if not all(_valid_count(v) for v in (requests, hits, misses)):
            errors.append(f"{stem}: missing/invalid requests/hits/misses")
            continue
        if requests != hits + misses:
            errors.append(f"{stem}: requests != hits + misses")
        for dimension, labels in DIMENSIONS.items():
            name = stem + dimension
            values = {label: stats.get(name + "::" + label) for label in labels}
            if not all(_valid_count(v) for v in values.values()):
                errors.append(f"{name}: missing/invalid buckets {values}")
                continue
            if sum(values.values()) != requests or stats.get(name + "::total") != requests:
                errors.append(f"{name}: marginal total != requests ({values})")
            if dimension == "State" and any(
                value for label, value in values.items()
                if not label.startswith(protocol + "_")
            ):
                errors.append(f"{name}: inactive protocol bucket nonzero")
            if dimension == "TagPresence" and values != {"absent": misses, "present": hits}:
                errors.append(f"{name}: tag presence != misses/hits")
            if dimension == "LocalDirty" and (
                values["absent"] != misses or values["clean"] + values["dirty"] != hits
            ):
                errors.append(f"{name}: local dirty entry presence != misses/hits")
            if (direction, dimension) in expected:
                categories = expected[direction, dimension]
                unknown = categories.keys() - set(labels)
                if unknown:
                    errors.append(f"{name}: unknown expected labels {unknown}")
                wanted = {label: categories.get(label, 0) for label in labels}
                if values != wanted:
                    errors.append(f"{name}: expected {wanted}, observed {values}")
    if errors:
        raise ValueError("; ".join(errors))


def check_cache_marginals(cache, protocol, expected=None):
    """Check a live gem5 cache, using the exported labels from get_simstat."""
    from m5.stats.gem5stats import get_simstat

    node = get_simstat(cache, prepare_stats=True).to_json()
    stats = {}

    def collect(value):
        if not isinstance(value, dict):
            return
        for name, item in value.items():
            if (isinstance(name, str) and name.startswith("dma")
                    and isinstance(item, dict) and "value" in item):
                if isinstance(item["value"], dict):
                    buckets = item["value"]
                    for label, bucket in buckets.items():
                        stats[name + "::" + label] = bucket["value"]
                    stats[name + "::total"] = sum(b["value"] for b in buckets.values())
                else:
                    stats[name] = item["value"]
            else:
                collect(item)

    collect(node)
    check_marginals(stats, "", protocol, expected)
