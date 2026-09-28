#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the FEX campaign tool over the real gadget runner on a tiny corpus.

The campaign reads the runner's result line, so this runs the runner the
campaign documents, on generated FEX-format inputs, and requires every
terminal state to reach the report as the runner stated it.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import importlib.util
import json
import pathlib
import struct
import subprocess
import sys
import tempfile

# FEX's configuration record: four u64 and three u32 header fields (the
# execution mode is the second u32), then offset/count pairs for memory
# regions, registers, memory data and the environment.
CONFIG_HEADER_SIZE = 76
CONFIG_MODE = 36
CONFIG_REGISTERS = 52
CONFIG_ENVIRONMENT = 68
LONG_MODE = 1
# Register keys are one bit each: bit 0 is RIP, bits 1-16 the GPRs from RAX.
RAX_KEY = 1 << 1

MOV_EAX_42_HLT = bytes.fromhex("b82a000000f4")
UD2 = bytes.fromhex("0f0b")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"test_fex_asm_campaign.py: {message}")


def config_expecting_rax(
    value: int, environment: tuple[tuple[bytes, bytes], ...] = ()
) -> bytes:
    record = bytearray(CONFIG_HEADER_SIZE)
    struct.pack_into("<I", record, CONFIG_MODE, LONG_MODE)
    struct.pack_into("<II", record, CONFIG_REGISTERS, len(record), 1)
    record += struct.pack("<IQQ", 1, RAX_KEY, value)
    struct.pack_into("<II", record, CONFIG_ENVIRONMENT, len(record),
                     len(environment))
    for key, setting in environment:
        record += key + b"\0" + setting + b"\0"
    return bytes(record)


def write_corpus(root: pathlib.Path, cases: dict[str, tuple[bytes, bytes]]) -> None:
    tests = []
    for name, (binary, config) in cases.items():
        binary_path = root / "tests" / f"{name}.asm.bin"
        config_path = root / "tests" / f"{name}.asm.config.bin"
        binary_path.parent.mkdir(parents=True, exist_ok=True)
        binary_path.write_bytes(binary)
        config_path.write_bytes(config)
        tests.append(
            {
                "test": f"Test_{name}.asm",
                "source": f"{name}.asm",
                "binary": str(binary_path.relative_to(root)),
                "binary_sha256": hashlib.sha256(binary).hexdigest(),
                "config": str(config_path.relative_to(root)),
                "config_sha256": hashlib.sha256(config).hexdigest(),
                "disabled_upstream": False,
                "host_features": [],
            }
        )
    metadata = {
        "schema": 1,
        "mode": "x86-64",
        "source": "fixture",
        "version": "fixture",
        "commit": "fixture",
        "test_count": len(tests),
        "tests": tests,
    }
    (root / "corpus.json").write_text(json.dumps(metadata))


def load_campaign(path: pathlib.Path):
    specification = importlib.util.spec_from_file_location("run_asm_corpus", path)
    require(specification is not None and specification.loader is not None,
            f"cannot load {path}")
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


def check_parser(campaign) -> None:
    line = ("status=passed\t{test}retired=2\taddress=0x10005\t"
            "opcode=f4\tdetail=reached HLT with expected state\n")
    parsed = campaign.parse_runner_output(line.format(test=""), "/a.bin")
    require(parsed["status"] == "passed", "a line without a test field failed")
    parsed = campaign.parse_runner_output(
        line.format(test="test=/a.bin\t"), "/a.bin")
    require(parsed["status"] == "passed",
            f"the runner's test field was rejected: {parsed}")
    parsed = campaign.parse_runner_output(
        line.format(test="test=/b.bin\t"), "/a.bin")
    require(parsed["status"] == "runner_error",
            "a result naming another test was accepted")
    parsed = campaign.parse_runner_output(
        line.format(test="extra=1\t"), "/a.bin")
    require(parsed["status"] == "runner_error",
            "a result with an unknown field was accepted")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner", required=True, type=pathlib.Path)
    parser.add_argument("--campaign", required=True, type=pathlib.Path)
    arguments = parser.parse_args()

    cases = {
        "pass": (MOV_EAX_42_HLT, config_expecting_rax(42)),
        "mismatch": (MOV_EAX_42_HLT, config_expecting_rax(43)),
        "illegal": (UD2, config_expecting_rax(0)),
        # FEX's expected registers are native full-precision results, so the
        # campaign drops the reduced-precision x87 request the runner refuses.
        "reduced": (
            MOV_EAX_42_HLT,
            config_expecting_rax(
                42,
                ((b"FEX_X87REDUCEDPRECISION", b"1"), (b"OTHER", b"kept")),
            ),
        ),
    }
    expected = {
        "Test_pass.asm": ("passed", "reached HLT with expected state"),
        "Test_mismatch.asm": ("result_mismatch", "RAX expected=0x2b actual=0x2a"),
        "Test_illegal.asm": ("illegal_instruction", "decoder rejected opcode"),
        "Test_reduced.asm": ("passed", "reached HLT with expected state"),
    }
    with tempfile.TemporaryDirectory(prefix="fex-campaign-") as scratch:
        root = pathlib.Path(scratch) / "corpus"
        report_path = pathlib.Path(scratch) / "report.json"
        write_corpus(root, cases)
        completed = subprocess.run(
            [
                sys.executable, "-B", str(arguments.campaign),
                str(arguments.runner), str(root), str(report_path),
                "--jobs", "2", "--timeout", "30",
            ],
            check=False,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=120,
        )
        require(completed.returncode == 1,
                f"campaign exited {completed.returncode}: "
                f"{completed.stdout}{completed.stderr}")
        report = json.loads(report_path.read_text())
    require(report["applicable_count"] == 4 and report["passed_count"] == 2,
            f"campaign counted {report['passed_count']} of "
            f"{report['applicable_count']}")
    require(report["x87_reduced_precision_dropped_count"] == 1,
            "campaign did not count the dropped reduced-precision request")
    results = {result["test"]: result for result in report["results"]}
    for test, (status, detail) in expected.items():
        result = results[test]
        require((result["status"], result["detail"]) == (status, detail),
                f"{test}: {result['status']} ({result['detail']}), "
                f"expected {status} ({detail})")
    check_parser(load_campaign(arguments.campaign))
    print("FEX campaign reported the runner's pass, mismatch, illegal and "
          "full-precision results")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
