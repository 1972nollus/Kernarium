#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Export the engine-harness cases of one unit build tree.

Every unit CTest whose executable links the arm64 engine harness runs twice
with the corpus export environment described in
tests/unit/arm64/arm64_instruction_corpus_export.hpp: a count pass that sizes
each equivalence class, then a sample pass that verifies every selected case
through all engine lanes and writes it. Build a 16 KiB and a 4 KiB tree, export
both, and pass the two directories to build-unit-corpus.py.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import collections
import csv
import json
import mmap
import os
import pathlib
import re
import subprocess
import sys


MARKER = b"UML_ARM64_CORPUS_EXPORT_PASS"


def fail(message: str) -> None:
    raise SystemExit(f"export-unit-corpus: {message}")


def unit_tests(build: pathlib.Path, timeout: int) -> list[dict]:
    result = subprocess.run(
        ["ctest", "--test-dir", str(build), "--show-only=json-v1", "-L", "^unit$"],
        check=True, capture_output=True, text=True, timeout=timeout,
        stdin=subprocess.DEVNULL,
    )
    return json.loads(result.stdout)["tests"]


def links_exporter(path: pathlib.Path) -> bool:
    with path.open("rb") as source:
        with mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as data:
            return data.find(MARKER) >= 0


def producers(build: pathlib.Path, pattern: str | None, timeout: int) -> list[str]:
    names = []
    for test in unit_tests(build, timeout):
        if pattern is not None and re.search(pattern, test["name"]) is None:
            continue
        executables = [pathlib.Path(item) for item in test.get("command", [])
                       if pathlib.Path(item).is_file()]
        if executables and links_exporter(executables[-1]):
            names.append(test["name"])
    return sorted(names)


def run_pass(options: argparse.Namespace, names: list[str], label: str) -> None:
    environment = dict(os.environ)
    environment.update({
        "UML_ARM64_CORPUS_EXPORT": str(options.output),
        "UML_ARM64_CORPUS_EXPORT_PASS": label,
        "UML_ARM64_CORPUS_K": str(options.sample_limit),
    })
    if options.seed is not None:
        environment["UML_ARM64_CORPUS_SEED"] = options.seed
    environment.pop("UML_ARM64_CORPUS_INCLUDE", None)
    if label == "sample" and options.include is not None:
        environment["UML_ARM64_CORPUS_INCLUDE"] = str(options.include)
    expression = "^(" + "|".join(re.escape(name) for name in names) + ")$"
    command = ["ctest", "--test-dir", str(options.build_dir), "-R", expression,
               "-j", str(options.jobs), "--output-on-failure"]
    log = options.output / f"ctest-{label}.log"
    with log.open("w", encoding="utf-8") as sink:
        try:
            status = subprocess.run(command, env=environment, stdout=sink,
                                    stderr=subprocess.STDOUT,
                                    stdin=subprocess.DEVNULL,
                                    timeout=options.timeout).returncode
        except subprocess.TimeoutExpired:
            fail(f"the {label} pass exceeded {options.timeout} s; see {log}")
    if status != 0:
        fail(f"the {label} pass failed; see {log}")


def read_rows(path: pathlib.Path) -> list[dict]:
    with path.open(encoding="utf-8") as source:
        return list(csv.DictReader(source, delimiter="\t"))


def summarize(options: argparse.Namespace, names: list[str]) -> None:
    selections = collections.Counter()
    forced = collections.Counter()
    silent = 0
    for name in names:
        index = options.output / f"{name}.cases.tsv"
        if not index.is_file():
            if (options.output / f"{name}.classes.tsv").is_file():
                fail(f"{name} wrote no sample-pass index")
            silent += 1
            continue
        for row in read_rows(index):
            selections[row["selection"]] += 1
        report = options.output / f"{name}.forced.tsv"
        if report.is_file():
            for row in read_rows(report):
                result = row["result"].split(":", 1)[0]
                forced[result] += 1
                if result != "exported":
                    print(f"{name}: {row['directive']} ordinal "
                          f"{row['ordinal'] or '-'}: {row['result']}")
    print(f"exported {sum(selections.values())} cases from "
          f"{len(names) - silent} producers ({silent} linked the harness "
          f"without checking a case): " + ", ".join(
              f"{key}={value}" for key, value in sorted(selections.items())))
    if forced:
        print("force-include results: " + ", ".join(
            f"{key}={value}" for key, value in sorted(forced.items())))


def check_directives(include: pathlib.Path, names: list[str]) -> None:
    known = set(names)
    with include.open(encoding="utf-8") as source:
        for line in source:
            fields = line.rstrip("\n").split("\t")
            if not fields[0] or fields[0].startswith("#"):
                continue
            producer = None
            if fields[0] == "case" and len(fields) == 3:
                producer = fields[1]
            elif fields[0] == "word" and 3 <= len(fields) <= 5:
                producer = fields[3] if len(fields) >= 4 else "*"
            else:
                fail(f"malformed force-include directive: {line.strip()}")
            if producer != "*" and producer not in known:
                fail(f"force-include names an unknown producer: {producer}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=pathlib.Path, required=True,
                        help="configured and built unit tree")
    parser.add_argument("--output", type=pathlib.Path, required=True,
                        help="new or empty export directory")
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--sample-limit", type=int, default=8,
                        help="cases per equivalence class (default: 8)")
    parser.add_argument("--seed", help="sampling seed (default: exporter's)")
    parser.add_argument("--include", type=pathlib.Path,
                        help="force-include directives for the sample pass")
    parser.add_argument("--tests", metavar="REGEX",
                        help="restrict the producers by CTest name")
    parser.add_argument("--timeout", type=int, default=3600,
                        help="seconds allowed for each pass (default: 3600)")
    options = parser.parse_args()
    options.build_dir = options.build_dir.resolve()
    options.output = options.output.resolve()
    if options.sample_limit < 2:
        parser.error("--sample-limit must keep the first and last case")
    if options.include is not None:
        options.include = options.include.resolve()
    if options.output.exists() and any(options.output.iterdir()):
        fail(f"{options.output} is not empty")
    options.output.mkdir(parents=True, exist_ok=True)
    names = producers(options.build_dir, options.tests, 300)
    if not names:
        fail("no unit test links the engine harness")
    if options.include is not None:
        check_directives(options.include, names)
    for label in ("count", "sample"):
        run_pass(options, names, label)
    summarize(options, names)
    return 0


if __name__ == "__main__":
    sys.exit(main())
