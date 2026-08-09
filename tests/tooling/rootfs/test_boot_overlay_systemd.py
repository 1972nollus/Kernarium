#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify the manual overlay launcher pins the unified EROFS boot contract."""

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

    launcher_source = arguments.launcher.read_text(encoding="utf-8")
    require(
        "getty@tty0.service" not in launcher_source
        and "agetty --autologin" not in launcher_source
        and "useradd" not in launcher_source
        and "groupadd" not in launcher_source,
        "launcher must preserve the image-owned account and tty0 getty setup",
    )
    require(
        "mkdir -p /var/lib/docker" in launcher_source
        and "mount -t tmpfs -o mode=0711,nosuid,nodev tmpfs /var/lib/docker"
        in launcher_source,
        "launcher must put Docker state on a disposable tmpfs before systemd",
    )
    require(
        "mkdir -p /var/lib/containerd" in launcher_source
        and "mount -t tmpfs -o mode=0711,nosuid,nodev tmpfs /var/lib/containerd"
        in launcher_source,
        "launcher must put containerd state on a disposable tmpfs before systemd",
    )
    docker_mount = launcher_source.index(
        "mount -t tmpfs -o mode=0711,nosuid,nodev tmpfs /var/lib/docker"
    )
    containerd_mount = launcher_source.index(
        "mount -t tmpfs -o mode=0711,nosuid,nodev tmpfs /var/lib/containerd"
    )
    require(
        launcher_source.index("pivot_root . .uml-lower") < docker_mount
        < containerd_mount < launcher_source.index("exec /lib/systemd/systemd"),
        "launcher must mount Docker and containerd state after the pivot and "
        "before systemd",
    )
    require(
        'exec "$rootfs_wrapper" --rootfs "$rootfs" --' in launcher_source,
        "launcher must execute the guest under the verified-rootfs wrapper",
    )

    with tempfile.TemporaryDirectory(prefix="uml-overlay-launcher-") as name:
        rootfs = Path(name) / "rootfs image.erofs"
        rootfs.touch()

        valid = run(arguments.launcher, rootfs, "quiet")
        require(valid.returncode == 0, f"valid dry run failed:\n{valid.stdout}")
        command = shlex.split(valid.stdout)
        require(
            f"ubd0c={rootfs}" in command
            and "root=/dev/ubda" in command
            and "rootfstype=erofs" in command,
            f"dry run lacks the raw EROFS boot options:\n{valid.stdout}",
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

    print("overlay launcher EROFS option policy tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
