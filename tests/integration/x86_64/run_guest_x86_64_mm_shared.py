#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the "mm is shared" dispatch flag of the x86_64-userspace slice.

The kernel marks an address space shared once clone(CLONE_VM) without
CLONE_VFORK gives it a second task, once it maps memory another address
space may map too (MAP_SHARED of any kind, SysV shm), which a fork child
inherits, or once io_uring keeps a ring in its memory. The host reads the
mark from each dispatch (DARWIN_UML_DISPATCH_MM_SHARED) for the x86-64 JIT.
Cross-builds the freestanding mm_shared_probe.cpp, boots an x86-64 image on
an emulator built with the macos-x86_64 preset with UML_X86_MM_SHARED_LOG
set, and runs the probe in each mode. The host logs the first shared
dispatch of each address space with its RIP; in the probe's code there must
be none for the modes that share nothing and one per shared address space
for the others.
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
LOG_LINE = re.compile(r"uml-x86 mm-shared generation=(\d+) rip=(0x[0-9a-f]+|0)")
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


def logged(log: pathlib.Path) -> list[tuple[int, int]]:
    if not log.exists():
        return []
    entries = []
    for line in log.read_text().splitlines():
        match = LOG_LINE.fullmatch(line)
        if match is None:
            raise RuntimeError(f"unexpected log line: {line!r}")
        entries.append((int(match.group(1)), int(match.group(2), 16)))
    return entries


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
                        if start <= entry[1] < end]
                passed = len(hits) == shared
                failures += not passed
                print(f"{'PASS' if passed else 'FAIL'} {mode}: {len(hits)} shared "
                      f"address space(s) in the probe, expected {shared}")
    if failures:
        raise RuntimeError(f"{failures} mode(s) failed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
