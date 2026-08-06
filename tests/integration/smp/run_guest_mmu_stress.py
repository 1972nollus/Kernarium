#!/usr/bin/env python3
"""Exercise shared-mm thread creation and mapping churn in Darwin UML."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import base64
import os
import pathlib
import shlex
import sys
import time


TESTS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TESTS / "support"))
from guest import Guest  # noqa: E402
from host_metrics import cpu_time_seconds  # noqa: E402


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument(
        "--root-device",
        default="/dev/ubda1",
        help="read-only root partition provided by the immutable GPT image",
    )
    parser.add_argument(
        "--overlay-shell",
        dest="overlay_shell",
        action="store_true",
        help="bootstrap a writable tmpfs OverlayFS shell from /bin/sh",
    )
    parser.add_argument(
        "--systemd-overlay",
        dest="overlay_shell",
        action="store_true",
        help=argparse.SUPPRESS,
    )
    parser.add_argument("--cpus", type=int, default=10)
    parser.add_argument(
        "--shared-mmap-binary",
        required=True,
        type=pathlib.Path,
        help="freestanding arm64/Linux shared-mm stress executable",
    )
    return parser.parse_args()


def run_timed(guest: Guest, name: str, command: str) -> str:
    before_cpu = cpu_time_seconds(guest.process_id)
    before_wall = time.monotonic()
    result = guest.run_command(command)
    elapsed = time.monotonic() - before_wall
    consumed = cpu_time_seconds(guest.process_id) - before_cpu
    if result.return_code != 0:
        raise RuntimeError(
            f"{name} failed ({result.return_code})\n{result.output}"
        )
    print(
        f"{name}: {elapsed:.3f}s wall, {consumed:.3f}s host CPU "
        f"({consumed / elapsed:.2f} cores)",
        flush=True,
    )
    return result.output


def upload_bytes(guest: Guest, payload: bytes, destination: str) -> None:
    encoded = base64.b64encode(payload).decode("ascii")
    staging = f"{destination}.base64"
    quoted_staging = shlex.quote(staging)
    quoted_destination = shlex.quote(destination)
    result = guest.run_command(f": > {quoted_staging}", 30)
    if result.return_code == 0:
        for offset in range(0, len(encoded), 512):
            result = guest.run_command(
                f"printf %s {shlex.quote(encoded[offset : offset + 512])} "
                f">> {quoted_staging}",
                30,
            )
            if result.return_code != 0:
                break
    if result.return_code == 0:
        result = guest.run_command(
            f"base64 -d {quoted_staging} > {quoted_destination} && "
            f"rm -f {quoted_staging}",
            30,
        )
    if result.return_code != 0:
        raise RuntimeError(
            f"failed to upload {destination} ({result.return_code})\n"
            f"{result.output}"
        )


def stage_shared_mmap_stress(guest: Guest, binary: pathlib.Path) -> str:
    binary = binary.resolve()
    if not binary.is_file():
        raise ValueError(f"shared-mm stress binary is not a file: {binary}")
    destination = "/tmp/guest-shared-mmap-stress"
    upload_bytes(guest, binary.read_bytes(), destination)
    result = guest.run_command(f"chmod 755 {shlex.quote(destination)}", 30)
    if result.return_code != 0:
        raise RuntimeError(
            f"could not make shared-mm stress executable ({result.return_code})\n"
            f"{result.output}"
        )
    return destination


def main() -> int:
    arguments = parse_arguments()
    host_cpus = min(os.cpu_count() or 1, 64)
    if not 1 <= arguments.cpus <= host_cpus:
        raise ValueError("--cpus must be between 1 and the host CPU count")

    guest = Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        "1024M",
        "tmpfs",
        (f"ncpus={arguments.cpus}", f"root={arguments.root_device}"),
        overlay_shell=arguments.overlay_shell,
    )
    try:
        guest.boot_and_login()
        executable = stage_shared_mmap_stress(
            guest, arguments.shared_mmap_binary
        )
        output = run_timed(guest, "shared-mm-distant-mmap", executable)
        if output.strip() != "OK":
            raise RuntimeError(
                f"shared-mm workload returned unexpected output {output!r}"
            )
        return 0
    finally:
        guest.close()


if __name__ == "__main__":
    raise SystemExit(main())
