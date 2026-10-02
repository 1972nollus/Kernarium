#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run every materialized FEX ASM test through the x86 gadget backend."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import concurrent.futures
import json
import os
import pathlib
import re
import subprocess
import tempfile
import sys
import time
from collections import Counter
from typing import Any

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import fex_config  # noqa: E402
from pinned_source import sha256_file  # noqa: E402


def fail(message: str) -> "NoReturn":
    raise SystemExit(f"run-asm-corpus.py: {message}")


def corpus_path(root: pathlib.Path, relative: object) -> pathlib.Path:
    path = pathlib.Path(str(relative))
    if path.is_absolute() or ".." in path.parts:
        fail(f"unsafe corpus path: {path}")
    result = root / path
    if not result.is_file():
        fail(f"missing corpus file: {path}")
    return result


def load_corpus(root: pathlib.Path) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    try:
        metadata = json.loads((root / "corpus.json").read_text())
    except (OSError, json.JSONDecodeError) as error:
        fail(f"invalid corpus metadata: {error}")
    tests = metadata.get("tests")
    if (
        metadata.get("schema") != 1
        or metadata.get("mode") not in ("x86-64", "i386")
        or not isinstance(tests, list)
        or metadata.get("test_count") != len(tests)
    ):
        fail("corpus metadata has the wrong schema or test count")
    for record in tests:
        if not isinstance(record, dict):
            fail("corpus contains a malformed test record")
        binary = corpus_path(root, record.get("binary"))
        config = corpus_path(root, record.get("config"))
        if sha256_file(binary) != record.get("binary_sha256"):
            fail(f"binary hash mismatch: {record.get('test')}")
        if sha256_file(config) != record.get("config_sha256"):
            fail(f"configuration hash mismatch: {record.get('test')}")
    return metadata, tests


def full_precision_config(config: bytes) -> bytes | None:
    """Return CONFIG without FEX's reduced-precision x87 request.

    Returns None when CONFIG carries no request, or when its record is
    malformed, which the runner then reports.
    """
    try:
        rewritten, requested = fex_config.without_reduced_precision(config)
    except ValueError:
        return None
    return rewritten if requested else None


def parse_runner_output(output: str, binary: str) -> dict[str, object]:
    """Parse the runner's one tab-separated result line for BINARY.

    The runner names the case in an optional `test` field; it must name the
    binary this campaign passed, so a result can never be credited to another
    input.
    """
    line = output.strip().splitlines()
    if len(line) != 1:
        return {
            "status": "runner_error",
            "retired": 0,
            "address": "0x0",
            "opcode": "",
            "detail": "runner did not emit exactly one result line",
        }
    fields: dict[str, str] = {}
    for item in line[0].split("\t"):
        key, separator, value = item.partition("=")
        if not separator or key in fields:
            return {
                "status": "runner_error",
                "retired": 0,
                "address": "0x0",
                "opcode": "",
                "detail": "runner emitted malformed fields",
            }
        fields[key] = value
    required = {"status", "retired", "address", "opcode", "detail"}
    if set(fields) - {"test"} != required:
        return {
            "status": "runner_error",
            "retired": 0,
            "address": "0x0",
            "opcode": "",
            "detail": "runner result fields differ from the contract",
        }
    if fields.get("test", binary) != binary:
        return {
            "status": "runner_error",
            "retired": 0,
            "address": "0x0",
            "opcode": "",
            "detail": "runner result names another test",
        }
    try:
        retired = int(fields["retired"])
        int(fields["address"], 0)
        bytes.fromhex(fields["opcode"])
    except ValueError:
        return {
            "status": "runner_error",
            "retired": 0,
            "address": "0x0",
            "opcode": "",
            "detail": "runner result has an invalid numeric field",
        }
    return {
        "status": fields["status"],
        "retired": retired,
        "address": fields["address"],
        "opcode": fields["opcode"],
        "detail": fields["detail"],
    }


def opcode_key(encoded: object) -> str:
    try:
        data = bytes.fromhex(str(encoded))
    except ValueError:
        return ""
    offset = 0
    legacy = {0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65, 0x66, 0x67, 0xF0, 0xF2, 0xF3}
    while offset < len(data) and (
        data[offset] in legacy or 0x40 <= data[offset] <= 0x4F
    ):
        offset += 1
    if offset >= len(data):
        return data.hex()
    first = data[offset]
    length = 1
    if first == 0x0F and offset + 1 < len(data):
        length = 2
        if data[offset + 1] in (0x38, 0x3A) and offset + 2 < len(data):
            length = 3
    elif first == 0xC5:
        length = 3
    elif first in (0xC4, 0x62):
        length = 4
    return data[offset : offset + length].hex()


