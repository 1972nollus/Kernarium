#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check that x86-64 and i386 processes run the code they hold, not the code
they held.

Cross-builds the freestanding code_change_probe.cpp for x86-64 and i386,
boots an x86-64 image with four vCPUs on an emulator built with the
macos-x86_64 preset, and runs both probes. Each changes the code of a
function the engine has already translated -- by a store from outside it or
from inside the unit that runs it, by mprotect round trips, by writing a page
a fork write-protected (copied, or reused once the other side is gone), by
MAP_FIXED, munmap and mremap, and from a second vCPU -- and every call after
the change must run the new bytes. One case has a thread patch two
instructions of a function with no link between the stores while three other
vCPUs run it: after the thread's flag and a system call each of them must run
both patches.
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


PROBE = pathlib.Path(__file__).with_name("code_change_probe.cpp")
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
    "code-change-probe": ("x86-64", ("--target=x86_64-unknown-linux-gnu", "-mno-red-zone")),
    "code-change-probe32": ("i386", ("--target=i686-unknown-linux-gnu", "-march=i686")),
}
RESULT = re.compile(r"code-change: abi=(\S+) checks=(\d+) failed=(\d+) verdict=(\S+)")
CPUS = 4
BUILD_TIMEOUT_SECONDS = 120
PROBE_TIMEOUT_SECONDS = 120


def build_probes(compiler: str) -> dict[str, bytes]:
    binaries = {}
    with tempfile.TemporaryDirectory(prefix="uml-code-change-probe-") as directory:
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
    parser.add_argument("--timeout", type=int, default=180, help="boot timeout")
    arguments = parser.parse_args()

    emulator = arguments.emulator.resolve()
    rootfs = arguments.rootfs.resolve()
    if not emulator.is_file() or not os.access(emulator, os.X_OK):
        parser.error(f"emulator is not executable: {emulator}")
    if not rootfs.is_file():
        parser.error(f"rootfs is not a regular file: {rootfs}")
    if arguments.timeout <= 0:
        parser.error("timeout must be positive")

    binaries = build_probes(arguments.compiler)
    failures = []
    with Guest(
        emulator,
        rootfs,
        "512M",
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
            result = guest.run_command(f"{GUEST_DIRECTORY}/{name}", PROBE_TIMEOUT_SECONDS)
            output = result.output.strip()
            print(output, flush=True)
            match = RESULT.search(output)
            if result.return_code != 0 or match is None or match.group(1) != abi \
                    or match.group(4) != "ok":
                failures.append(f"{abi}: exit status {result.return_code}")
    for failure in failures:
        print(f"FAIL {failure}")
    if failures:
        raise RuntimeError(f"{len(failures)} probe(s) failed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
