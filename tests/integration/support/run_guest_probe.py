#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run one guest probe and fail on its exit status.

The guest boots an init shell on a tmpfs root with /tmp mounted, runs the
probe, prints its output, and then runs one more command, so a probe that
leaves the kernel unable to start a task fails too. A .py probe runs with
python3 -B. A .cpp probe is installed as source, built in the guest with
c++ -std=c++20 -O2 and --cxxflags, and run. Each probe's header states what
it checks; its exit status is its failure count. --console-failure also
fails the run on a console line, the kernel's messages included, that matches.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import pathlib
import re
import shlex

# The script's own directory, the harness's, is first on sys.path.
from guest import Guest, GuestFile


GUEST_DIRECTORY = "/usr/local/libexec/uml-test"
GUEST_BINARY = "/tmp/probe"
BUILD_TIMEOUT_SECONDS = 180


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    parser.add_argument("--probe", required=True, type=pathlib.Path)
    parser.add_argument("--ncpus", required=True, type=int)
    parser.add_argument(
        "--probe-timeout", required=True, type=int, metavar="SECONDS"
    )
    parser.add_argument(
        "--cxxflags",
        default="",
        help='compiler flags of a .cpp probe, as --cxxflags="FLAG FLAG"',
    )
    parser.add_argument(
        "--console-failure",
        type=re.compile,
        metavar="REGEX",
        help="fail when a console line matches",
    )
    arguments = parser.parse_args()
    compiled = arguments.probe.suffix == ".cpp"
    if arguments.cxxflags and not compiled:
        parser.error("--cxxflags applies to a .cpp probe")
    pattern = arguments.console_failure
    guest_probe = f"{GUEST_DIRECTORY}/probe{arguments.probe.suffix}"

    with Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        "1G",
        "tmpfs",
        (f"ncpus={arguments.ncpus}",),
        init_shell=True,
        engine=arguments.engine,
        payloads={guest_probe: GuestFile(arguments.probe.read_bytes())},
    ) as guest:
        guest.boot_to_shell(180)
        guest.check_command("mount -t tmpfs -o mode=1777 tmpfs /tmp", 20)
        if compiled:
            guest.check_command(
                " ".join(
                    ["c++", "-std=c++20", "-O2"]
                    + shlex.split(arguments.cxxflags)
                    + ["-o", GUEST_BINARY, guest_probe]
                ),
                BUILD_TIMEOUT_SECONDS,
            )
            command = GUEST_BINARY
        else:
            command = f"python3 -B {guest_probe}"
        result = guest.run_command(command, arguments.probe_timeout)
        print(result.output.strip())
        complaints = [
            line.strip()
            for line in guest.output().splitlines()
            if pattern and pattern.search(line)
        ]
        for line in complaints:
            print(f"FAIL console: {line}")
        problems = []
        if result.return_code != 0:
            problems.append(
                f"{arguments.probe.name} reported {result.return_code} "
                "failure(s)"
            )
        if complaints:
            problems.append(f"{len(complaints)} console line(s) matched")
        if problems:
            raise RuntimeError("; ".join(problems))
        guest.check_command("echo alive", 20)
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
