#!/usr/bin/env python3
"""Measure cached gadget-block link throughput in a standalone guest."""

from __future__ import annotations

import argparse
import base64
import os
import pathlib
import shlex
import statistics
import sys
import time


TESTS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TESTS / "ltp"))
sys.path.insert(0, str(TESTS / "smp"))
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

    guest = Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        "1024M",
        "tmpfs",
        (
            f"ncpus={arguments.cpus}",
            f"root={arguments.root_device}",
        ),
    )
    elapsed_samples: list[float] = []
    utilization_samples: list[float] = []
    try:
        guest.boot_and_login()
        upload(guest, arguments.binary)
        command = workload_command(arguments.workers)
        for repetition in range(arguments.repetitions):
            before_cpu = cpu_time_seconds(guest.process_id)
            before_wall = time.monotonic()
            result = guest.run_command(command)
            elapsed = time.monotonic() - before_wall
            consumed = cpu_time_seconds(guest.process_id) - before_cpu
            if result.return_code != 0:
                raise RuntimeError(
                    f"repetition {repetition + 1} failed "
                    f"({result.return_code})\n{result.output}"
                )
            output_lines = result.output.splitlines()
            if output_lines != ["OK"] * arguments.workers:
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
