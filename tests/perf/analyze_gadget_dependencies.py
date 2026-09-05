#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Summarize execution-weighted opportunities in the shared scalar contract.

Counters are exact per entered decoder group. Time-window attribution uses
100-ms snapshots and host-observed console markers, not retired instructions.
Diagnostic runs are not performance scores. No workloads are averaged equally:
the summary divides totals of entered groups and declared source accessors.
Conditional accessors are potential reads; this does not infer taken CCMP
conditions, retirement after a fault, or data dependencies across block exits.
"""
from __future__ import annotations
__import__("sys").dont_write_bytecode = True

import argparse
import csv
import json
from pathlib import Path
import re

LEGACY_BUCKET = re.compile(r"^(add_imm|sub_imm|add_reg|sub_reg|and|orr|eor|ands)_(w|x)_f([01])_m([0-3])$")
BUCKET = re.compile(r"^([a-z_]+)_s([0-3])_m([0-7])$")


def read_counts(path: Path) -> list[dict[str, int]]:
    with path.open() as source:
        reader = csv.DictReader(source)
        if not reader.fieldnames or reader.fieldnames[:3] != [
            "monotonic_ns", "entered_guest_words", "unsupported_groups"
        ]:
            raise ValueError("unexpected dependency counter schema")
        buckets = reader.fieldnames[3:]
        legacy = len(buckets) == 128 and all(LEGACY_BUCKET.fullmatch(name) for name in buckets)
        families = list(dict.fromkeys(
            match[1] for name in buckets if (match := BUCKET.fullmatch(name))))
        expected = [f"{family}_s{sources}_m{mask}" for family in families
                    for sources in range(4) for mask in range(8)]
        if not legacy and (not families or buckets != expected):
            raise ValueError("unexpected scalar dependency buckets")
        rows = [{key: int(value) for key, value in row.items()} for row in reader]
    if len(rows) < 2:
        raise ValueError("need at least two complete counter snapshots")
    for before, after in zip(rows, rows[1:]):
        if any(after[key] < before[key] for key in after):
            raise ValueError("counter snapshot moved backwards")
    return rows


def summarize(values: dict[str, int]) -> dict[str, object]:
    families: dict[str, dict[str, int]] = {}
    for key, value in values.items():
        match = BUCKET.fullmatch(key)
        if match:
            family, source_text, mask_text = match.groups()
            source_count = int(source_text)
        elif (match := LEGACY_BUCKET.fullmatch(key)):
            family, _, _, mask_text = match.groups()
            source_count = 1 if family.endswith("_imm") else 2
        else:
            continue
        mask = int(mask_text)
        if value and mask.bit_count() > source_count:
            raise ValueError("forwarding mask exceeds the declared source count")
        row = families.setdefault(family, dict(groups=0, forwarded_groups=0, source_reads=0, forwarded_reads=0))
        row["groups"] += value
        row["forwarded_groups"] += value * bool(mask)
        row["source_reads"] += value * source_count
        row["forwarded_reads"] += value * mask.bit_count()
    totals = {key: sum(row[key] for row in families.values()) for key in next(iter(families.values()))}
    words = values["entered_guest_words"]
    groups = totals["groups"] + values["unsupported_groups"]
    def percent(numerator: int, denominator: int) -> float:
        return 100 * numerator / denominator if denominator else 0.0
    return {
        "entered_guest_words": words,
        "entered_decode_groups": groups,
        "eligible_groups": totals["groups"],
        "forwardable_groups": totals["forwarded_groups"],
        "eligible_source_reads": totals["source_reads"],
        "forwardable_source_reads": totals["forwarded_reads"],
        "eligible_word_percent": percent(totals["groups"], words),
        "forwardable_word_percent": percent(totals["forwarded_groups"], words),
        "forwardable_eligible_percent": percent(totals["forwarded_groups"], totals["groups"]),
        "forwardable_source_percent": percent(totals["forwarded_reads"], totals["source_reads"]),
        "families": families,
    }


def interval(rows: list[dict[str, int]], start: int, end: int) -> dict[str, object]:
    if end <= start:
        raise ValueError("empty dependency window")
    before = min(rows, key=lambda row: abs(row["monotonic_ns"] - start))
    after = min(rows, key=lambda row: abs(row["monotonic_ns"] - end))
    if after["monotonic_ns"] <= before["monotonic_ns"]:
        raise ValueError("dependency window is shorter than snapshot resolution")
    values = {key: after[key] - before[key] for key in before if key != "monotonic_ns"}
    return {
        "requested_start_ns": start, "requested_end_ns": end,
        "snapshot_start_ns": before["monotonic_ns"],
        "snapshot_end_ns": after["monotonic_ns"],
        **summarize(values),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("counters", type=Path)
    windows = parser.add_mutually_exclusive_group(required=True)
    windows.add_argument("--geekbench-events", type=Path)
    windows.add_argument("--aquarium-metrics", type=Path)
    args = parser.parse_args()
    rows = read_counts(args.counters)
    result: dict[str, object] = {"schema_version": 2 if any(BUCKET.fullmatch(key) for key in rows[0]) else 1,
                                 "counter_interval_ms": 100}
    if args.geekbench_events:
        with args.geekbench_events.open() as source:
            events = list(csv.DictReader(source, delimiter="\t"))
        starts = [event for event in events if event["phase"] == "Single-Core"]
        ends = [event for event in events if event["phase"] == "complete"]
        if len(starts) != 1 or len(ends) != 1 or any("Multi-Core" in event["phase"] for event in events):
            raise ValueError("requires a completed single-core-only Geekbench run")
        start, end = int(starts[0]["monotonic_ns"]), int(ends[0]["monotonic_ns"])
        result["summary"] = interval(rows, start, end)
        completed = [event for event in events if event["phase"] == "Single-Core-completed"]
        workloads = []
        previous = start
        for event in completed:
            boundary = int(event["monotonic_ns"])
            workloads.append({"workload": event["workload"], **interval(rows, previous, boundary)})
            previous = boundary
        result["completed_workloads"] = workloads
        result["console_poll_interval_ms"] = 100
    else:
        metrics = json.loads(args.aquarium_metrics.read_text())
        samples = metrics["host_measurement"]["snapshots"]
        start = round(samples[0]["monotonic_seconds"] * 1e9)
        end = round(samples[-1]["monotonic_seconds"] * 1e9)
        result["summary"] = interval(rows, start, end)
        result["console_poll_interval_ms"] = 500
        result["fish"] = metrics["FISH"]
        result["sample_count"] = metrics["SAMPLES"]
    print(json.dumps(result, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
