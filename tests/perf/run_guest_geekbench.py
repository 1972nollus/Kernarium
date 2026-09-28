#!/usr/bin/env python3
"""Run the image-local Geekbench payload in a disposable overlay shell."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import pathlib
import re
import subprocess
import sys


TESTS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TESTS / "integration" / "support"))
from guest import CORE_CLASSES, Guest, GuestFile  # noqa: E402


# The workload script Kernarium runs as well; it prints the run's markers.
WORKLOAD = pathlib.Path(__file__).resolve().parent / "guest" / "geekbench.sh"
GUEST_WORKLOAD = "/usr/local/libexec/uml-test/geekbench"
GUEST_PARAMETERS = "/etc/uml-geekbench.env"
WORKLOAD_LIST = re.compile(r"[0-9]+(?:,[0-9]+)*")


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
        "--workloads",
        metavar="LIST",
        help=(
            "run only these comma-separated single-core workload numbers "
            "instead of the full benchmark"
        ),
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


def main() -> int:
    arguments = parse_arguments()
    if arguments.cpus < 1 or arguments.cpus > 64:
        raise ValueError("--cpus must be between 1 and 64")
    if (
        arguments.timeout < 1
        or arguments.sample_seconds < 1
    ):
        raise ValueError("timeouts must be positive")
    payloads = {GUEST_WORKLOAD: GuestFile(WORKLOAD.read_bytes(), 0o755)}
    if arguments.workloads is not None:
        if not WORKLOAD_LIST.fullmatch(arguments.workloads):
            raise ValueError("--workloads must be comma-separated numbers")
        payloads[GUEST_PARAMETERS] = GuestFile(
            f"UML_GEEKBENCH_WORKLOADS={arguments.workloads}\n".encode()
        )

    if arguments.profile_dir is not None:
        arguments.profile_dir.mkdir(parents=True, exist_ok=True)
    arguments.console_log.parent.mkdir(parents=True, exist_ok=True)
    with arguments.console_log.open("wb") as console_log, Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        arguments.memory,
        "tmpfs",
        (
            f"ncpus={arguments.cpus}",
            *arguments.kernel_argument,
        ),
        overlay_shell=True,
        engine=arguments.engine,
        core_class=arguments.core_class,
        console_sink=console_log,
        payloads=payloads,
    ) as guest:
        profiler: subprocess.Popen[bytes] | None = None
        try:
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
            result = guest.run_command(GUEST_WORKLOAD, arguments.timeout)
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
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
