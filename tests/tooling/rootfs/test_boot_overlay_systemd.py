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
    parser.add_argument("--kernel-config", required=True, type=Path)
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
    for token in (
        "immutableExt4Rootfs(rootfsPath)",
        'arguments.emplace_back("ubd0cr=" + std::string(rootfsPath))',
        'arguments.emplace_back("rootfstype=ext4")',
        'arguments.emplace_back("ro")',
        "std::filesystem::perms::owner_write",
        "std::filesystem::perms::group_write",
        "std::filesystem::perms::others_write",
    ):
        require(
            token in runtime_source,
            "Kernarium ext4 roots must be host-immutable and attached "
            "block-read-only as the common OverlayFS lower",
        )
    require(
        runtime_source.count('arguments.emplace_back("plymouth.enable=0")') == 1,
        "Kernarium systemd boots must disable the Plymouth handoff that "
        "blocks before greetd on iOS",
    )
    systemd_policy = """if (!geekbench) {
            arguments.emplace_back("systemd.unit=graphical.target");
            arguments.emplace_back("plymouth.enable=0");
        }"""
    require(
        systemd_policy in runtime_source,
        "Kernarium must apply the Plymouth policy with its systemd boot "
        "arguments, leaving the direct Geekbench shell unchanged",
    )
    bootstrap_fragments = []
    for name in ("desktopBootstrapPrefix", "desktopBootstrapSuffix"):
        declaration = f'constexpr std::string_view {name} = R"krn('
        bootstrap_start = runtime_source.index(declaration) + len(declaration)
        bootstrap_end = runtime_source.index(')krn";', bootstrap_start)
        bootstrap_fragments.append(
            runtime_source[bootstrap_start:bootstrap_end]
        )
    kernarium_bootstrap = "".join(bootstrap_fragments)
    require(
        "mkfs_ext4=$(command -v mkfs.ext4 || true)" in kernarium_bootstrap
        and '"$mkfs_ext4" -F -q -m 0' in kernarium_bootstrap,
        "Kernarium must locate the image-provided ext4 formatter instead of "
        "assuming a Debian-only path",
    )
    kde_branch_start = kernarium_bootstrap.index(
        "if getent passwd uml >/dev/null 2>&1 && \\"
    )
    kde_branch_end = kernarium_bootstrap.index(
        "elif [ -x /usr/bin/phosh-session ]", kde_branch_start
    )
    kde_branch = kernarium_bootstrap[kde_branch_start:kde_branch_end]
    require(
        "serial-getty@" not in kernarium_bootstrap
        and "agetty" not in kernarium_bootstrap
        and "chpasswd" not in kernarium_bootstrap
        and "getty"
        not in kernarium_bootstrap[:kde_branch_start]
        + kernarium_bootstrap[kde_branch_end:],
        "Kernarium Phosh and postmarketOS setup must preserve the "
        "image-owned VT and serial getties plus their login policy",
    )
    require(
        kde_branch.count(
            "ln -sf /dev/null /etc/systemd/system/getty@tty0.service"
        )
        == 1
        and kde_branch.count(
            "/etc/systemd/system/getty.target.wants/getty@tty1.service"
        )
        == 1,
        "the KDE overlay's bounded getty relocation must mask the active-VT "
        "alias and enable exactly one fixed-VT replacement",
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
    openrc_policy = (
        "if [ -x /sbin/openrc ] && [ -x /sbin/init ]; then",
        "mount -t proc proc /proc || fail \"OpenRC proc mount\"",
        "mount -t sysfs sysfs /sys || fail \"OpenRC sysfs mount\"",
        "mount -t tmpfs -o mode=0755,nosuid,nodev tmpfs /run",
        "exec /sbin/init",
    )
    require(
        all(token in kernarium_bootstrap for token in openrc_policy),
        "Kernarium must mount OpenRC's early API filesystems and start the "
        "image-provided init",
    )
    sxmo_policy = (
        "/usr/bin/sxmo_swayinit.sh",
        "/usr/bin/tinydm-run-session",
        "getent passwd 10000",
        "/var/lib/tinydm/default-session.desktop",
        "for service in udev udev-trigger udev-settle",
        "keyword -containers",
        'deviceinfo_arch="aarch64"',
        'deviceinfo_swap_size_recommended="0"',
        'deviceinfo_zram_swap_pct="0"',
        "/etc/udev/rules.d/59-kernarium-input.rules",
        'ATTRS{name}=="Kernarium Touchscreen"',
        'ENV{ID_INPUT_TOUCHSCREEN}="1"',
        'ATTRS{name}=="Kernarium Keyboard"',
        'ENV{ID_INPUT_KEYBOARD}="1"',
        "/etc/init.d/kernarium-device-access",
        "need udev-settle",
        "before display-manager",
        'chgrp video "$node"',
        'chgrp input "$node"',
        "image Sxmo/Sway session preserved",
    )
    require(
        all(token in kernarium_bootstrap for token in sxmo_policy),
        "Kernarium must recognize the postmarketOS Sxmo/Sway image while "
        "preserving its account and tinydm session",
    )
    openrc_mounts = kernarium_bootstrap.index(
        "mount -t proc proc /proc || fail \"OpenRC proc mount\""
    )
    openrc_exec = kernarium_bootstrap.index("exec /sbin/init")
    require(
        kernarium_bootstrap.index("pivot_root . .uml-lower") < openrc_mounts
        < openrc_exec,
        "Kernarium must prepare OpenRC only after entering the writable root",
    )
    kernel_config = arguments.kernel_config.read_text(encoding="utf-8")
    for setting in (
        "CONFIG_SYN_COOKIES=y",
        "CONFIG_NF_TABLES_INET=y",
        "CONFIG_NFT_CT=y",
        "CONFIG_NFT_REJECT=y",
        "CONFIG_NFT_REJECT_INET=y",
    ):
        require(
            f"\n{setting}\n" in f"\n{kernel_config}\n",
            f"postmarketOS networking requires {setting}",
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

    print("overlay launcher rootfs option policy tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
