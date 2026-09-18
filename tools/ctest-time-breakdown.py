#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Summarise CTest JUnit timings by test-manifest owner and by name family.

A suite's wall clock is only actionable once it is attributed. This joins one
or more `ctest --output-junit` files with `tests/test-manifest.tsv` and prints
the time each owning directory and each test-name family costs, plus the
slowest individual tests.

    tools/ctest-time-breakdown.py --junit 16k=unit-16k.xml --junit 4k=unit-4k.xml

Owner groups come from the manifest's owner column relative to `tests/`; a name
family is the first `--family-depth` dash-separated components of the test name.
Tests absent from the manifest are reported under `(unregistered)` so a missing
registration stays visible instead of silently vanishing from the totals.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import pathlib
import statistics
import sys
import xml.etree.ElementTree as ElementTree

UNREGISTERED = "(unregistered)"


def read_manifest(path: pathlib.Path) -> dict[str, str]:
    """Map every registered test name to its owner path."""
    owners: dict[str, str] = {}
    with path.open(encoding="utf-8") as manifest:
        for line_number, line in enumerate(manifest, start=1):
            row = line.rstrip("\n").split("\t")
            if line_number == 1 and row[0] == "name":
                continue
            if len(row) < 2 or not row[0]:
                continue
            owners[row[0]] = row[1]
    return owners


def owner_group(owner: str | None, root: str) -> str:
    """Reduce an owner path to the directory that owns the test."""
    if owner is None:
        return UNREGISTERED
    root = root.rstrip("/")
    if owner == root:
        return "(top-level)"
    prefix = root + "/"
    rest = owner[len(prefix):] if owner.startswith(prefix) else owner
    head, separator, _ = rest.partition("/")
    if separator:
        return head
    # A bare leaf is either a directory owner or a single source file.
    return "(top-level)" if "." in head else head


def name_family(name: str, depth: int) -> str:
    parts = name.split("-")
    if len(parts) <= depth:
        return name
    return "-".join(parts[:depth])


def read_junit(path: pathlib.Path) -> list[tuple[str, float, str]]:
    """Return (name, seconds, status) for every case in a JUnit file."""
    cases: list[tuple[str, float, str]] = []
    for case in ElementTree.parse(path).getroot().iter("testcase"):
        name = case.get("name") or ""
        try:
            seconds = float(case.get("time") or 0.0)
        except ValueError:
            seconds = 0.0
        status = case.get("status") or ""
        if case.find("failure") is not None:
            status = status or "fail"
        elif case.find("skipped") is not None:
            status = status or "skipped"
        cases.append((name, seconds, status or "run"))
    return cases


def emit_table(title: str, groups: dict[str, list[float]], total: float) -> None:
    print(f"\n{title}")
    print(
        f"{'group':<24}{'tests':>7}{'seconds':>11}{'share':>9}"
        f"{'mean':>9}{'median':>9}{'max':>9}"
    )
    for group, times in sorted(
        groups.items(), key=lambda item: sum(item[1]), reverse=True
    ):
        group_total = sum(times)
        share = (group_total / total * 100.0) if total else 0.0
        print(
            f"{group:<24}{len(times):>7}{group_total:>11.2f}{share:>8.1f}%"
            f"{statistics.fmean(times):>9.3f}"
            f"{statistics.median(times):>9.3f}{max(times):>9.3f}"
        )
    print(f"{'TOTAL':<24}{sum(len(t) for t in groups.values()):>7}{total:>11.2f}")


def report(
    label: str,
    cases: list[tuple[str, float, str]],
    owners: dict[str, str],
    arguments: argparse.Namespace,
) -> None:
    total = sum(seconds for _, seconds, _ in cases)
    print(f"\n{'=' * 72}\n{label}: {len(cases)} tests, {total:.2f} s of test time")

    by_owner: dict[str, list[float]] = {}
    for name, seconds, _ in cases:
        by_owner.setdefault(
            owner_group(owners.get(name), arguments.manifest_root), []
        ).append(seconds)
    emit_table(f"{label} by owner directory", by_owner, total)

    dominant = max(by_owner, key=lambda group: sum(by_owner[group]), default=None)
    if dominant is not None:
        members = [
            (name, seconds)
            for name, seconds, _ in cases
            if owner_group(owners.get(name), arguments.manifest_root) == dominant
        ]
        by_family: dict[str, list[float]] = {}
        for name, seconds in members:
            by_family.setdefault(
                name_family(name, arguments.family_depth), []
            ).append(seconds)
        emit_table(
            f"{label} {dominant} by name family (depth {arguments.family_depth})",
            by_family,
            sum(seconds for _, seconds in members),
        )

        heaviest = max(by_family, key=lambda family: sum(by_family[family]))
        if sum(by_family[heaviest]) > sum(seconds for _, seconds in members) / 2:
            by_sub: dict[str, list[float]] = {}
            for name, seconds in members:
                if name_family(name, arguments.family_depth) != heaviest:
                    continue
                by_sub.setdefault(
                    name_family(name, arguments.family_depth + 1), []
                ).append(seconds)
            emit_table(
                f"{label} {heaviest} by sub-family "
                f"(depth {arguments.family_depth + 1})",
                by_sub,
                sum(sum(times) for times in by_sub.values()),
            )

    ranked = sorted(cases, key=lambda case: case[1], reverse=True)
    print(f"\n{label} slowest {arguments.top} tests")
    for name, seconds, status in ranked[: arguments.top]:
        print(f"{seconds:>9.2f}  {name}  [{status}]")

    over = [case for case in ranked if case[1] > arguments.over]
    print(f"\n{label} tests over {arguments.over:g} s: {len(over)}")
    for name, seconds, status in over:
        print(f"{seconds:>9.2f}  {name}  [{status}]")

    failures = [case for case in cases if case[2] not in ("run", "passed", "skipped")]
    print(f"\n{label} non-passing cases: {len(failures)}")
    for name, seconds, status in failures:
        print(f"{seconds:>9.2f}  {name}  [{status}]")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--junit",
        action="append",
        required=True,
        metavar="[LABEL=]PATH",
        help="a ctest --output-junit file; repeat for several profiles",
    )
    parser.add_argument(
        "--manifest",
        type=pathlib.Path,
        default=pathlib.Path("tests/test-manifest.tsv"),
        help="test manifest to join against (default: %(default)s)",
    )
    parser.add_argument(
        "--manifest-root",
        default="tests/unit",
        help="owner prefix stripped before grouping (default: %(default)s)",
    )
    parser.add_argument(
        "--family-depth",
        type=int,
        default=2,
        help="dash-separated components forming a name family (default: %(default)s)",
    )
    parser.add_argument(
        "--top", type=int, default=20, help="slowest tests to list (default: %(default)s)"
    )
    parser.add_argument(
        "--over",
        type=float,
        default=10.0,
        help="also list every test above this many seconds (default: %(default)s)",
    )
    parser.add_argument(
        "--combined",
        action="store_true",
        help="also report every JUnit file joined into one total",
    )
    arguments = parser.parse_args(argv)

    owners = read_manifest(arguments.manifest)
    everything: list[tuple[str, float, str]] = []
    for entry in arguments.junit:
        label, separator, path = entry.partition("=")
        if not separator:
            label, path = pathlib.Path(entry).stem, entry
        cases = read_junit(pathlib.Path(path))
        report(label, cases, owners, arguments)
        everything.extend(cases)

    if arguments.combined and len(arguments.junit) > 1:
        report("combined", everything, owners, arguments)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
