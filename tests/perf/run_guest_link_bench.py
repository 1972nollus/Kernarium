#!/usr/bin/env python3
"""Measure cached gadget-block link throughput in a standalone guest."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import base64
import os
import pathlib
import shlex
import statistics
import sys
import time


TESTS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TESTS / "integration" / "ltp"))
sys.path.insert(0, str(TESTS / "integration" / "smp"))
from run_guest_ltp import Guest  # noqa: E402
from run_guest_smp import cpu_time_seconds  # noqa: E402


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--root-device", default="/dev/ubda1")
    parser.add_argument("--binary", required=True, type=pathlib.Path)
    parser.add_argument("--cpus", type=int, default=10)
    parser.add_argument("--workers", type=int, default=10)
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--memory", default="1024M")
    parser.add_argument("--boot-timeout", type=int, default=180)
    parser.add_argument(
        "--command-timeout",
        type=int,
        default=30,
        help="seconds allowed for each measured guest repetition",
    )
    parser.add_argument(
        "--engine",
        choices=("jit", "gadgets", "interpreter"),
        default="jit",
    )
    boot_mode = parser.add_mutually_exclusive_group()
    boot_mode.add_argument(
        "--shell-init",
        action="store_true",
        help="boot /bin/sh directly and mount disposable runtime filesystems",
    )
    boot_mode.add_argument(
        "--overlay-shell",
        dest="overlay_shell",
        action="store_true",
        help="boot rootfs.img through a writable tmpfs OverlayFS shell",
    )
    boot_mode.add_argument(
        "--systemd-overlay",
        dest="overlay_shell",
        action="store_true",
        help=argparse.SUPPRESS,
    )
    return parser.parse_args()


def upload(guest: Guest, binary: pathlib.Path) -> None:
    payload = base64.b64encode(binary.resolve().read_bytes()).decode("ascii")
    result = guest.run_command("rm -f /tmp/guest-link-loop.b64")
    if result.return_code == 0:
        for offset in range(0, len(payload), 512):
            chunk = payload[offset : offset + 512]
            result = guest.run_command(
                f"printf %s {shlex.quote(chunk)} >>/tmp/guest-link-loop.b64"
            )
            if result.return_code != 0:
                break
    if result.return_code == 0:
        result = guest.run_command(
            "base64 -d /tmp/guest-link-loop.b64 >/tmp/guest-link-loop && "
            "chmod 755 /tmp/guest-link-loop"
        )
    if result.return_code != 0:
        raise RuntimeError(f"guest upload failed\n{result.output}")


def workload_command(workers: int) -> str:
    return (
        "rm -f /tmp/link-loop-output.*; "
        ": >/tmp/link-loop-pids; "
        "i=0; "
        f"while [ \"$i\" -lt {workers} ]; do "
        "/tmp/guest-link-loop >\"/tmp/link-loop-output.$i\" 2>&1 & "
        "echo \"$!\" >>/tmp/link-loop-pids; "
        "i=$((i + 1)); "
        "done; "
        "status=0; "
        "for pid in $(cat /tmp/link-loop-pids); do "
        "wait \"$pid\" || status=1; "
        "done; "
        "cat /tmp/link-loop-output.*; "
        "rm -f /tmp/link-loop-output.* /tmp/link-loop-pids; "
        "test \"$status\" -eq 0"
    )


def main() -> int:
    arguments = parse_arguments()
    host_cpus = min(os.cpu_count() or 1, 64)
    if not 1 <= arguments.cpus <= host_cpus:
        raise ValueError("--cpus must be between 1 and the host CPU count")
    if not 1 <= arguments.workers <= arguments.cpus:
        raise ValueError("--workers must be between 1 and --cpus")
    if arguments.repetitions < 1:
        raise ValueError("--repetitions must be positive")
    if arguments.command_timeout < 1:
        raise ValueError("--command-timeout must be positive")

    guest = Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        arguments.memory,
        "tmpfs",
        (
            f"ncpus={arguments.cpus}",
            f"root={arguments.root_device}",
        ),
        init_shell=arguments.shell_init,
        overlay_shell=arguments.overlay_shell,
        engine=arguments.engine,
    )
    elapsed_samples: list[float] = []
    utilization_samples: list[float] = []
    try:
        if arguments.shell_init:
            guest.boot_to_shell(arguments.boot_timeout)
            mounts = guest.run_command(
                "mount -t proc proc /proc && "
                "mount -t tmpfs -o mode=1777 tmpfs /tmp",
                30,
            )
            if mounts.return_code != 0:
                raise RuntimeError(
                    f"guest mount setup failed\n{mounts.output}"
                )
        else:
            guest.boot_and_login(arguments.boot_timeout)
        upload(guest, arguments.binary)
        command = workload_command(arguments.workers)
        for repetition in range(arguments.repetitions):
            before_cpu = cpu_time_seconds(guest.process_id)
            before_wall = time.monotonic()
            result = guest.run_command(command, arguments.command_timeout)
            elapsed = time.monotonic() - before_wall
            consumed = cpu_time_seconds(guest.process_id) - before_cpu
            if result.return_code != 0:
                raise RuntimeError(
                    f"repetition {repetition + 1} failed "
                    f"({result.return_code})\n{result.output}"
                )
            output_lines = result.output.splitlines()
            success_lines = [line for line in output_lines if line == "OK"]
            if success_lines != ["OK"] * arguments.workers:
                raise RuntimeError(
                    f"repetition {repetition + 1} returned "
                    f"unexpected output {result.output!r}"
                )
            utilization = consumed / elapsed
            elapsed_samples.append(elapsed)
            utilization_samples.append(utilization)
            print(
                f"run {repetition + 1}: {elapsed:.3f}s wall, "
                f"{consumed:.3f}s host CPU ({utilization:.2f} cores)",
                flush=True,
            )
    finally:
        guest.close()

    print(
        f"median: {statistics.median(elapsed_samples):.3f}s wall, "
        f"{statistics.median(utilization_samples):.2f} host cores",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
