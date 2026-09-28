#!/usr/bin/env python3
"""Check that unmapping user memory beside the native window keeps the guest.

The kernel's native window sits inside every task's address space, and no user
mapping may enter it. A task can still map pages that touch it from either side
and unmap them with one munmap() whose range spans it. The TLB flush that
follows covers the page-table pages freed on both sides, and a page table that
maps the first page above the window starts inside it. The guest probe unmaps
below, above and across the window, and the guest must survive each unmap and
keep running commands afterwards.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import pathlib
import sys


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest  # noqa: E402


PROBE = pathlib.Path(__file__).with_name("native_window_unmap_probe.py")
PROBE_TIMEOUT_SECONDS = 120


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
    ) as guest:
        guest.boot_to_shell(180)
        guest.check_command(
            "mount -t proc proc /proc && mount -t tmpfs -o mode=1777 tmpfs /tmp",
            20,
        )
        guest.upload(PROBE.read_bytes(), "/tmp/probe.py")
        result = guest.run_command("python3 -B /tmp/probe.py", PROBE_TIMEOUT_SECONDS)
        print(result.output.strip())
        if result.return_code != 0:
            raise RuntimeError(
                f"the native-window probe reported {result.return_code} failure(s)"
            )
        # The unmaps must leave the kernel able to run another task.
        guest.check_command("echo alive", 20)
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
