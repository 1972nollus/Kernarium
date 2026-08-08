#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pin the translated gadget bank's layout across optimization trials.

G17 and G36 were both rejected after a gadget-body change moved every later
entry in the bank and the resulting cache-line phase shift outweighed the
instruction reduction.  G30 hit the same effect and had to restore "its
established 64-byte phase", and G35 finally established the technique:
unreachable padding after each indirect dispatch keeps every later gadget at
its previous address and phase.

This audit makes that technique checkable.  It records each defined
``uml_arm64_gadget_*`` entry's offset relative to the first gadget and its
64-byte cache-line phase, then fails when a trial moves an entry it did not
declare.  A trial that intends to change a gadget lists it in an
acknowledgement file; everything outside that list must stay exactly where it
was, which is precisely the discipline the measurement rules require.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import fnmatch
import re
import subprocess
import sys
from pathlib import Path

GADGET_PREFIX = "_uml_arm64_gadget_"
CACHE_LINE_BYTES = 64
REPORT_LIMIT = 20

SYMBOL_PATTERN = re.compile(r"^([0-9a-fA-F]+)\s+([tT])\s+(\S+)$")


class AuditError(RuntimeError):
    """A layout difference the trial did not declare."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def object_list(value: str) -> list[Path]:
    result = [Path(entry) for entry in value.split("|") if entry]
    require(bool(result), "configured object list is empty")
    missing = sorted(str(path) for path in result if not path.is_file())
    require(not missing, f"configured objects are missing: {missing}")
    return result


def defined_text_symbols(nm: Path, path: Path) -> list[tuple[int, str]]:
    """Return ``(address, name)`` for every defined text symbol, sorted."""
    completed = subprocess.run(
        [str(nm), "-n", str(path)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    require(
        completed.returncode == 0,
        f"nm failed for {path}:\n{completed.stdout}",
    )
    symbols: list[tuple[int, str]] = []
    for line in completed.stdout.splitlines():
        matched = SYMBOL_PATTERN.match(line.strip())
        if matched is None:
            continue
        symbols.append((int(matched.group(1), 16), matched.group(3)))
    symbols.sort()
    return symbols


def bank_layout(nm: Path, objects: list[Path]) -> dict[str, tuple[int, int, int]]:
    """Map each gadget entry to ``(relative offset, phase, size)``.

    Offsets are relative to the lowest-addressed gadget so that moving the
    whole bank -- which the linker may do for unrelated reasons -- is not
    reported as a per-gadget change.  Sizes are the distance to the next
    defined text symbol, which is the gadget body plus any padding.
    """
    layout: dict[str, tuple[int, int, int]] = {}
    for path in objects:
        symbols = defined_text_symbols(nm, path)
        gadgets = [entry for entry in symbols if entry[1].startswith(GADGET_PREFIX)]
        if not gadgets:
            continue
        base = gadgets[0][0]
        addresses = [address for address, _ in symbols]
        for address, name in gadgets:
            require(
                name not in layout,
                f"gadget {name} is defined in more than one object",
            )
            index = addresses.index(address)
            following = (
                addresses[index + 1] if index + 1 < len(addresses) else address
            )
            layout[name] = (
                address - base,
                address % CACHE_LINE_BYTES,
                following - address,
            )
    require(bool(layout), "no gadget entries were found in the configured objects")
    return layout


def write_layout(layout: dict[str, tuple[int, int, int]], path: Path) -> None:
    lines = ["symbol\toffset\tphase\tsize"]
    lines.extend(
        f"{name}\t{offset}\t{phase}\t{size}"
        for name, (offset, phase, size) in sorted(layout.items())
    )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def read_layout(path: Path) -> dict[str, tuple[int, int, int]]:
    require(path.is_file(), f"missing gadget layout baseline: {path}")
    layout: dict[str, tuple[int, int, int]] = {}
    for number, line in enumerate(
        path.read_text(encoding="utf-8").splitlines(), start=1
    ):
        if not line.strip() or line.startswith("symbol\t"):
            continue
        fields = line.split("\t")
        require(
            len(fields) == 4,
            f"{path}:{number}: expected four tab-separated fields",
        )
        try:
            layout[fields[0]] = (
                int(fields[1]),
                int(fields[2]),
                int(fields[3]),
            )
        except ValueError as error:
            raise RuntimeError(
                f"{path}:{number}: non-integer layout field"
            ) from error
    require(bool(layout), f"{path}: baseline contains no gadget entries")
    return layout


def read_acknowledgements(path: Path | None) -> list[str]:
    if path is None:
        return []
    require(path.is_file(), f"missing acknowledgement file: {path}")
    patterns: list[str] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        entry = line.split("#", 1)[0].strip()
        if entry:
            patterns.append(entry)
    return patterns


def acknowledged(name: str, patterns: list[str]) -> bool:
    bare = name.removeprefix("_")
    return any(
        fnmatch.fnmatchcase(name, pattern) or fnmatch.fnmatchcase(bare, pattern)
        for pattern in patterns
    )


def compare(
    baseline: dict[str, tuple[int, int, int]],
    current: dict[str, tuple[int, int, int]],
    patterns: list[str],
) -> tuple[list[str], dict[str, int]]:
    """Return unacknowledged differences and a summary of all differences."""
    removed = sorted(set(baseline) - set(current))
    added = sorted(set(current) - set(baseline))
    shared = sorted(set(baseline) & set(current))

    moved = [name for name in shared if baseline[name][0] != current[name][0]]
    rephased = [name for name in shared if baseline[name][1] != current[name][1]]
    resized = [name for name in shared if baseline[name][2] != current[name][2]]

    summary = {
        "removed": len(removed),
        "added": len(added),
        "moved": len(moved),
        "rephased": len(rephased),
        "resized": len(resized),
    }

    failures: list[str] = []
    for name in removed:
        if not acknowledged(name, patterns):
            failures.append(f"removed entry {name}")
    for name in added:
        if not acknowledged(name, patterns):
            failures.append(f"added entry {name}")
    for name in moved:
        if acknowledged(name, patterns):
            continue
        delta = current[name][0] - baseline[name][0]
        failures.append(
            f"{name} moved {delta:+d} bytes "
            f"({baseline[name][0]} -> {current[name][0]})"
        )
    for name in rephased:
        if acknowledged(name, patterns) or name in moved:
            continue
        failures.append(
            f"{name} changed cache-line phase "
            f"({baseline[name][1]} -> {current[name][1]})"
        )
    return failures, summary


def describe(summary: dict[str, int], total: int) -> str:
    return (
        f"{total} gadget entries; "
        f"{summary['moved']} moved, "
        f"{summary['rephased']} changed 64-byte phase, "
        f"{summary['resized']} changed size, "
        f"{summary['added']} added, "
        f"{summary['removed']} removed"
    )


def self_test() -> None:
    """Prove the comparator detects each difference class and honours globs."""
    baseline = {
        "_uml_arm64_gadget_a": (0, 0, 64),
        "_uml_arm64_gadget_b": (64, 0, 32),
        "_uml_arm64_gadget_c": (96, 32, 32),
    }

    failures, summary = compare(baseline, dict(baseline), [])
    require(not failures, f"identical layouts were reported as changed: {failures}")
    require(
        summary == {
            "removed": 0,
            "added": 0,
            "moved": 0,
            "rephased": 0,
            "resized": 0,
        },
        f"identical layouts produced a non-empty summary: {summary}",
    )

    moved = dict(baseline)
    moved["_uml_arm64_gadget_c"] = (112, 48, 32)
    failures, summary = compare(baseline, moved, [])
    require(len(failures) == 1, f"a moved entry was not reported once: {failures}")
    require("+16 bytes" in failures[0], f"movement delta is missing: {failures[0]}")
    require(summary["moved"] == 1, f"moved count is wrong: {summary}")

    failures, _ = compare(baseline, moved, ["uml_arm64_gadget_c"])
    require(not failures, f"acknowledged movement still failed: {failures}")
    failures, _ = compare(baseline, moved, ["uml_arm64_gadget_*"])
    require(not failures, f"glob acknowledgement did not apply: {failures}")
    failures, _ = compare(baseline, moved, ["uml_arm64_gadget_b"])
    require(
        len(failures) == 1,
        f"an unrelated acknowledgement suppressed a real move: {failures}",
    )

    rephased = dict(baseline)
    rephased["_uml_arm64_gadget_b"] = (64, 16, 32)
    failures, summary = compare(baseline, rephased, [])
    require(
        len(failures) == 1 and "cache-line phase" in failures[0],
        f"a phase-only change was not reported: {failures}",
    )
    require(summary["rephased"] == 1, f"rephased count is wrong: {summary}")

    resized = dict(baseline)
    resized["_uml_arm64_gadget_b"] = (64, 0, 48)
    failures, summary = compare(baseline, resized, [])
    require(
        not failures,
        f"a size-only change must be advisory, not a failure: {failures}",
    )
    require(summary["resized"] == 1, f"resized count is wrong: {summary}")

    removed = {
        name: value
        for name, value in baseline.items()
        if name != "_uml_arm64_gadget_b"
    }
    failures, summary = compare(baseline, removed, [])
    require(
        len(failures) == 1 and failures[0].startswith("removed entry"),
        f"a removed entry was not reported: {failures}",
    )

    added = dict(baseline)
    added["_uml_arm64_gadget_d"] = (128, 0, 16)
    failures, summary = compare(baseline, added, [])
    require(
        len(failures) == 1 and failures[0].startswith("added entry"),
        f"an added entry was not reported: {failures}",
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Audit the translated gadget bank's address layout.",
    )
    parser.add_argument("--nm", type=Path, default=Path("nm"))
    parser.add_argument(
        "--objects",
        type=object_list,
        help="pipe-separated gadget object files",
    )
    parser.add_argument(
        "--baseline-objects",
        type=object_list,
        help="pipe-separated gadget objects from the accepted build",
    )
    # Reading the linked image is strictly better than reading the objects, and
    # since G99 turned on interprocedural optimization it is the only way to see
    # the whole bank: the gadget objects are now bitcode, so nm reports the
    # 9,576 entries defined in assembly and none of the 87 defined in C++. Those
    # 87 are not inert - G101 caught them moving 752 bytes and changing
    # cache-line phase while every assembly entry stayed put, which an
    # object-based snapshot cannot see at all.
    parser.add_argument(
        "--image",
        type=object_list,
        help="linked image to read the bank from, in place of --objects",
    )
    parser.add_argument(
        "--baseline-image",
        type=object_list,
        help="linked image from the accepted build",
    )
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--write-baseline", type=Path)
    parser.add_argument("--acknowledge", type=Path)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--self-test", action="store_true")
    arguments = parser.parse_args()

    self_test()
    sources = arguments.image or arguments.objects
    if arguments.self_test and sources is None:
        print("gadget bank layout comparator self-test passed")
        return 0

    require(
        sources is not None,
        "--image or --objects is required unless only --self-test is requested",
    )
    require(
        arguments.image is None or arguments.objects is None,
        "--image and --objects are mutually exclusive",
    )
    baseline_sources = arguments.baseline_image or arguments.baseline_objects
    require(
        arguments.baseline is None or baseline_sources is None,
        "--baseline and a baseline image/object set are mutually exclusive",
    )
    current = bank_layout(arguments.nm, sources)

    if arguments.write_baseline is not None:
        arguments.write_baseline.parent.mkdir(parents=True, exist_ok=True)
        write_layout(current, arguments.write_baseline)
        print(
            f"wrote {len(current)} gadget entries to "
            f"{arguments.write_baseline}"
        )
        if arguments.baseline is None and baseline_sources is None:
            return 0

    require(
        arguments.baseline is not None or baseline_sources is not None,
        "--baseline, a baseline image/object set, or --write-baseline is required",
    )
    baseline = (
        bank_layout(arguments.nm, baseline_sources)
        if baseline_sources is not None
        else read_layout(arguments.baseline)
    )
    patterns = read_acknowledgements(arguments.acknowledge)
    failures, summary = compare(baseline, current, patterns)

    if arguments.report is not None:
        arguments.report.parent.mkdir(parents=True, exist_ok=True)
        write_layout(current, arguments.report)

    print(describe(summary, len(current)), flush=True)
    if patterns:
        print(f"acknowledged patterns: {', '.join(patterns)}", flush=True)

    if failures:
        print(
            f"gadget bank layout changed in {len(failures)} undeclared ways:",
            file=sys.stderr,
        )
        for entry in failures[:REPORT_LIMIT]:
            print(f"  {entry}", file=sys.stderr)
        if len(failures) > REPORT_LIMIT:
            print(
                f"  ... and {len(failures) - REPORT_LIMIT} more",
                file=sys.stderr,
            )
        print(
            "Pad the changed gadget so later entries keep their address and "
            "64-byte phase, as G35 did, or list the intended entries in the "
            "acknowledgement file.",
            file=sys.stderr,
        )
        return 1

    print("gadget bank layout audit passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(2) from error
