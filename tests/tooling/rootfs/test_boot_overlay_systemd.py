#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify that the manual overlay launcher cannot weaken rootfs policy."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import shlex
import subprocess
import tempfile
from pathlib import Path


def run(
    launcher: Path,
    rootfs: Path,
    *kernel_arguments: str,
) -> subprocess.CompletedProcess[str]:
    command = [
        str(launcher),
        "--emulator",
        "/usr/bin/true",
        "--rootfs",
        str(rootfs),
        "--dry-run",
    ]
    if kernel_arguments:
        command.append("--")
        command.extend(kernel_arguments)
    return subprocess.run(
        command,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--launcher", required=True, type=Path)
    arguments = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="uml-overlay-launcher-") as name:
        rootfs = Path(name) / "rootfs image.img"
        rootfs.touch()

        valid = run(arguments.launcher, rootfs, "quiet")
        require(valid.returncode == 0, f"valid dry run failed:\n{valid.stdout}")
        command = shlex.split(valid.stdout)
        require(
            f"ubd0r={rootfs}" in command and "ro" in command,
            f"dry run lacks the read-only disk and mount options:\n{valid.stdout}",
        )

        rejected = {
            "rw": "writable-root kernel argument",
            "ro": "supplied by the launcher",
            "ubd0=/tmp/writable.img": "controlled by the launcher",
            "ubd0r=/tmp/other.img": "controlled by the launcher",
            "root=/dev/ubda": "controlled by the launcher",
            "init=/bin/sh": "controlled by the launcher",
            "mem=1G": "controlled by the launcher",
            "console=null": "controlled by the launcher",
            "systemd.unit=rescue.target": "controlled by the launcher",
        }
        for kernel_argument, diagnostic in rejected.items():
            result = run(arguments.launcher, rootfs, kernel_argument)
            require(
                result.returncode == 2,
                f"protected option was accepted: {kernel_argument}\n{result.stdout}",
            )
            require(
                diagnostic in result.stdout,
                f"protected option failed without its diagnostic: "
                f"{kernel_argument}\n{result.stdout}",
            )

    print("overlay launcher read-only option policy tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
