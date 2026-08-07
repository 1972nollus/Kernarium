#!/usr/bin/env python3
"""Run a matched command repeatedly in a read-only overlay-root guest."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import base64
import os
import pathlib
import shlex
import subprocess
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
    parser.add_argument("--root-device", default="/dev/ubda1")
    parser.add_argument("--command", required=True)
    parser.add_argument("--cpus", type=int, default=1)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--memory", default="1024M")
    parser.add_argument(
        "--engine",
        choices=("jit", "gadgets", "interpreter"),
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
        "--sample-output",
        type=pathlib.Path,
        help="capture a macOS sample profile during each command",
    )
    parser.add_argument("--sample-seconds", type=int, default=5)
    parser.add_argument("--timeout", type=int, default=30)
    parser.add_argument(
        "--upload",
        action="append",
        default=[],
        metavar="HOST_PATH:GUEST_PATH",
        help="stage a host file in guest /tmp before running the command",
    )
    parser.add_argument(
        "--shell-init",
        action="store_true",
        help="boot /bin/sh directly (for rootfs images without overlay init)",
    )
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    if not 1 <= arguments.cpus <= min(os.cpu_count() or 1, 64):
        raise ValueError("--cpus must be between 1 and the host CPU count")
    if arguments.repetitions < 1:
        raise ValueError("--repetitions must be positive")
    if arguments.sample_seconds < 1 or arguments.timeout < 1:
        raise ValueError("sample duration and timeout must be positive")

    extra_arguments = [
        f"ncpus={arguments.cpus}",
        f"root={arguments.root_device}",
    ]
    guest = Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        arguments.memory,
        "tmpfs",
        tuple(extra_arguments),
        init_shell=arguments.shell_init,
        engine=arguments.engine,
        core_class=arguments.core_class,
    )
    elapsed_samples: list[float] = []
    utilization_samples: list[float] = []
    try:
        if arguments.shell_init:
            guest.boot_to_shell(180)
            mounts = guest.run_command(
                "mount -t proc proc /proc && "
                "mount -t tmpfs -o mode=1777 tmpfs /tmp",
                30,
            )
            if mounts.return_code != 0:
                raise RuntimeError(f"guest mount setup failed\n{mounts.output}")
        else:
            guest.boot_and_login()
        for upload in arguments.upload:
            try:
                host_text, guest_text = upload.split(":", 1)
            except ValueError as error:
                raise ValueError(
                    "--upload must be HOST_PATH:GUEST_PATH"
                ) from error
            host_path = pathlib.Path(host_text).resolve()
            guest_path = pathlib.PurePosixPath(guest_text)
            if not host_path.is_file():
                raise ValueError(f"upload source is not a file: {host_path}")
            if not guest_path.is_absolute() or (
                guest_path != pathlib.PurePosixPath("/tmp")
                and pathlib.PurePosixPath("/tmp") not in guest_path.parents
            ):
                raise ValueError("upload destination must be below /tmp")
            encoded = base64.b64encode(host_path.read_bytes()).decode("ascii")
            encoded_path = f"{guest_path}.base64"
            quoted_encoded_path = shlex.quote(encoded_path)
            quoted_guest_path = shlex.quote(str(guest_path))
            initialize = guest.run_command(f": > {quoted_encoded_path}", 10)
            if initialize.return_code != 0:
                raise RuntimeError(
                    f"failed to initialize upload\n{initialize.output}"
                )
            # Stay comfortably below the guest console's canonical line limit.
            for offset in range(0, len(encoded), 512):
                chunk = shlex.quote(encoded[offset : offset + 512])
                append = guest.run_command(
                    f"printf %s {chunk} >> {quoted_encoded_path}", 10
                )
                if append.return_code != 0:
                    raise RuntimeError(f"failed to upload chunk\n{append.output}")
            finalize = guest.run_command(
                f"base64 -d {quoted_encoded_path} > {quoted_guest_path} && "
                f"chmod 755 {quoted_guest_path} && "
                f"rm -f {quoted_encoded_path}",
                10,
            )
            if finalize.return_code != 0:
                raise RuntimeError(f"failed to finalize upload\n{finalize.output}")
        for repetition in range(arguments.repetitions):
            sampler = None
            if arguments.sample_output is not None:
                sample_path = arguments.sample_output
                if arguments.repetitions != 1:
                    sample_path = sample_path.with_name(
                        f"{sample_path.stem}-{repetition + 1}"
                        f"{sample_path.suffix}"
                    )
                sample_path.parent.mkdir(parents=True, exist_ok=True)
                sampler = subprocess.Popen(
                    (
                        "/usr/bin/sample",
                        str(guest.process_id),
                        str(arguments.sample_seconds),
                        "-file",
                        str(sample_path.resolve()),
                    ),
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
            before_cpu = cpu_time_seconds(guest.process_id)
            before_wall = time.monotonic()
            result = guest.run_command(arguments.command, arguments.timeout)
            elapsed = time.monotonic() - before_wall
            consumed = cpu_time_seconds(guest.process_id) - before_cpu
            if sampler is not None:
                try:
                    sampler.wait(timeout=arguments.sample_seconds + 5)
                except subprocess.TimeoutExpired:
                    sampler.terminate()
                    sampler.wait()
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
    except BaseException:
        print(guest.output(), file=sys.stderr, flush=True)
        raise
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
