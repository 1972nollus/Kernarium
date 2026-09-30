#!/usr/bin/env python3
"""Check the user RFLAGS contract of the x86_64-userspace slice.

Cross-builds the freestanding rflags_probe.cpp for x86-64 and i386, boots an
x86-64 image on an emulator built with the macos-x86_64 preset, and runs the
probes there. As on Linux x86, execve() starts a task with RFLAGS 0x202, a
tracer changes only ptrace's FLAG_MASK bits, and sigreturn restores only
FIX_EFLAGS; IOPL, VM, VIF, VIP and ID stay the task's, and IF stays set. RF
from IRET or sigreturn is gone by the next instruction's system call, and TF
set by IRET traps after the instruction IRET returns to.
"""

from __future__ import annotations

# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
import pathlib
import subprocess
import sys
import tempfile


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest, GuestFile  # noqa: E402


PROBE = pathlib.Path(__file__).with_name("rflags_probe.cpp")
GUEST_DIRECTORY = "/usr/local/libexec/uml-test"
COMMON_FLAGS = (
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
# The x86-64 probe reads the flags with pushfq below its stack pointer.
PROBES = {
    "rflags-probe": ("--target=x86_64-unknown-linux-gnu", "-mno-red-zone"),
    "rflags-probe32": ("--target=i686-unknown-linux-gnu", "-march=i686"),
}
BUILD_TIMEOUT_SECONDS = 120
PROBE_TIMEOUT_SECONDS = 120


def build_probes(compiler: str) -> dict[str, bytes]:
    binaries = {}
    with tempfile.TemporaryDirectory(prefix="uml-rflags-probe-") as directory:
        for name, target_flags in PROBES.items():
            output = pathlib.Path(directory) / name
            subprocess.run(
                [compiler, *target_flags, *COMMON_FLAGS, str(PROBE),
                 "-o", str(output)],
                check=True,
                stdin=subprocess.DEVNULL,
                timeout=BUILD_TIMEOUT_SECONDS,
            )
            binaries[name] = output.read_bytes()
    return binaries


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("gadgets", "jit"), default="gadgets")
    parser.add_argument(
        "--compiler",
        default="clang++",
        help="clang driver with x86-64 and i386 Linux targets and ld.lld",
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

    binaries = build_probes(arguments.compiler)
    with Guest(
        emulator,
        rootfs,
        "512M",
        "tmpfs",
        init_shell=True,
        engine=arguments.engine,
        payloads={
            f"{GUEST_DIRECTORY}/{name}": GuestFile(contents, 0o755)
            for name, contents in binaries.items()
        },
    ) as guest:
        guest.boot_to_shell(arguments.timeout)
        result = guest.run_command(
            f"{GUEST_DIRECTORY}/rflags-probe {GUEST_DIRECTORY}/rflags-probe32",
            PROBE_TIMEOUT_SECONDS,
        )
        print(result.output.strip())
        if result.return_code != 0:
            raise RuntimeError(
                f"the RFLAGS probe reported {result.return_code} failure(s)"
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
