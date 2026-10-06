#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the "mm is shared" dispatch flag of the x86_64-userspace slice.

The kernel marks an address space shared once clone(CLONE_VM) without
CLONE_VFORK gives it a second task, once it maps memory another address
space may map too (MAP_SHARED of any kind, SysV shm), which a fork child
inherits, or once io_uring keeps a ring in its memory. The host reads the
mark from each dispatch (DARWIN_UML_DISPATCH_MM_SHARED), and the x86-64 JIT
keeps x86's store order for a marked address space only. Cross-builds the
freestanding mm_shared_probe.cpp, boots an x86-64 image on an emulator built
with the macos-x86_64 preset with UML_X86_MM_SHARED_LOG set, and runs the
probe in each mode. The host logs the first shared dispatch of each address
space with its RIP; in the probe's code there must be none for the modes
that share nothing and one per shared address space for the others.

On the JIT, the log also records the first dispatch of each address space
that ran unordered code and the first that ran ordered code: an unshared
probe must run only unordered code, a shared one must run ordered code, and
no address space may run unordered code once it has run ordered code.
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


PROBE = pathlib.Path(__file__).with_name("mm_shared_probe.cpp")
GUEST_PROBE = "/usr/local/libexec/uml-test/mm-shared-probe"
FLAGS = (
    "--target=x86_64-unknown-linux-gnu",
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
# Each mode with the number of address spaces it shares: the probe's own,
# and for fork-shared its child's as well.
MODES = {
    "single": 0,
    "fork": 0,
    "vfork": 0,
    "private-file": 0,
    "thread": 1,
    "shared-anon": 1,
    "shared-ro": 1,
    "shm": 1,
    "uring-nommap": 1,
    "fork-shared": 2,
    "thread-fork": 1,
}
RANGE = re.compile(r"mm-shared-probe: mode=([\w-]+) range=(0x[0-9a-f]+)-(0x[0-9a-f]+)")
LOG_LINE = re.compile(
    r"uml-x86 (mm-shared|jit-space|jit-regressed) generation=(\d+)"
    r"(?: ordered=([01]))? rip=(0x[0-9a-f]+|0)"
)
BUILD_TIMEOUT_SECONDS = 120
PROBE_TIMEOUT_SECONDS = 60


def build_probe(compiler: str, directory: pathlib.Path) -> bytes:
    output = directory / "mm-shared-probe"
    subprocess.run(
        [compiler, *FLAGS, str(PROBE), "-o", str(output)],
        check=True,
        stdin=subprocess.DEVNULL,
        timeout=BUILD_TIMEOUT_SECONDS,
    )
    return output.read_bytes()


def logged(log: pathlib.Path) -> list[tuple[str, int, int | None, int]]:
    """(event, generation, ordered, rip) for each line of the host's log."""
    if not log.exists():
        return []
    entries = []
    for line in log.read_text().splitlines():
        match = LOG_LINE.fullmatch(line)
        if match is None or (match.group(1) == "mm-shared") != (match.group(3) is None):
            raise RuntimeError(f"unexpected log line: {line!r}")
        ordered = None if match.group(3) is None else int(match.group(3))
        entries.append((match.group(1), int(match.group(2)), ordered,
                        int(match.group(4), 16)))
    return entries


def check_jit(mode: str, shared: int, hits: list[tuple[str, int, int | None, int]]) -> str | None:
    """The JIT's part of one mode's verdict, or None when it passes."""
    unordered = {entry[1] for entry in hits if entry[0] == "jit-space" and entry[2] == 0}
    ordered = {entry[1] for entry in hits if entry[0] == "jit-space" and entry[2] == 1}
    if not unordered and not ordered:
        return "the JIT logged no dispatch in the probe"
    if shared == 0 and ordered:
        return "an unshared probe ran ordered code"
    if shared and len(ordered) != shared:
        return f"{len(ordered)} address space(s) ran ordered code, expected {shared}"
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("gadgets", "jit"), default="gadgets")
    parser.add_argument(
        "--compiler",
        default="clang++",
        help="clang driver with an x86-64 Linux target and ld.lld",
    )
    parser.add_argument("--timeout", type=int, default=180)
    arguments = parser.parse_args()

    emulator = arguments.emulator.resolve()
    rootfs = arguments.rootfs.resolve()
    if not emulator.is_file() or not os.access(emulator, os.X_OK):
        parser.error(f"emulator is not executable: {emulator}")
    if not rootfs.is_file():
        parser.error(f"rootfs is not a regular file: {rootfs}")
    if arguments.timeout <= 0:
        parser.error("timeout must be positive")

    failures = 0
    with tempfile.TemporaryDirectory(prefix="uml-mm-shared-") as directory:
        probe = build_probe(arguments.compiler, pathlib.Path(directory))
        log = pathlib.Path(directory) / "mm-shared.log"
        os.environ["UML_X86_MM_SHARED_LOG"] = str(log)
        with Guest(
            emulator,
            rootfs,
            "512M",
            "tmpfs",
            init_shell=True,
            engine=arguments.engine,
            payloads={GUEST_PROBE: GuestFile(probe, 0o755)},
        ) as guest:
            guest.boot_to_shell(arguments.timeout)
            for mode, shared in MODES.items():
                before = len(logged(log))
                result = guest.run_command(f"{GUEST_PROBE} {mode}",
                                           PROBE_TIMEOUT_SECONDS)
                output = result.output.strip()
                print(output)
                match = RANGE.search(output)
                if result.return_code != 0 or match is None or match.group(1) != mode:
                    raise RuntimeError(f"{mode}: the probe failed: {output!r}")
                start, end = int(match.group(2), 16), int(match.group(3), 16)
                hits = [entry for entry in logged(log)[before:]
                        if start <= entry[3] < end]
                marked = sum(entry[0] == "mm-shared" for entry in hits)
                problem = None
                if marked != shared:
                    problem = (f"{marked} shared address space(s) in the probe, "
                               f"expected {shared}")
                elif arguments.engine == "jit":
                    problem = check_jit(mode, shared, hits)
                failures += problem is not None
                print(f"FAIL {mode}: {problem}" if problem else
                      f"PASS {mode}: {shared} shared address space(s)")
        regressed = [entry for entry in logged(log) if entry[0] == "jit-regressed"]
        for entry in regressed:
            print(f"FAIL generation {entry[1]} ran unordered code at {entry[3]:#x} "
                  "after ordered code")
        failures += len(regressed)
    if failures:
        raise RuntimeError(f"{failures} mode(s) failed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
