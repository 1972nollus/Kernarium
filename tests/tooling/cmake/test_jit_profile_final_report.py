#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prove FILE-independent records and change-detected cumulative draining."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import os
import re
import subprocess
from pathlib import Path


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def matching_lines(output: str, prefix: str) -> list[str]:
    return [line for line in output.splitlines() if line.startswith(prefix)]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixture", required=True, type=Path)
    arguments = parser.parse_args()

    environment = os.environ.copy()
    environment.update(
        {
            "UML_JIT_PROFILE_COMPILE": "1",
            "UML_JIT_PROFILE_COMPILE_INTERVAL": "1048576",
            "UML_JIT_PROFILE_OVERLAP": "1",
            "UML_JIT_PROFILE_OVERLAP_INTERVAL": "1048576",
            "UML_JIT_PROFILE_SHARED_TRANSLATIONS": "1",
            "UML_JIT_PROFILE_EXECUTOR_POOL": "1",
            "UML_JIT_DUMP_EXPANSION": "1",
            "UML_JIT_DUMP_MIN_PC": "0x3000",
            "UML_JIT_DUMP_MAX_PC": "0x4000",
        }
    )
    try:
        completed = subprocess.run(
            [str(arguments.fixture)],
            check=False,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=10,
        )
    except subprocess.TimeoutExpired as error:
        raise RuntimeError(
            "JIT final reporter blocked on the deliberately held stderr "
            "FILE lock"
        ) from error
    output = completed.stdout + completed.stderr
    require(
        completed.returncode == 0,
        f"JIT final-report fixture failed with {completed.returncode}:\n{output}",
    )
    require(
        output.count(
            "JIT nonblocking backpressure and line framing passed"
        )
        == 1,
        "nonblocking stderr backpressure or unterminated-prefix framing "
        f"was not preserved:\n{output}",
    )

    compile_lines = matching_lines(output, "Darwin JIT compile profile final=")
    require(
        len(compile_lines) == 2,
        f"expected two changed compile finals, found {len(compile_lines)}:\n"
        f"{output}",
    )
    require(
        " blocks=4 " in compile_lines[0]
        and " guest=4 " in compile_lines[0]
        and " local-compile-calls=2 local-compile-ns=3000 "
        in compile_lines[0]
        and " local-compile-avg-ns=1500.00 " in compile_lines[0]
        and " shared-materialization-calls=2 shared-materialization-ns=300 "
        in compile_lines[0]
        and " shared-materialization-avg-ns=150.00 " in compile_lines[0],
        f"compile final line did not combine retired and live TLS tails:\n{output}",
    )
    require(
        " blocks=5 " in compile_lines[1]
        and " guest=5 " in compile_lines[1]
        and " local-compile-calls=3 local-compile-ns=3500 "
        in compile_lines[1]
        and " shared-materialization-calls=3 shared-materialization-ns=350 "
        in compile_lines[1],
        f"compile final line did not report later cumulative activity:\n{output}",
    )
    require(
        not matching_lines(output, "Darwin JIT compile profile thread="),
        f"one block unexpectedly reached the periodic compile interval:\n{output}",
    )
    expansion_headers = matching_lines(output, "Darwin JIT expansion thread=")
    expansion_records = matching_lines(
        output, "Darwin JIT expansion words thread="
    )
    header_pattern = re.compile(
        r"^Darwin JIT expansion thread=(\S+) pc=(0x[0-9a-f]+) "
        r"guest=1 hot=400 total=400 guest-chunks=1 host-chunks=17$"
    )
    record_pattern = re.compile(
        r"^Darwin JIT expansion words thread=(\S+) pc=(0x[0-9a-f]+) "
        r"section=(guest|host) chunk=(\d+)/(\d+) offset=(\d+) "
        r"count=(\d+) words=((?:[0-9a-f]{8},)*)$"
    )
    headers = [header_pattern.fullmatch(line) for line in expansion_headers]
    records = [record_pattern.fullmatch(line) for line in expansion_records]
    require(
        len(headers) == 2
        and all(match is not None for match in headers)
        and len(records) == 36
        and all(match is not None for match in records)
        and all(len(line) < 512 for line in expansion_records),
        f"concurrent expansion records are malformed or overlong:\n{output}",
    )
    indexed_headers = {match.group(2): match.group(1) for match in headers if match}
    require(
        set(indexed_headers) == {"0x3000", "0x4000"},
        f"concurrent expansion headers have the wrong PCs:\n{output}",
    )
    for pc, prefix in (("0x3000", 0xA0000000), ("0x4000", 0xB0000000)):
        matching = [
            match
            for match in records
            if match is not None and match.group(2) == pc
        ]
        require(
            len(matching) == 18
            and all(match.group(1) == indexed_headers[pc] for match in matching),
            f"{pc} expansion chunks mixed producer identities:\n{output}",
        )
        guest = [match for match in matching if match.group(3) == "guest"]
        host = sorted(
            (match for match in matching if match.group(3) == "host"),
            key=lambda match: int(match.group(4)),
        )
        require(
            len(guest) == 1
            and guest[0].groups()[3:6] == ("1", "1", "0")
            and guest[0].group(7) == "1"
            and guest[0].group(8) == "d503201f,"
            and len(host) == 17,
            f"{pc} expansion section layout is incomplete:\n{output}",
        )
        reconstructed: list[str] = []
        for index, match in enumerate(host):
            expected_count = 24 if index < 16 else 16
            require(
                int(match.group(4)) == index + 1
                and int(match.group(5)) == 17
                and int(match.group(6)) == index * 24
                and int(match.group(7)) == expected_count,
                f"{pc} expansion chunk sequence is malformed:\n{output}",
            )
            reconstructed.extend(match.group(8).rstrip(",").split(","))
        require(
            reconstructed
            == [f"{prefix | index:08x}" for index in range(400)],
            f"{pc} expansion words were truncated or interleaved:\n{output}",
        )

    overlap_lines = matching_lines(output, "Darwin JIT translation overlap ")
    require(
        len(overlap_lines) == 3,
        f"expected the retired-worker snapshot and two changed final overlap "
        f"lines, found {len(overlap_lines)}:\n{output}",
    )
    require(
        "threads=1 translations=1 per-thread-distinct=1 union-distinct=1 "
        in overlap_lines[0],
        f"retired-worker overlap snapshot is malformed:\n{output}",
    )
    require(
        "threads=4 translations=4 per-thread-distinct=4 union-distinct=4 "
        in overlap_lines[1],
        f"overlap final line did not combine retired and live TLS tails:\n{output}",
    )
    require(
        "threads=4 translations=5 per-thread-distinct=5 union-distinct=5 "
        in overlap_lines[2],
        f"overlap final line did not report later cumulative activity:\n{output}",
    )

    shared_lines = matching_lines(
        output, "Darwin JIT shared translations final "
    )
    require(
        len(shared_lines) == 2,
        f"expected two changed shared-cache finals, found {len(shared_lines)}:\n"
        f"{output}",
    )
    require(
        "entries=0 published=0" in shared_lines[0]
        and "missed=0" in shared_lines[0]
        and "insertion-failures=0" in shared_lines[0]
        and "failed-insertion-retained-accounted-bytes=0" in shared_lines[0]
        and "entries=0 published=0" in shared_lines[1]
        and "missed=1" in shared_lines[1]
        and "insertion-failures=0" in shared_lines[1]
        and "failed-insertion-retained-accounted-bytes=0" in shared_lines[1],
        f"empty fixture shared-cache summary is malformed:\n{output}",
    )

    pool_lines = matching_lines(output, "Darwin JIT executor pool final ")
    require(
        len(pool_lines) == 2,
        f"expected two changed executor-pool finals, found {len(pool_lines)}:\n"
        f"{output}",
    )
    expected_first_pool_fields = {
        "wrappers": 3,
        "borrows": 6,
        "no-preference": 2,
        "preferred-attempts": 4,
        "preferred-hits": 2,
        "fallback-claims": 2,
        "cells-probed": 10,
        "failed-cas": 6,
        "waiter-enqueues": 1,
        "waiter-wakes": 1,
        "fifo-handoffs": 1,
        "max-waiters": 3,
        "construction-attempts": 2,
        "created": 2,
        "peak-live": 2,
        "peak-borrowed-observed": 2,
        "slot-changes": 1,
        "wrappers-slot-changed": 1,
        "max-slot-changes": 1,
        "null-sequences": 1,
        "sequence-changes": 4,
        "wrappers-sequence-changed": 3,
        "max-sequence-changes": 2,
        "same-sequence-different-slot": 0,
        "sequence-distinct": 4,
        "sequence-shared": 1,
        "sequence-extra-wrapper-associations": 1,
        "sequence-max-fanout": 2,
        "sequence-multislot": 2,
        "sequence-extra-slot-associations": 2,
        "sequence-max-slot-fanout": 2,
        "sequence-association-overflow": 0,
        "sequence-global-overflow": 0,
        "standalone-constructions": 2,
        "standalone-live-peak": 2,
    }
    expected_second_pool_fields = {
        **expected_first_pool_fields,
        "wrappers": 4,
        "borrows": 7,
        "no-preference": 3,
        "construction-attempts": 3,
        "created": 3,
        "sequence-distinct": 5,
    }
    parsed_pool_fields = [
        {
            key: int(value)
            for key, value in re.findall(r"([a-z][a-z-]+)=(\d+)", line)
        }
        for line in pool_lines
    ]
    require(
        parsed_pool_fields
        == [expected_first_pool_fields, expected_second_pool_fields],
        f"executor-pool final fields are malformed:\n{output}",
    )
    retired_marker = (
        "Darwin JIT executor pool fixture phase=retired-unchanged"
    )
    pool_only_marker = (
        "Darwin JIT executor pool fixture phase=pool-only-complete"
    )
    require(
        all("0x" not in line for line in pool_lines)
        and output.count(retired_marker) == 1
        and output.count(pool_only_marker) == 1
        and output.index(pool_lines[0]) < output.index(shared_lines[0])
        < output.index(retired_marker) < output.index(pool_lines[1])
        < output.index(pool_only_marker) < output.index(shared_lines[1]),
        "executor-pool output leaked an address, reordered finals, duplicated "
        f"a retirement-only snapshot, or forced an unchanged shared final:\n{output}",
    )

    print("JIT explicit final-report contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
