#!/usr/bin/env python3
"""Verify that Darwin UML runs guest work on multiple host cores."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
import pathlib
import sys
import time


SUPPORT = pathlib.Path(__file__).resolve().parents[1] / "support"
sys.path.insert(0, str(SUPPORT))
from guest import Guest  # noqa: E402
from host_metrics import cpu_time_seconds  # noqa: E402


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
    parser.add_argument(
        "--root-device",
        help=(
            "guest block device containing /; when set, append an explicit "
            "root= argument"
        ),
    )
    parser.add_argument(
        "--overlay-shell",
        dest="overlay_shell",
        action="store_true",
        default=True,
        help=(
            "bootstrap the required writable tmpfs OverlayFS shell from "
            "/bin/sh (default and retained for CLI compatibility)"
        ),
    )
    parser.add_argument(
        "--systemd-overlay",
        dest="overlay_shell",
        action="store_true",
        help=argparse.SUPPRESS,
    )
    parser.add_argument(
        "--cpus",
        type=int,
        default=10,
        help="number of guest cores to start (default: 10)",
    )
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    if not 1 <= arguments.cpus <= min(os.cpu_count() or 1, 64):
        raise ValueError("--cpus must be between 1 and the host CPU count")
    expected_cpus = arguments.cpus
    kernel_arguments = [f"ncpus={expected_cpus}"]
    if arguments.root_device:
        kernel_arguments.append(f"root={arguments.root_device}")
    guest = Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        "1024M",
        "tmpfs",
        tuple(kernel_arguments),
        overlay_shell=arguments.overlay_shell,
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
            "for cpu in /sys/devices/system/cpu/cpu[0-9]*; do "
            "id=${cpu##*cpu}; "
            "test \"$(cat \"$cpu/topology/physical_package_id\")\" = 0 || exit 1; "
            "test \"$(cat \"$cpu/topology/core_id\")\" = \"$id\" || exit 1; "
            "test \"$(cat \"$cpu/topology/thread_siblings_list\")\" = \"$id\" "
            "|| exit 1; "
            "done; echo TOPOLOGY_OK",
            "TOPOLOGY_OK",
        )
        require_command(
            guest,
            ": >/tmp/uml-smp-pids; i=0; "
            f"while [ \"$i\" -lt {expected_cpus} ]; do "
            "sh -c 'while :; do :; done' & "
            "echo \"$!\" >>/tmp/uml-smp-pids; "
            "i=$((i + 1)); "
            "done",
        )
        before = cpu_time_seconds(guest.process_id)
        started = time.monotonic()
        time.sleep(4)
        elapsed = time.monotonic() - started
        consumed = cpu_time_seconds(guest.process_id) - before
        require_command(
            guest,
            "pids=$(cat /tmp/uml-smp-pids); kill $pids; "
            "wait $pids 2>/dev/null; rm /tmp/uml-smp-pids",
        )
        utilization = consumed / elapsed
        print(
            f"guest SMP: {expected_cpus} vCPUs, host CPU time "
            f"{consumed:.2f}s over "
            f"{elapsed:.2f}s wall ({utilization:.2f} cores)",
            flush=True,
        )
        minimum_utilization = (
            0.75 if expected_cpus == 1 else max(1.25, expected_cpus * 0.5)
        )
        if utilization < minimum_utilization:
            raise RuntimeError(
                f"{expected_cpus} runnable guest processes used only "
                f"{utilization:.2f} host cores; expected at least "
                f"{minimum_utilization:.2f}"
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
