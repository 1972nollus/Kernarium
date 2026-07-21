#!/usr/bin/env python3
"""Verify that Darwin UML runs guest work on multiple host cores."""

from __future__ import annotations

import argparse
import os
import pathlib
import subprocess
import sys
import time


LTP_TOOLS = pathlib.Path(__file__).resolve().parents[1] / "ltp"
sys.path.insert(0, str(LTP_TOOLS))
from run_guest_ltp import Guest  # noqa: E402


def cpu_time_seconds(process_id: int) -> float:
    value = subprocess.check_output(
        ["ps", "-p", str(process_id), "-o", "time="],
        text=True,
    ).strip()
    if not value:
        raise RuntimeError("ps returned no CPU time for the emulator")
    days = 0
    if "-" in value:
        day_text, value = value.split("-", 1)
        days = int(day_text)
    fields = value.split(":")
    if len(fields) == 2:
        hours = 0
        minutes, seconds = fields
    elif len(fields) == 3:
        hours, minutes, seconds = fields
    else:
        raise RuntimeError(f"unrecognized ps CPU time: {value!r}")
    return (
        days * 86400
        + int(hours) * 3600
        + int(minutes) * 60
        + float(seconds)
    )


def require_command(guest: Guest, command: str, expected: str | None = None) -> str:
    result = guest.run_command(command)
    if result.return_code != 0:
        raise RuntimeError(
            f"guest command failed ({result.return_code}): {command}\n{result.output}"
        )
    if expected is not None and result.output.strip() != expected:
        raise RuntimeError(
            f"guest command {command!r} returned {result.output!r}, "
            f"expected {expected!r}"
        )
    return result.output


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    expected_cpus = min(os.cpu_count() or 1, 64)
    guest = Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        "1024M",
        "tmpfs",
    )
    try:
        guest.boot_and_login()
        require_command(guest, "nproc", str(expected_cpus))
        require_command(
            guest,
            "cat /sys/devices/system/cpu/online",
            "0" if expected_cpus == 1 else f"0-{expected_cpus - 1}",
        )
        require_command(
            guest,
            "sh -c 'while :; do :; done' & p1=$!; "
            "sh -c 'while :; do :; done' & p2=$!; "
            "echo \"$p1 $p2\" >/tmp/uml-smp-pids",
        )
        before = cpu_time_seconds(guest.process_id)
        started = time.monotonic()
        time.sleep(4)
        elapsed = time.monotonic() - started
        consumed = cpu_time_seconds(guest.process_id) - before
        require_command(
            guest,
            "read p1 p2 </tmp/uml-smp-pids; kill \"$p1\" \"$p2\"; "
            "wait \"$p1\" \"$p2\" 2>/dev/null; rm /tmp/uml-smp-pids",
        )
        utilization = consumed / elapsed
        print(
            f"guest SMP: {expected_cpus} vCPUs, host CPU time "
            f"{consumed:.2f}s over "
            f"{elapsed:.2f}s wall ({utilization:.2f} cores)",
            flush=True,
        )
        if utilization < 1.25:
            raise RuntimeError(
                "two runnable guest processes did not execute concurrently"
            )
        if expected_cpus > 1:
            require_command(
                guest,
                "i=0; while [ $i -lt 5 ]; do "
                "echo 0 >/sys/devices/system/cpu/cpu1/online || exit 1; "
                "echo 1 >/sys/devices/system/cpu/cpu1/online || exit 1; "
                "i=$((i + 1)); done; echo HOTPLUG_OK",
                "HOTPLUG_OK",
            )
            require_command(
                guest,
                "cat /sys/devices/system/cpu/online",
                f"0-{expected_cpus - 1}",
            )
        require_command(guest, "nproc", str(expected_cpus))
        return 0
    finally:
        guest.close()


if __name__ == "__main__":
    raise SystemExit(main())
