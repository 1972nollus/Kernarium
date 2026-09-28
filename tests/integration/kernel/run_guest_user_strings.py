#!/usr/bin/env python3
"""Check the kernel's copies of user strings against their contract.

A system call that takes a path or a task name copies it from user memory
with strncpy_from_user(), which User-Mode Linux performs a page at a time.
The guest probe places strings at chosen offsets of its own mappings and
requires the documented results: length limits at the embedded name buffer
and PATH_MAX, strings that cross or end at a page boundary, EFAULT for
unreadable memory, and the exact bytes a symlink target and a task name keep.
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


PROBE = pathlib.Path(__file__).with_name("user_strings_probe.py")
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
                f"the user-string probe reported {result.return_code} failure(s)"
            )
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
