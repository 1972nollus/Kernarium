#!/usr/bin/env python3
"""Check that a bad stack pointer reaches an arm64 task as native Linux's.

The guest builds stack_pointer_probe.cpp, which pins itself to one vCPU and
makes system calls and faults stores with SP at zero and at and above the end
of the task's address space, many times each. Every call must return and
every store must deliver SIGSEGV with SEGV_MAPERR at SP, with no task killed
and no kernel complaint.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import pathlib
import sys


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest, GuestFile  # noqa: E402


PROBE = pathlib.Path(__file__).with_name("stack_pointer_probe.cpp")
GUEST_SOURCE = "/usr/local/libexec/uml-test/stack_pointer_probe.cpp"
GUEST_BINARY = "/tmp/stack-pointer-probe"
BUILD_TIMEOUT_SECONDS = 180
PROBE_TIMEOUT_SECONDS = 120
# fatal_userspace() prints this before it kills a task the host failed.
KERNEL_COMPLAINT = "Darwin: "


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
            f"c++ -std=c++20 -O2 -o {GUEST_BINARY} {GUEST_SOURCE}",
            BUILD_TIMEOUT_SECONDS,
        )
        result = guest.run_command(GUEST_BINARY, PROBE_TIMEOUT_SECONDS)
        print(result.output.strip())
        complaints = [
            line.strip()
            for line in guest.output().splitlines()
            if KERNEL_COMPLAINT in line and "failed" in line
        ]
    for line in complaints:
        print(f"FAIL kernel: {line}")
    if result.return_code != 0 or complaints:
        raise RuntimeError(
            f"the stack-pointer probe reported {result.return_code} "
            f"failure(s) and the kernel {len(complaints)} complaint(s)"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
