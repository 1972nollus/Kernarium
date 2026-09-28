#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run one guest Python probe and fail on its exit status.

The guest boots an init shell on a tmpfs root with /tmp mounted, runs the
probe with python3 -B, prints its output, and then runs one more command, so
a probe that leaves the kernel unable to start a task fails too. Each probe's
docstring states what it checks; its exit status is its failure count.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import pathlib
import sys


sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from guest import Guest, GuestFile  # noqa: E402


GUEST_PROBE = "/usr/local/libexec/uml-test/probe.py"


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
    arguments = parser.parse_args()

    with Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        "1G",
        "tmpfs",
        (f"ncpus={arguments.ncpus}",),
        init_shell=True,
        engine=arguments.engine,
        payloads={GUEST_PROBE: GuestFile(arguments.probe.read_bytes())},
    ) as guest:
        guest.boot_to_shell(180)
        guest.check_command("mount -t tmpfs -o mode=1777 tmpfs /tmp", 20)
        result = guest.run_command(
            f"python3 -B {GUEST_PROBE}", arguments.probe_timeout
        )
        print(result.output.strip())
        if result.return_code != 0:
            raise RuntimeError(
                f"{arguments.probe.name} reported {result.return_code} "
                "failure(s)"
            )
        guest.check_command("echo alive", 20)
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
