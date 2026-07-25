#!/usr/bin/env python3
"""Run a matched command repeatedly in a read-only overlay-root guest."""

from __future__ import annotations

import argparse
import os
import pathlib
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
    parser.add_argument("--command", required=True)
    parser.add_argument("--cpus", type=int, default=1)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--memory", default="1024M")
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    if not 1 <= arguments.cpus <= min(os.cpu_count() or 1, 64):
        raise ValueError("--cpus must be between 1 and the host CPU count")
    if arguments.repetitions < 1:
        raise ValueError("--repetitions must be positive")

    guest = Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        arguments.memory,
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
        for repetition in range(arguments.repetitions):
            before_cpu = cpu_time_seconds(guest.process_id)
            before_wall = time.monotonic()
            result = guest.run_command(arguments.command)
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
            print(result.output, flush=True)
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
