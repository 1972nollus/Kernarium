#!/usr/bin/env python3
"""Exercise shared-mm thread creation and mapping churn in Darwin UML."""

from __future__ import annotations

import argparse
import base64
import os
import pathlib
import shlex
import sys
import time


TESTS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TESTS / "ltp"))
sys.path.insert(0, str(TESTS / "smp"))
from run_guest_ltp import Guest, ltp_guest_command  # noqa: E402
from run_guest_smp import cpu_time_seconds  # noqa: E402


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--cpus", type=int, default=10)
    parser.add_argument("--openfile-iterations", type=int, default=20)
    parser.add_argument("--toolchain-only", action="store_true")
    parser.add_argument("--shared-mmap-binary", type=pathlib.Path)
    parser.add_argument("--shared-only", action="store_true")
    return parser.parse_args()


def run_timed(guest: Guest, name: str, command: str) -> str:
    before_cpu = cpu_time_seconds(guest.process_id)
    before_wall = time.monotonic()
    result = guest.run_command(ltp_guest_command(command))
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


def main() -> int:
    arguments = parse_arguments()
    host_cpus = min(os.cpu_count() or 1, 64)
    if not 1 <= arguments.cpus <= host_cpus:
        raise ValueError("--cpus must be between 1 and the host CPU count")
    if arguments.openfile_iterations < 1:
        raise ValueError("--openfile-iterations must be positive")

    guest = Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        "1024M",
        "tmpfs",
        (f"ncpus={arguments.cpus}",),
    )
    try:
        guest.boot_and_login()
        tools = guest.run_command(
            "for tool in cc gcc clang javac java; do "
            "command -v \"$tool\" 2>/dev/null || true; "
            "done"
        )
        if tools.return_code != 0:
            raise RuntimeError(f"guest tool discovery failed\n{tools.output}")
        print(f"guest tools:\n{tools.output.strip()}", flush=True)
        if arguments.toolchain_only:
            return 0
        if arguments.shared_mmap_binary is not None:
            payload = base64.b64encode(
                arguments.shared_mmap_binary.resolve().read_bytes()
            ).decode("ascii")
            upload = guest.run_command("rm -f /tmp/guest-shared-mmap-stress.b64")
            if upload.return_code == 0:
                for offset in range(0, len(payload), 512):
                    chunk = payload[offset : offset + 512]
                    upload = guest.run_command(
                        f"printf %s {shlex.quote(chunk)} "
                        ">>/tmp/guest-shared-mmap-stress.b64"
                    )
                    if upload.return_code != 0:
                        break
            if upload.return_code == 0:
                upload = guest.run_command(
                    "base64 -d /tmp/guest-shared-mmap-stress.b64 "
                    ">/tmp/guest-shared-mmap-stress && "
                    "chmod 755 /tmp/guest-shared-mmap-stress"
                )
            if upload.return_code != 0:
                raise RuntimeError(f"guest upload failed\n{upload.output}")
            output = run_timed(
                guest,
                "shared-mm-distant-mmap",
                "/tmp/guest-shared-mmap-stress",
            )
            if output.strip() != "OK":
                raise RuntimeError(
                    f"shared-mm workload returned unexpected output {output!r}"
                )
            if arguments.shared_only:
                return 0
        run_timed(
            guest,
            "pthread-stack-churn",
            "i=0; "
            f"while [ \"$i\" -lt {arguments.openfile_iterations} ]; do "
            "openfile -f10 -t10 >/dev/null || exit; "
            "i=$((i + 1)); "
            "done",
        )
        run_timed(
            guest,
            "concurrent-anonymous-mmap",
            "mmapstress09 -p 20 -t 0.2 >/dev/null",
        )
        run_timed(
            guest,
            "concurrent-file-mmap",
            "mmapstress10 -p 20 -t 0.2 >/dev/null",
        )
        return 0
    finally:
        guest.close()


if __name__ == "__main__":
    raise SystemExit(main())
