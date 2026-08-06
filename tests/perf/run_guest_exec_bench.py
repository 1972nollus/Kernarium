#!/usr/bin/env python3
"""Measure cold translation and exec throughput with repeated /bin/true."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
import pathlib
import statistics
import sys
import time


TESTS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TESTS / "integration" / "support"))
from guest import Guest  # noqa: E402
from host_metrics import cpu_time_seconds  # noqa: E402


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--root-device", default="/dev/ubda1")
    parser.add_argument("--cpus", type=int, default=10)
    parser.add_argument("--workers", type=int, default=10)
    parser.add_argument("--executions", type=int, default=5000)
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument(
        "--engine",
        choices=("jit", "gadgets", "interpreter"),
        default="gadgets",
    )
    return parser.parse_args()


def workload_command(workers: int, executions: int) -> str:
    count = executions // workers
    return (
        "rm -f /tmp/exec-bench-pids; : >/tmp/exec-bench-pids; "
        "worker=0; "
        f"while [ \"$worker\" -lt {workers} ]; do "
        "(i=0; "
        f"while [ \"$i\" -lt {count} ]; do /bin/true || exit; "
        "i=$((i + 1)); done) & "
        "echo \"$!\" >>/tmp/exec-bench-pids; "
        "worker=$((worker + 1)); "
        "done; "
        "status=0; "
        "for pid in $(cat /tmp/exec-bench-pids); do "
        "wait \"$pid\" || status=1; done; "
        "rm -f /tmp/exec-bench-pids; "
        "test \"$status\" -eq 0"
    )


def main() -> int:
    arguments = parse_arguments()
    host_cpus = min(os.cpu_count() or 1, 64)
    if not 1 <= arguments.cpus <= host_cpus:
        raise ValueError("--cpus must be between 1 and the host CPU count")
    if not 1 <= arguments.workers <= arguments.cpus:
        raise ValueError("--workers must be between 1 and --cpus")
    if arguments.executions < arguments.workers or (
        arguments.executions % arguments.workers
    ):
        raise ValueError("--executions must be a positive multiple of --workers")
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
        overlay_shell=True,
        engine=arguments.engine,
    )
    elapsed_samples: list[float] = []
    utilization_samples: list[float] = []
    try:
        guest.boot_and_login()
        command = workload_command(arguments.workers, arguments.executions)
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
