#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check that a TLB replay's runs map each page to its own frame and protection.

The kernel sends neighbouring user PTEs to the host as one call when they are
all absent or map consecutive offsets of one backing with one protection. The
guest builds tlb_runs_probe, which lays read-only and writable blocks of 1 to
130 pages across a PMD boundary and the 64-page batch limit, in private
anonymous and shared file memory, punches holes, lets a fork's replay walk
shared blocks whole, and changes protections from a second thread while it
checks, then verifies that every page holds its own contents and that a store
faults exactly on a read-only page.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import pathlib
import re
import sys

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))
from guest import Guest, GuestFile  # noqa: E402

PROBE = pathlib.Path(__file__).with_name("tlb_runs_probe.cpp")
GUEST_SOURCE = "/usr/local/libexec/uml-test/tlb_runs_probe.cpp"
GUEST_BINARY = "/tmp/tlb-runs-probe"
BUILD_TIMEOUT_SECONDS = 180
PROBE_TIMEOUT_SECONDS = 300
EXPECTED = ("blocks", "file", "unmap", "fork", "churn")
RESULT_LINE = re.compile(r"^RESULT (\S+) (PASS|FAIL) (.*)$")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    arguments = parser.parse_args()

    with Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        "1G",
        "tmpfs",
        ("ncpus=2",),
        init_shell=True,
        engine=arguments.engine,
        payloads={GUEST_SOURCE: GuestFile(PROBE.read_bytes())},
    ) as guest:
        guest.boot_to_shell(180)
        guest.check_command("mount -t tmpfs -o mode=1777 tmpfs /tmp", 20)
        guest.check_command(
            f"c++ -std=c++20 -O2 -Wall -Wextra -Werror -pthread "
            f"-o {GUEST_BINARY} {GUEST_SOURCE}",
            BUILD_TIMEOUT_SECONDS,
        )
        result = guest.run_command(GUEST_BINARY, PROBE_TIMEOUT_SECONDS)
        print(result.output.strip(), flush=True)

    results: dict[str, str] = {}
    for line in result.output.splitlines():
        match = RESULT_LINE.match(line.strip())
        if match:
            results[match.group(1)] = match.group(2)
    failed = sorted(name for name, state in results.items() if state != "PASS")
    missing = sorted(set(EXPECTED) - set(results))
    problems = []
    if failed:
        problems.append(f"failed: {', '.join(failed)}")
    if missing:
        problems.append(f"missing: {', '.join(missing)}")
    if result.return_code != 0 and not failed:
        problems.append(f"the probe exited with {result.return_code}")
    if problems:
        raise RuntimeError("; ".join(problems))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
