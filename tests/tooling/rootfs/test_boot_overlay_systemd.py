#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the manual overlay launcher's EROFS boot options in dry-run mode."""

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
        rootfs = Path(name) / "rootfs image.erofs"
        rootfs.touch()

        # The launcher runs under macOS bash 3.2, where expanding an empty
        # array under set -u aborts, so the default invocation is checked
        # without any extra kernel argument too.
        for extra in ((), ("quiet",)):
            valid = run(arguments.launcher, rootfs, *extra)
            require(
                valid.returncode == 0,
                f"valid dry run with {list(extra)} failed:\n{valid.stdout}",
            )
            command = shlex.split(valid.stdout)
            require(
                f"ubd0c={rootfs}" in command
                and "root=/dev/ubda" in command
                and "rootfstype=erofs" in command,
                f"dry run lacks the raw EROFS boot options:\n{valid.stdout}",
            )
            require(
                not extra or command[-len(extra):] == list(extra),
                f"dry run lost its extra kernel arguments:\n{valid.stdout}",
            )

        rejected = {
            "ubd0=/tmp/other.erofs": "controlled by the launcher",
            "ubd0c=/tmp/other.erofs": "controlled by the launcher",
            "ubd0r=/tmp/other.erofs": "controlled by the launcher",
            "root=/dev/ubdb": "controlled by the launcher",
            "rootfstype=ext4": "controlled by the launcher",
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

    print("overlay launcher rootfs option policy tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
