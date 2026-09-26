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
sys.path.insert(0, str(TESTS / "integration" / "support"))
from guest import CORE_CLASSES, Guest  # noqa: E402
from host_metrics import cpu_time_seconds  # noqa: E402


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument(
        "--root-device",
        default="/dev/ubda",
        help="raw EROFS root block device (default: /dev/ubda)",
    )
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
        choices=("jit", "gadgets"),
        default="jit",
    )
    parser.add_argument(
        "--core-class",
        choices=CORE_CLASSES,
        default="performance",
        help=(
            "host core class to measure on; 'efficiency' clamps the emulator "
            "to background QoS so it runs on the E-cores that dominate iOS"
        ),
    )
    parser.add_argument(
        "--guest-top-byte-ignore",
        action="store_true",
        help="run the guest with the top address byte ignored, as Android is",
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
        help="boot the EROFS root through a writable tmpfs OverlayFS shell",
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
            *(("--guest-top-byte-ignore",)
              if arguments.guest_top_byte_ignore else ()),
        ),
        init_shell=arguments.shell_init,
        overlay_shell=arguments.overlay_shell,
        systemd_overlay=not (arguments.shell_init or arguments.overlay_shell),
        engine=arguments.engine,
        core_class=arguments.core_class,
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
            probe_lines = [
                line
                for line in output_lines
                if line.startswith("UML_PERF_PROBE ")
            ]
            for line in probe_lines:
                print(f"guest run {repetition + 1}: {line}", flush=True)
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
        shutdown_ok = guest.shutdown(60)
        emulator_status = guest.return_code
        if not shutdown_ok:
            raise RuntimeError("emulator did not halt cleanly within 60 seconds")
        if emulator_status != 0:
            raise RuntimeError(
                f"emulator exited with status {emulator_status}"
            )
    finally:
        guest.close()

    print(
        f"median: {statistics.median(elapsed_samples):.3f}s wall, "
        f"{statistics.median(utilization_samples):.2f} host cores "
        f"[engine={arguments.engine} cores={arguments.core_class} "
        f"cpus={arguments.cpus} workers={arguments.workers}]",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
