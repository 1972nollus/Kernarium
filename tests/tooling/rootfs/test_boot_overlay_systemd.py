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
    parser.add_argument("--kernarium-runtime", required=True, type=Path)
    arguments = parser.parse_args()

    launcher_source = arguments.launcher.read_text(encoding="utf-8")
    require(
        "getty@tty0.service" not in launcher_source
        and "agetty --autologin" not in launcher_source
        and "useradd" not in launcher_source
        and "groupadd" not in launcher_source,
        "launcher must preserve the image-owned account and getty setup",
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

    runtime_source = arguments.kernarium_runtime.read_text(encoding="utf-8")
    require(
        "plymouth.enable=0" not in runtime_source,
        "Kernarium must let the updated image's VT and serial getties own "
        "the normal Plymouth handoff",
    )
    declaration = 'constexpr std::string_view systemdBootstrap = R"krn('
    bootstrap_start = runtime_source.index(declaration)
    bootstrap_end = runtime_source.index(')krn";', bootstrap_start)
    kernarium_bootstrap = runtime_source[bootstrap_start:bootstrap_end]
    require(
        "/etc/systemd/system/getty.target.wants/" not in kernarium_bootstrap
        and "serial-getty@" not in kernarium_bootstrap
        and "agetty" not in kernarium_bootstrap
        and "chpasswd" not in kernarium_bootstrap,
        "Kernarium Phosh setup must preserve the image-owned VT and serial "
        "getties plus their login policy",
    )
    require(
        runtime_source.index('"console=tty0"')
        < runtime_source.index('"console=ttyS0"'),
        "Kernarium must retain fbcon output before the host-facing serial "
        "console",
    )
    phoc_overrides = (
        "/etc/phrog/phoc.ini",
        "/etc/phosh/phoc.ini",
    )
    require(
        all(
            kernarium_bootstrap.count(path) == 1
            and f"install -m 0644 /dev/null {path}" in kernarium_bootstrap
            for path in phoc_overrides
        ),
        "Kernarium must discard the image's fixed Virtual-1 mode for both "
        "the Phrog greeter and the logged-in Phosh session",
    )
    for path in phoc_overrides:
        override = kernarium_bootstrap.index(
            f"install -m 0644 /dev/null {path}"
        )
        require(
            kernarium_bootstrap.index("pivot_root . .uml-lower") < override
            < kernarium_bootstrap.rindex("exec /lib/systemd/systemd"),
            "Kernarium must install each Phoc override in the mounted "
            "overlay before starting systemd",
        )
    development_phosh_enablement = (
        "/etc/systemd/system/graphical.target.wants/phosh.service"
    )
    require(
        kernarium_bootstrap.count(development_phosh_enablement) == 1
        and development_phosh_enablement in kernarium_bootstrap,
        "Kernarium must select the stock Phrog/greetd display-manager path "
        "over the image's mutually exclusive development phosh.service",
    )
    require(
        "kernarium-wait-tty0-autologin" not in kernarium_bootstrap
        and "Wants=getty@tty0.service" not in kernarium_bootstrap
        and "After=getty@tty0.service" not in kernarium_bootstrap,
        "Kernarium retained the obsolete tty0/greetd ordering workaround",
    )
    phosh_removal = kernarium_bootstrap.index(
        f"rm -f {development_phosh_enablement}"
    )
    require(
        kernarium_bootstrap.index("pivot_root . .uml-lower") < phosh_removal
        < kernarium_bootstrap.rindex("exec /lib/systemd/systemd"),
        "Kernarium must remove only the stale development Phosh enablement "
        "from the mounted overlay before starting systemd",
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
