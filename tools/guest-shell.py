#!/usr/bin/env python3
"""Run shell commands in a guest without starting systemd.

Binary analysis, instruction sanity checks and one-off inspection do not need
a full systemd userspace - they need a root shell and the rootfs. Booting
systemd for them costs about ninety seconds of service startup, and the
benchmark runners' quiescence phase then has to stop most of it again.

This boots straight to a shell over the OverlayFS bootstrap instead, runs each
command, and prints the output.

    tools/guest-shell.py --emulator .build/presets/macos-integration/bin/uml-darwin \\
        -c 'objdump -d /opt/geekbench/... | head'

Wrap it in tools/rootfs/with-verified-rootfs.sh exactly like the other guest
entry points so the image digest is checked before and after.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import pathlib
import sys


TESTS = pathlib.Path(__file__).resolve().parents[1] / "tests"
sys.path.insert(0, str(TESTS / "integration" / "support"))
from guest import CORE_CLASSES, Guest  # noqa: E402


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="run shell commands in a guest without systemd",
    )
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument(
        "--rootfs", type=pathlib.Path, default=pathlib.Path("rootfs.erofs")
    )
    parser.add_argument(
        "-c",
        "--command",
        action="append",
        required=True,
        metavar="COMMAND",
        help="shell command to run; repeatable, executed in order",
    )
    parser.add_argument("--memory", default="4G")
    parser.add_argument("--cpus", type=int, default=4)
    parser.add_argument(
        "--engine", choices=("jit", "gadgets", "interpreter"), default="gadgets"
    )
    parser.add_argument("--core-class", choices=CORE_CLASSES, default="performance")
    parser.add_argument("--boot-timeout", type=int, default=300)
    parser.add_argument("--timeout", type=int, default=1800)
    parser.add_argument(
        "--root-device",
        default="/dev/ubda",
        help="raw EROFS root block device (default: /dev/ubda)",
    )
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    if arguments.cpus < 1 or arguments.cpus > 64:
        raise ValueError("--cpus must be between 1 and 64")
    if arguments.timeout < 1 or arguments.boot_timeout < 1:
        raise ValueError("timeouts must be positive")

    guest = Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        arguments.memory,
        "tmpfs",
        (
            f"ncpus={arguments.cpus}",
            f"root={arguments.root_device}",
        ),
        overlay_shell=True,
        engine=arguments.engine,
        core_class=arguments.core_class,
    )
    status = 0
    try:
        guest.boot_and_login(arguments.boot_timeout)
        for command in arguments.command:
            result = guest.run_command(command, arguments.timeout)
            print(result.output, flush=True)
            if result.return_code != 0:
                print(
                    f"command exited {result.return_code}: {command}",
                    file=sys.stderr,
                    flush=True,
                )
                status = result.return_code
        guest.shutdown(60)
    finally:
        guest.close()
    return status


if __name__ == "__main__":
    raise SystemExit(main())
