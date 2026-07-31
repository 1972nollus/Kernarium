#!/usr/bin/env python3
"""Run and profile Geekbench in a read-only overlay-root Darwin UML guest."""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys
import threading
import time


TESTS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TESTS / "ltp"))
from run_guest_ltp import Guest  # noqa: E402


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--profile-dir", required=True, type=pathlib.Path)
    parser.add_argument("--console-log", required=True, type=pathlib.Path)
    parser.add_argument(
        "--command",
        default="/root/release/geekbench6",
    )
    parser.add_argument("--root-device", default="/dev/ubda1")
    parser.add_argument("--memory", default="8G")
    parser.add_argument("--cpus", type=int, default=10)
    parser.add_argument(
        "--engine",
        choices=("jit", "gadgets", "interpreter"),
        default="jit",
    )
    parser.add_argument("--timeout", type=int, default=7200)
    parser.add_argument("--sample-seconds", type=int, default=5)
    parser.add_argument(
        "--sample-command",
        default="/usr/bin/sample",
    )
    return parser.parse_args()


def mirror_transcript(
    guest: Guest,
    output_path: pathlib.Path,
    ready: threading.Event,
    stop: threading.Event,
) -> None:
    """Append new raw console bytes while Guest owns the PTY reader."""
    offset = 0
    with output_path.open("wb") as output:
        ready.set()
        while True:
            available = len(guest._transcript)
            if available > offset:
                output.write(bytes(guest._transcript[offset:available]))
                output.flush()
                offset = available
            if stop.is_set() and offset == len(guest._transcript):
                return
            time.sleep(0.1)


def main() -> int:
    arguments = parse_arguments()
    if arguments.cpus < 1 or arguments.cpus > 64:
        raise ValueError("--cpus must be between 1 and 64")
    if arguments.timeout < 1 or arguments.sample_seconds < 1:
        raise ValueError("timeouts must be positive")

    arguments.profile_dir.mkdir(parents=True, exist_ok=True)
    arguments.console_log.parent.mkdir(parents=True, exist_ok=True)
    guest = Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        arguments.memory,
        "tmpfs",
        (
            f"ncpus={arguments.cpus}",
            f"root={arguments.root_device}",
            f"--execution-engine={arguments.engine}",
        ),
        systemd_overlay=True,
    )
    mirror_stop = threading.Event()
    mirror_ready = threading.Event()
    mirror: threading.Thread | None = None
    profiler: subprocess.Popen[bytes] | None = None
    try:
        mirror = threading.Thread(
            target=mirror_transcript,
            args=(
                guest,
                arguments.console_log.resolve(),
                mirror_ready,
                mirror_stop,
            ),
            name="geekbench-console-mirror",
        )
        mirror.start()
        mirror_ready.wait()
        guest.boot_and_login(180)
        profiler = subprocess.Popen(
            (
                sys.executable,
                str(pathlib.Path(__file__).with_name("profile_geekbench.py")),
                "--pid",
                str(guest.process_id),
                "--console-log",
                str(arguments.console_log.resolve()),
                "--output-dir",
                str(arguments.profile_dir.resolve()),
                "--sample-seconds",
                str(arguments.sample_seconds),
                "--sample-command",
                arguments.sample_command,
            )
        )
        command = (
            f"{arguments.command}; benchmark_rc=$?; "
            "printf '\\nGB_END\\n'; (exit \"$benchmark_rc\")"
        )
        result = guest.run_command(command, arguments.timeout)
        print(result.output, flush=True)
        shutdown_ok = guest.shutdown(60)
        if result.return_code != 0:
            raise RuntimeError(
                f"Geekbench exited with status {result.return_code}"
            )
        if not shutdown_ok:
            raise RuntimeError(
                "guest did not halt cleanly after Geekbench"
            )
    finally:
        if profiler is not None and profiler.poll() is None:
            profiler.terminate()
            try:
                profiler.wait(timeout=10)
            except subprocess.TimeoutExpired:
                profiler.kill()
                profiler.wait()
        mirror_stop.set()
        if mirror is not None:
            mirror.join(timeout=5)
        guest.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
