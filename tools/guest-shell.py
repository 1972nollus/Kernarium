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

The runner maps the EROFS image directly; writable state remains in its tmpfs
OverlayFS upper layer.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import pathlib
import sys
import threading
import time


TESTS = pathlib.Path(__file__).resolve().parents[1] / "tests"
sys.path.insert(0, str(TESTS / "integration" / "support"))
from guest import CORE_CLASSES, Guest  # noqa: E402


def mirror_console(guest: Guest, stop: threading.Event, ready: threading.Event) -> None:
    """Copy new console bytes to stdout while Guest owns the PTY reader.

    `run_command` only returns once its end marker arrives, so a long command
    like a full benchmark would otherwise produce nothing for its whole run and
    give no way to tell progress from a wedge. Guest appends everything it reads
    to `_transcript`, so following that buffer streams the console without
    competing for the descriptor.
    """
    offset = 0
    ready.set()
    while True:
        available = len(guest._transcript)
        if available > offset:
            sys.stdout.buffer.write(bytes(guest._transcript[offset:available]))
            sys.stdout.buffer.flush()
            offset = available
        elif stop.is_set():
            return
        time.sleep(0.1)


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
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="suppress the live console stream and print only command output",
    )
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
    stop = threading.Event()
    mirror: threading.Thread | None = None
    if not arguments.quiet:
        ready = threading.Event()
        mirror = threading.Thread(
            target=mirror_console,
            args=(guest, stop, ready),
            name="guest-console-mirror",
            daemon=True,
        )
        mirror.start()
        ready.wait()
    try:
        guest.boot_and_login(arguments.boot_timeout)
        for command in arguments.command:
            result = guest.run_command(command, arguments.timeout)
            if arguments.quiet:
                print(result.output, flush=True)
            if result.return_code != 0:
                print(
                    f"command exited {result.return_code}: {command}",
                    file=sys.stderr,
                    flush=True,
                )
                status = result.return_code
        shutdown_ok = guest.shutdown(60)
        if not shutdown_ok:
            print(
                "guest did not halt cleanly within 60 seconds",
                file=sys.stderr,
                flush=True,
            )
            if status == 0:
                status = 1
        else:
            emulator_status = guest.return_code
            if emulator_status != 0:
                print(
                    f"emulator exited with status {emulator_status} "
                    "during shutdown",
                    file=sys.stderr,
                    flush=True,
                )
                if status == 0:
                    status = 1
    finally:
        stop.set()
        if mirror is not None:
            mirror.join(timeout=5)
        guest.close()
    return status


if __name__ == "__main__":
    raise SystemExit(main())
