#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Profile a live Darwin UML Geekbench run and correlate samples with tests."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import csv
import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path


ANSI_ESCAPE = re.compile(r"\x1b(?:\[[0-?]*[ -/]*[@-~]|\][^\x07]*(?:\x07|\x1b\\))")
WORKLOAD = re.compile(r"(?:^|\s)Running\s+(.+?)\s*$")


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Capture per-workload samples and host-CPU telemetry")
    parser.add_argument("--pid", type=int, required=True, help="emulator PID")
    parser.add_argument("--console-log", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--poll-seconds", type=float, default=1.0)
    parser.add_argument("--sample-seconds", type=int, default=5)
    parser.add_argument(
        "--sample-command", default="/usr/bin/sample",
        help="macOS sample executable")
    return parser.parse_args()


def process_exists(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def process_telemetry(pid: int) -> tuple[str, str, str] | None:
    result = subprocess.run(
        ["ps", "-p", str(pid), "-o", "time=,%cpu=,rss="],
        check=False, capture_output=True, text=True)
    fields = result.stdout.split()
    if result.returncode != 0 or len(fields) != 3:
        return None
    return fields[0], fields[1], fields[2]


def clean_line(raw: str) -> str:
    return ANSI_ESCAPE.sub("", raw).replace("\r", "").strip()


def slug(value: str) -> str:
    result = re.sub(r"[^a-z0-9]+", "-", value.lower()).strip("-")
    return result[:80] or "unknown"


def main() -> int:
    args = arguments()
    if args.pid <= 0:
        raise SystemExit("--pid must be positive")
    if args.poll_seconds <= 0 or args.sample_seconds <= 0:
        raise SystemExit("poll and sample durations must be positive")

    args.output_dir.mkdir(parents=True, exist_ok=True)
    telemetry_path = args.output_dir / "host-cpu.csv"
    events_path = args.output_dir / "workloads.tsv"
    monitor_path = args.output_dir / "monitor.log"

    phase = "startup"
    workload = "startup"
    event_index = 0
    line_fragment = ""
    sample_process: subprocess.Popen[str] | None = None
    stopping = False

    def stop(_signum: int, _frame: object) -> None:
        nonlocal stopping
        stopping = True

    for handled in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(handled, stop)

    with (
        args.console_log.open("r", encoding="utf-8", errors="replace") as console,
        telemetry_path.open("w", newline="", encoding="utf-8") as telemetry_file,
        events_path.open("w", encoding="utf-8") as events_file,
        monitor_path.open("w", encoding="utf-8") as monitor_file,
    ):
        telemetry = csv.writer(telemetry_file)
        telemetry.writerow(
            ["epoch", "monotonic", "cpu_time", "cpu_percent", "rss_kib",
             "phase", "workload"])
        events_file.write("epoch\tmonotonic\tphase\tworkload\tsample\n")

        while not stopping and process_exists(args.pid):
            chunk = console.read()
            if chunk:
                line_fragment += chunk
                lines = re.split(r"[\r\n]+", line_fragment)
                line_fragment = lines.pop()
                for raw_line in lines:
                    line = clean_line(raw_line)
                    if line in {"Single-Core", "Multi-Core"}:
                        phase = line
                        workload = "between-workloads"
                        now = time.time()
                        events_file.write(
                            f"{now:.6f}\t{time.monotonic():.6f}\t"
                            f"{phase}\t{workload}\t\n")
                        events_file.flush()
                        continue
                    if line.startswith("Uploading results"):
                        phase = "results"
                        workload = "upload"
                        now = time.time()
                        events_file.write(
                            f"{now:.6f}\t{time.monotonic():.6f}\t"
                            f"{phase}\t{workload}\t\n")
                        events_file.flush()
                        continue
                    if line.startswith("UML_GEEKBENCH_END"):
                        phase = "complete"
                        workload = "complete"
                        now = time.time()
                        events_file.write(
                            f"{now:.6f}\t{time.monotonic():.6f}\t"
                            f"{phase}\t{workload}\t\n")
                        events_file.flush()
                        continue
                    match = WORKLOAD.search(line)
                    if not match:
                        continue
                    workload = match.group(1).strip()
                    event_index += 1
                    now = time.time()
                    sample_name = (
                        f"{event_index:03d}-{slug(phase)}-"
                        f"{slug(workload)}.sample.txt")
                    sample_path = args.output_dir / sample_name
                    if sample_process is not None and sample_process.poll() is None:
                        sample_name = "skipped-previous-sample-active"
                        monitor_file.write(
                            f"{now:.6f}: skipped {phase} / {workload}: "
                            "previous sample still active\n")
                    else:
                        sample_process = subprocess.Popen(
                            [args.sample_command, str(args.pid),
                             str(args.sample_seconds), "-file",
                             str(sample_path)],
                            stdout=monitor_file, stderr=subprocess.STDOUT,
                            text=True)
                    events_file.write(
                        f"{now:.6f}\t{time.monotonic():.6f}\t"
                        f"{phase}\t{workload}\t{sample_name}\n")
                    events_file.flush()
                    monitor_file.flush()

            observed = process_telemetry(args.pid)
            if observed is not None:
                cpu_time, cpu_percent, rss_kib = observed
                telemetry.writerow(
                    [f"{time.time():.6f}", f"{time.monotonic():.6f}",
                     cpu_time, cpu_percent, rss_kib, phase, workload])
                telemetry_file.flush()
            time.sleep(args.poll_seconds)

    if sample_process is not None and sample_process.poll() is None:
        sample_process.terminate()
        try:
            sample_process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            sample_process.kill()
            sample_process.wait()
    return 0


if __name__ == "__main__":
    sys.exit(main())
