#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the guest probe of the kernel's AArch32 task support.

The guest boots an init shell on a tmpfs root with /tmp mounted and runs
aarch32_guest_probe.py with python3 -B against the armhf program built from
aarch32_probe.S, in a layout for any page size and in one for 4 KiB pages
only, then runs one more command, so a probe that leaves the kernel unable to
start a task fails too.  The probe's docstring states what it checks; its
exit status is its failure count.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import pathlib
import sys

# The harness's directory.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "support"))

from guest import Guest, GuestFile  # noqa: E402


GUEST_PROBE = "/usr/local/libexec/uml-test/aarch32_probe.py"
GUEST_ELF = "/usr/local/libexec/uml-test/a32-probe"
GUEST_ELF_4K = "/usr/local/libexec/uml-test/a32-probe-4k"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    parser.add_argument("--probe", required=True, type=pathlib.Path)
    parser.add_argument("--elf", required=True, type=pathlib.Path)
    parser.add_argument("--elf-4k", required=True, type=pathlib.Path)
    parser.add_argument("--ncpus", type=int, default=2)
    parser.add_argument("--probe-timeout", type=int, metavar="SECONDS",
                        help="the probe's whole run; default 300 on the JIT "
                        "engine and 900 on the slower gadget engine")
    arguments = parser.parse_args()
    if arguments.probe_timeout is None:
        arguments.probe_timeout = 300 if arguments.engine == "jit" else 900

    with Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        "1G",
        "tmpfs",
        (f"ncpus={arguments.ncpus}",),
        init_shell=True,
        engine=arguments.engine,
        payloads={
            GUEST_PROBE: GuestFile(arguments.probe.read_bytes()),
            GUEST_ELF: GuestFile(arguments.elf.read_bytes(), 0o755),
            GUEST_ELF_4K: GuestFile(arguments.elf_4k.read_bytes(), 0o755),
        },
    ) as guest:
        guest.boot_to_shell(180)
        guest.check_command("mount -t tmpfs -o mode=1777 tmpfs /tmp", 20)
        result = guest.run_command(
            f"python3 -B {GUEST_PROBE} --elf {GUEST_ELF} "
            f"--elf-4k {GUEST_ELF_4K} "
            f"--deadline {arguments.probe_timeout - 20}",
            arguments.probe_timeout,
        )
        print(result.output.strip())
        if result.return_code != 0:
            raise RuntimeError(
                f"the AArch32 probe reported {result.return_code} failure(s)"
            )
        guest.check_command("echo alive", 20)
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