def run_one(
    runner: pathlib.Path,
    root: pathlib.Path,
    record: dict[str, Any],
    timeout: float,
    scratch: pathlib.Path,
    index: int,
) -> dict[str, object]:
    started = time.monotonic()
    binary = str(corpus_path(root, record["binary"]))
    config = corpus_path(root, record["config"])
    rewritten = full_precision_config(config.read_bytes())
    if rewritten is not None:
        config = scratch / f"{index}.config.bin"
        config.write_bytes(rewritten)
    command = [
        str(runner),
        binary,
        str(config),
    ]
    try:
        completed = subprocess.run(
            command,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout,
        )
        result = parse_runner_output(completed.stdout, binary)
        result["runner_returncode"] = completed.returncode
        if completed.stderr:
            result["runner_stderr"] = completed.stderr[-4096:]
    except subprocess.TimeoutExpired:
        result = {
            "status": "timeout",
            "retired": 0,
            "address": "0x0",
            "opcode": "",
            "detail": f"runner exceeded {timeout:g} seconds",
            "runner_returncode": None,
        }
    result.update(
        {
            "test": record["test"],
            "source": record["source"],
            "host_features": record.get("host_features", []),
            "disabled_upstream": bool(record.get("disabled_upstream")),
            "x87_reduced_precision_dropped": rewritten is not None,
            "duration_ms": round((time.monotonic() - started) * 1000, 3),
        }
    )
    if result["status"] == "illegal_instruction":
        result["opcode_key"] = opcode_key(result["opcode"])
    return result


def atomic_json(path: pathlib.Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", dir=path.parent
    )
    temporary = pathlib.Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w") as output:
            json.dump(value, output, indent=2, sort_keys=True)
            output.write("\n")
        os.replace(temporary, path)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runner", type=pathlib.Path)
    parser.add_argument("corpus", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 1, 32))
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--filter", help="regular expression matched against test names")
    arguments = parser.parse_args()
    runner = arguments.runner.resolve()
    root = arguments.corpus.resolve()
    if not runner.is_file() or not os.access(runner, os.X_OK):
        fail(f"runner is not executable: {runner}")
    if arguments.jobs < 1 or arguments.timeout <= 0:
        fail("jobs and timeout must be positive")
    metadata, tests = load_corpus(root)
    if arguments.filter:
        try:
            expression = re.compile(arguments.filter)
        except re.error as error:
            fail(f"invalid filter: {error}")
        tests = [record for record in tests if expression.search(record["test"])]
    if not tests:
        fail("no tests selected")

    results_by_name: dict[str, dict[str, object]] = {}
    with tempfile.TemporaryDirectory(prefix="fex-asm-configs-") as scratch, \
            concurrent.futures.ThreadPoolExecutor(max_workers=arguments.jobs) as pool:
        futures = {
            pool.submit(
                run_one, runner, root, record, arguments.timeout,
                pathlib.Path(scratch), index,
            ): record
            for index, record in enumerate(tests)
        }
        for future in concurrent.futures.as_completed(futures):
            result = future.result()
            results_by_name[str(result["test"])] = result
    results = [results_by_name[record["test"]] for record in tests]

    applicable = [result for result in results if not result["disabled_upstream"]]
    disabled = [result for result in results if result["disabled_upstream"]]
    status_counts = Counter(str(result["status"]) for result in applicable)
    disabled_status_counts = Counter(str(result["status"]) for result in disabled)
    opcode_counts = Counter(
        str(result.get("opcode_key", ""))
        for result in applicable
        if result["status"] == "illegal_instruction"
    )
    opcode_counts.pop("", None)
    passed = status_counts.get("passed", 0)
    report = {
        "schema": 1,
        "source": metadata.get("source"),
        "version": metadata.get("version"),
        "commit": metadata.get("commit"),
        "mode": metadata.get("mode"),
        "runner": str(runner),
        "selected_count": len(results),
        "applicable_count": len(applicable),
        "upstream_disabled_count": len(disabled),
        "x87_reduced_precision_dropped_count": sum(
            1 for result in results if result["x87_reduced_precision_dropped"]
        ),
        "passed_count": passed,
        "failed_count": len(applicable) - passed,
        "status_counts": dict(sorted(status_counts.items())),
        "upstream_disabled_status_counts": dict(
            sorted(disabled_status_counts.items())
        ),
        "illegal_opcode_counts": dict(
            sorted(opcode_counts.items(), key=lambda item: (-item[1], item[0]))
        ),
        "results": results,
    }
    atomic_json(arguments.output.resolve(), report)
    print(
        f"FEX {metadata.get('version')} {metadata.get('mode')}: "
        f"{passed}/{len(applicable)} applicable tests passed; "
        f"ran {len(disabled)} upstream-disabled tests; "
        f"report: {arguments.output.resolve()}"
    )
    print("statuses: " + ", ".join(f"{key}={value}" for key, value in sorted(status_counts.items())))
    if opcode_counts:
        top = ", ".join(
            f"{key}={value}" for key, value in opcode_counts.most_common(12)
        )
        print(f"top illegal opcode maps: {top}")
    return 0 if passed == len(applicable) else 1


if __name__ == "__main__":
    raise SystemExit(main())
