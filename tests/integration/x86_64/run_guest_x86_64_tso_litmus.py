#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check that x86-64 and i386 guests keep x86's memory order across vCPUs.

Cross-builds the freestanding tso_litmus_probe.cpp for x86-64 and i386, boots
an x86-64 image with four vCPUs on an emulator built with the macos-x86_64
preset, and runs message passing (also with the variables in a thread's own
stack frame), store buffering with and without MFENCE, and IRIW, between the
threads of one process and between processes that share SysV shm, a memfd
mapped MAP_SHARED or inherited shared anonymous memory. No test may see an
outcome x86 forbids.

The negative control, which shows that message passing catches a space
that keeps no order, is the unit test x86-64-jit-store-order: it drives the
JIT engine directly, so the product carries no setting that relaxes a
shared address space.
"""

from __future__ import annotations

# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
import pathlib
import re
import subprocess
import sys
import tempfile


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest, GuestFile  # noqa: E402


PROBE = pathlib.Path(__file__).with_name("tso_litmus_probe.cpp")
GUEST_DIRECTORY = "/usr/local/libexec/uml-test"
COMMON_FLAGS = (
    "-std=c++20",
    "-O2",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-ffreestanding",
    "-fno-exceptions",
    "-fno-rtti",
    "-fno-stack-protector",
    "-fno-asynchronous-unwind-tables",
    "-fno-pic",
    "-nostdlib",
    "-static",
    "-fuse-ld=lld",
    "-Wl,-e,_start",
    "-Wl,-z,max-page-size=4096",
    "-Wl,-s",
)
PROBES = {
    "tso-litmus-probe": ("x86-64", ("--target=x86_64-unknown-linux-gnu", "-mno-red-zone")),
    "tso-litmus-probe32": ("i386", ("--target=i686-unknown-linux-gnu", "-march=i686")),
}
CASES = ("thread", "shm", "memfd", "anon")
TESTS = {
    "thread": ("mp", "mp-stack-load", "mp-stack-store", "sb", "sb-mfence", "iriw"),
    "shm": ("mp", "sb", "sb-mfence", "iriw"),
    "memfd": ("mp", "sb", "sb-mfence", "iriw"),
    "anon": ("mp", "sb", "sb-mfence", "iriw"),
}
RESULT = re.compile(
    r"tso-litmus: abi=(\S+) case=(\S+) test=(\S+) rounds=(\d+) "
    r"(forbidden|allowed)=(\d+)"
)
CPUS = 4
BUILD_TIMEOUT_SECONDS = 120


def build_probes(compiler: str) -> dict[str, bytes]:
    binaries = {}
    with tempfile.TemporaryDirectory(prefix="uml-tso-litmus-") as directory:
        for name, (_, target_flags) in PROBES.items():
            output = pathlib.Path(directory) / name
            subprocess.run(
                [compiler, *target_flags, *COMMON_FLAGS, str(PROBE), "-o", str(output)],
                check=True,
                stdin=subprocess.DEVNULL,
                timeout=BUILD_TIMEOUT_SECONDS,
            )
            binaries[name] = output.read_bytes()
    return binaries


def run_cases(
    arguments: argparse.Namespace,
    binaries: dict[str, bytes],
    cases: tuple[str, ...],
    rounds: int,
) -> dict[tuple[str, str, str], int]:
    """Runs every case under both ABIs in one boot; returns the count each
    (abi, case, test) printed, after checking that each printed once."""
    counts: dict[tuple[str, str, str], int] = {}
    with Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        "1G",
        "tmpfs",
        (f"ncpus={CPUS}",),
        init_shell=True,
        engine=arguments.engine,
        payloads={
            f"{GUEST_DIRECTORY}/{name}": GuestFile(contents, 0o755)
            for name, contents in binaries.items()
        },
    ) as guest:
        guest.boot_to_shell(arguments.timeout)
        for name, (abi, _) in PROBES.items():
            for case in cases:
                result = guest.run_command(
                    f"{GUEST_DIRECTORY}/{name} {case} {rounds}",
                    arguments.probe_timeout,
                )
                output = result.output.strip()
                print(output, flush=True)
                if result.return_code not in (0, 1):
                    raise RuntimeError(f"{abi} {case}: the probe failed: {output!r}")
                for match in RESULT.finditer(output):
                    key = (match.group(1), match.group(2), match.group(3))
                    if key in counts or int(match.group(4)) != rounds:
                        raise RuntimeError(f"unexpected result line: {match.group(0)}")
                    counts[key] = int(match.group(6))
                expected = {(abi, case, test) for test in TESTS[case]}
                missing = expected - counts.keys()
                if missing:
                    raise RuntimeError(f"{abi} {case}: no result for {sorted(missing)}")
    return counts


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("gadgets", "jit"), default="gadgets")
    parser.add_argument(
        "--compiler",
        default="clang++",
        help="clang driver with x86-64 and i386 Linux targets and ld.lld",
    )
    parser.add_argument("--rounds", type=int, default=1_000_000,
                        help="rounds of each test in each case and ABI")
    parser.add_argument("--timeout", type=int, default=180, help="boot timeout")
    parser.add_argument("--probe-timeout", type=int, default=900,
                        help="timeout of one case under one ABI")
    arguments = parser.parse_args()

    emulator = arguments.emulator.resolve()
    if not emulator.is_file() or not os.access(emulator, os.X_OK):
        parser.error(f"emulator is not executable: {emulator}")
    if not arguments.rootfs.resolve().is_file():
        parser.error(f"rootfs is not a regular file: {arguments.rootfs}")
    if min(arguments.rounds, arguments.timeout, arguments.probe_timeout) <= 0:
        parser.error("rounds and timeouts must be positive")

    binaries = build_probes(arguments.compiler)
    failures = 0
    counts = run_cases(arguments, binaries, CASES, arguments.rounds)
    for (abi, case, test), count in sorted(counts.items()):
        if test == "sb":
            continue
        passed = count == 0
        failures += not passed
        print(f"{'PASS' if passed else 'FAIL'} {abi} {case} {test}: "
              f"{count} forbidden outcome(s) in {arguments.rounds} rounds")

    if failures:
        raise RuntimeError(f"{failures} check(s) failed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
