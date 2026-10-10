#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the AArch32 engine probe on one execution engine.

The probe (engine_probe/) is a freestanding armhf program that boots as the
guest's /init and checks, from user mode, what an AArch32 task sees of the
host engine: system calls and errno, A32/T32 interworking, signal frames with
VFP state, every kuser helper, TPIDRURO and TPIDRURW, cacheflush with
self-modifying code, alignment and memory faults, UNDEFINED instructions,
ptrace and single steps through an IT block, exclusives and kuser cmpxchg
under contention across threads and processes, and then the VFP, Advanced
SIMD and Armv8 extensions as far as the engine's level goes. A group needs a
least level and the probe skips it below that, so one program serves every
level; the level it detects from the HWCAP words must equal the engine's
(A32_PROBE_LEVEL).

An engine that implements no AArch32 yet is skipped, naming the reason, with
CTest's skip status. Otherwise the runner sets DARWIN_UML_GUEST_AARCH32=1, as
the development policy requires, boots the probe laid out for any page size
and, on a 4 KiB guest, the one laid out for 4 KiB pages, and fails on any
failed check, any SIGILL the probe did not expect (a guest illegal
instruction is a blocker) and any crash of the guest or the emulator.
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

# The harness's directory.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "support"))

from guest import ConsoleProcess, emulator_arguments  # noqa: E402


SKIP = 77
LEVELS = ("none", "base", "simd", "complete")
SUMMARY = re.compile(
    rb"a32-engine-probe: (?P<failures>\d+) failure\(s\) in (?P<checks>\d+) "
    rb"check\(s\), sigill expected (?P<expected>\d+) unexpected "
    rb"(?P<unexpected>\d+)"
)
DETECTED = re.compile(r"a32-engine-probe: level (\w+)")
# The probe's own lines are not host crashes, whatever they name.
PROBE_LINE = re.compile(r"^(PASS|FAIL|SKIP|NOTE) |^a32-engine-probe: ")
HOST_CRASH = re.compile(
    r"SIGILL|[Ii]llegal instruction|host fault|Kernel panic|"
    r"Attempted to kill init|segfault at")


def engine_level(tool: pathlib.Path, engine: str) -> str:
    output = subprocess.run(
        [str(tool)], capture_output=True, text=True, check=True,
        stdin=subprocess.DEVNULL, timeout=30,
    ).stdout
    return dict(field.split("=", 1) for field in output.split())[engine]


def run_probe(arguments: argparse.Namespace, label: str,
              elf: pathlib.Path, level: str) -> int:
    """Boot `elf` as /init and return the failures it reports."""
    kernel = [f"ncpus={arguments.ncpus}", f"A32_PROBE_LEVEL={level}"]
    if arguments.groups:
        kernel.append(f"A32_PROBE_GROUPS={arguments.groups}")
    command = emulator_arguments(
        arguments.emulator, arguments.rootfs, "512M",
        (*kernel, "console=tty0", "console=ttyS0", *arguments.kernel_argument),
        engine=arguments.engine,
    )
    # The launcher's per-run initramfs takes the probe as its /init.
    command.insert(command.index(str(arguments.emulator)) + 1,
                   f"--guest-init={elf}")
    print(f"== {arguments.engine}: {label}", flush=True)
    with ConsoleProcess(command) as guest:
        match = guest._read_until(SUMMARY, arguments.timeout)
        guest.wait_for_exit(60)
        output = guest.output()
    lines = output.splitlines()
    verbose = bool(os.environ.get("A32_PROBE_VERBOSE"))
    for line in lines:
        # Failures and skips are the news; the passes are a count, unless
        # A32_PROBE_VERBOSE asks for every line of the guest's output.
        if verbose or line.startswith(
                ("FAIL ", "SKIP ", "NOTE ", "a32-engine-probe: ")):
            print(line)
    crashes = [line for line in lines
               if HOST_CRASH.search(line) and not PROBE_LINE.match(line)]
    if crashes:
        print("\n".join(crashes))
        raise RuntimeError(f"{label}: the guest or the emulator crashed")
    detected = DETECTED.search(output)
    if detected is None or detected.group(1) != level:
        raise RuntimeError(
            f"{label}: the probe saw the level "
            f"{detected.group(1) if detected else 'nothing'}, not {level}")
    if int(match.group("checks")) == 0:
        raise RuntimeError(f"{label}: the probe ran no checks")
    failures = int(match.group("failures"))
    unexpected = int(match.group("unexpected"))
    print(f"{label}: {match.group('checks').decode()} check(s), "
          f"{failures} failure(s), SIGILLs expected "
          f"{match.group('expected').decode()} unexpected {unexpected}",
          flush=True)
    return failures + unexpected


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), required=True)
    parser.add_argument("--probe", required=True, type=pathlib.Path,
                        help="the probe laid out for any page size")
    parser.add_argument("--probe-4k", required=True, type=pathlib.Path,
                        help="the probe laid out for 4 KiB pages only")
    parser.add_argument("--page-size", required=True, type=int,
                        choices=(4096, 16384), help="the guest's page size")
    parser.add_argument("--engine-support-tool", required=True,
                        type=pathlib.Path)
    parser.add_argument("--groups", help="comma-separated probe groups; "
                        "default every group the level allows")
    parser.add_argument("--ncpus", type=int, default=4)
    parser.add_argument("--timeout", type=int, default=1200, metavar="SECONDS",
                        help="the longest a probe boot may take to report")
    parser.add_argument("--kernel-argument", action="append", default=[])
    arguments = parser.parse_args()
    arguments.emulator = arguments.emulator.resolve()
    arguments.rootfs = arguments.rootfs.resolve()
    name = f"guest-aarch32-engine-{arguments.engine}"

    level = engine_level(arguments.engine_support_tool, arguments.engine)
    if level == "none":
        print(f"{name}: SKIP: the {arguments.engine} engine implements no "
              "AArch32 yet (level none)")
        return SKIP

    os.environ["DARWIN_UML_GUEST_AARCH32"] = "1"
    failures = run_probe(arguments, "any page size layout",
                         arguments.probe.resolve(), level)
    if arguments.page_size == 4096:
        failures += run_probe(arguments, "4 KiB layout",
                              arguments.probe_4k.resolve(), level)
    if failures:
        raise RuntimeError(f"{name}: {failures} failure(s)")
    print(f"{name}: the probe passed at the {level} level")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
