#!/usr/bin/env python3
"""Run the image-local Geekbench payload in a direct OverlayFS shell."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import contextlib
import os
import pathlib
import re
import subprocess
import sys
import threading
import time


TESTS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TESTS / "integration" / "support"))
from guest import CORE_CLASSES, Guest  # noqa: E402


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument(
        "--profile-dir",
        type=pathlib.Path,
        help="enable host sampling and store profiles in this directory",
    )
    parser.add_argument("--console-log", required=True, type=pathlib.Path)
    parser.add_argument(
        "--coverage-file", type=pathlib.Path,
        help="collect executed gadget dependency counters and workload events; requires a coverage build",
    )
    parser.add_argument(
        "--command",
        default=(
            "/opt/geekbench/release/geekbench6 "
            "--no-upload"
        ),
    )
    parser.add_argument(
        "--root-device",
        default="/dev/ubda",
        help="raw EROFS root block device (default: /dev/ubda)",
    )
    parser.add_argument(
        "--kernel-argument",
        action="append",
        default=[],
        metavar="ARGUMENT",
        help=(
            "append one extra guest kernel argument; repeatable. Isolating a "
            "kernel policy that the config turns on by default, such as "
            "'psi=0', needs a matched pair of runs that differ only here"
        ),
    )
    parser.add_argument("--memory", default="8G")
    parser.add_argument("--cpus", type=int, default=10)
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
    events_path: pathlib.Path | None = None,
) -> None:
    """Append new raw console bytes while Guest owns the PTY reader."""
    offset = 0
    fragment = ""
    phase = "startup"
    with (
        output_path.open("wb") as output,
        events_path.open("w") if events_path else contextlib.nullcontext() as events,
    ):
        if events is not None:
            events.write("monotonic_ns\tphase\tworkload\n")
            events.flush()
        ready.set()
        while True:
            available = len(guest._transcript)
            if available > offset:
                chunk = bytes(guest._transcript[offset:available])
                output.write(chunk)
                output.flush()
                offset = available
                if events is not None:
                    fragment += chunk.decode("utf-8", errors="replace")
                    lines = re.split(r"[\r\n]+", fragment)
                    fragment = lines.pop()
                    for raw in lines:
                        line = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", raw).strip()
                        workload = None
                        if line == "GB_BEGIN":
                            phase, workload = "benchmark", "startup"
                        elif line in {"Single-Core", "Multi-Core"}:
                            phase, workload = line, "between-workloads"
                        elif line == "GB_END":
                            phase, workload = "complete", "complete"
                        elif line.startswith("Running "):
                            workload = line.removeprefix("Running ").strip()
                        elif phase in {"Single-Core", "Multi-Core"}:
                            completed = re.match(r"^(.+?)\s{2,}\d+\s{2,}\S", line)
                            if completed:
                                # One-iteration preview builds report each name
                                # at completion instead of emitting Running.
                                events.write(
                                    f"{time.monotonic_ns()}\t{phase}-completed\t"
                                    f"{completed.group(1)}\n"
                                )
                                events.flush()
                        if workload is not None:
                            events.write(f"{time.monotonic_ns()}\t{phase}\t{workload}\n")
                            events.flush()
            if stop.is_set() and offset == len(guest._transcript):
                return
            time.sleep(0.1)


def main() -> int:
    arguments = parse_arguments()
    if arguments.cpus < 1 or arguments.cpus > 64:
        raise ValueError("--cpus must be between 1 and 64")
    if (
        arguments.timeout < 1
        or arguments.sample_seconds < 1
    ):
        raise ValueError("timeouts must be positive")

    if arguments.profile_dir is not None:
        arguments.profile_dir.mkdir(parents=True, exist_ok=True)
    if arguments.coverage_file is not None:
        if arguments.engine != "gadgets":
            raise ValueError("dependency coverage requires --engine gadgets")
        arguments.coverage_file = arguments.coverage_file.resolve()
        arguments.coverage_file.parent.mkdir(parents=True, exist_ok=True)
        os.environ["DARWIN_UML_GADGET_DEPENDENCY_COVERAGE"] = str(arguments.coverage_file)
    arguments.console_log.parent.mkdir(parents=True, exist_ok=True)
    guest = Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        arguments.memory,
        "tmpfs",
        (
            f"ncpus={arguments.cpus}",
            f"root={arguments.root_device}",
            *arguments.kernel_argument,
        ),
        overlay_shell=True,
        engine=arguments.engine,
        core_class=arguments.core_class,
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
                arguments.coverage_file.with_suffix(".events.tsv")
                if arguments.coverage_file is not None else None,
            ),
            name="geekbench-console-mirror",
        )
        mirror.start()
        mirror_ready.wait()
        guest.boot_and_login(180)
        if arguments.profile_dir is not None:
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
            "printf '\\nGB_BEGIN\\n'; "
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
        emulator_status = guest.return_code
        if emulator_status != 0:
            raise RuntimeError(
                f"emulator exited with status {emulator_status} after Geekbench"
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
