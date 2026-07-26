#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot KDE Plasma in the immutable GPU guest and expose its VNC display."""

from __future__ import annotations

import argparse
import ipaddress
import os
from pathlib import Path
import sys
from typing import Sequence


REPO_ROOT = Path(__file__).resolve().parents[1]
GUEST_HARNESS = REPO_ROOT / "tests/gpu/run_gpu_guest.py"
DEFAULT_EMULATOR = REPO_ROOT / ".build/cmake-macos/bin/uml-darwin"
DEFAULT_ROOTFS = REPO_ROOT / "rootfs.img"
DEFAULT_BASELINE = REPO_ROOT / "tests/gpu/rootfs-baseline.json"

KDE_OVERLAY_COMMANDS = (
    "getent passwd plasma >/dev/null || "
    "useradd --create-home --uid 1000 --shell /bin/bash plasma",
    "usermod -a -G video,render,input plasma",
    "install -d -m 0755 -o plasma -g plasma "
    "/home/plasma/Desktop /home/plasma/Documents /home/plasma/Downloads "
    "/home/plasma/Music /home/plasma/Pictures /home/plasma/Public "
    "/home/plasma/Templates /home/plasma/Videos /home/plasma/.config "
    "/home/plasma/.local/share/Trash/files "
    "/home/plasma/.local/share/Trash/info",
    "printf '%s\\n' "
    "'XDG_DESKTOP_DIR=\"$HOME/Desktop\"' "
    "'XDG_DOWNLOAD_DIR=\"$HOME/Downloads\"' "
    "'XDG_TEMPLATES_DIR=\"$HOME/Templates\"' "
    "'XDG_PUBLICSHARE_DIR=\"$HOME/Public\"' "
    "'XDG_DOCUMENTS_DIR=\"$HOME/Documents\"' "
    "'XDG_MUSIC_DIR=\"$HOME/Music\"' "
    "'XDG_PICTURES_DIR=\"$HOME/Pictures\"' "
    "'XDG_VIDEOS_DIR=\"$HOME/Videos\"' "
    "> /home/plasma/.config/user-dirs.dirs",
    "chown plasma:plasma /home/plasma/.config/user-dirs.dirs",
    # KInfoCenter 6.3 counts Mesa's CPU-only Lavapipe Vulkan device as the
    # guest GPU and labels an otherwise accelerated Virgl session "llvmpipe".
    # This OpenGL-only guest has no Vulkan render path, so hide that software
    # ICD in the tmpfs overlay. KInfoCenter then uses its OpenGL probe, which
    # reports the same Virgl renderer used by KWin and applications.
    "rm -f /usr/share/vulkan/icd.d/lvp_icd.json",
    # The desktop:/ KIO worker crashes under UML. Keep Plasma's normal Folder
    # View containment, but point it directly at the user's Desktop directory
    # so it uses the stable local-file worker instead. This edits only the
    # tmpfs overlay; the frozen guest image remains unchanged.
    "sed -i "
    "'s#<default>desktop:/</default>#"
    "<default>file:///home/plasma/Desktop</default>#' "
    "/usr/share/plasma/plasmoids/org.kde.desktopcontainment/"
    "contents/config/main.xml",
    "install -d -m 0755 /etc/sddm.conf.d",
    "printf '%s\\n' '[Autologin]' 'User=plasma' 'Session=plasma' "
    "'Relogin=false' > /etc/sddm.conf.d/uml-autologin.conf",
)


def ipv4_address(value: str) -> str:
    try:
        return str(ipaddress.IPv4Address(value))
    except ipaddress.AddressValueError as error:
        raise argparse.ArgumentTypeError(str(error)) from error


def positive_integer(value: str) -> int:
    try:
        parsed = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError(f"not an integer: {value}") from error
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return parsed


def port_number(value: str) -> int:
    parsed = positive_integer(value)
    if parsed > 65535:
        raise argparse.ArgumentTypeError("must be at most 65535")
    return parsed


def parse_arguments(arguments: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Boot KDE Plasma Wayland with Virgl and leave an external VNC "
            "server running until Ctrl-C"
        )
    )
    parser.add_argument(
        "--bind",
        type=ipv4_address,
        default="0.0.0.0",
        help="VNC IPv4 bind address (default: all interfaces)",
    )
    parser.add_argument("--port", type=port_number, default=5900)
    parser.add_argument("--width", type=positive_integer, default=1280)
    parser.add_argument("--height", type=positive_integer, default=720)
    parser.add_argument("--memory", default="8G")
    parser.add_argument("--emulator", type=Path, default=DEFAULT_EMULATOR)
    parser.add_argument("--rootfs", type=Path, default=DEFAULT_ROOTFS)
    parser.add_argument("--baseline", type=Path, default=DEFAULT_BASELINE)
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="verify the frozen image and print the emulator command",
    )
    return parser.parse_args(arguments)


def build_harness_command(options: argparse.Namespace) -> list[str]:
    command = [
        sys.executable,
        str(GUEST_HARNESS),
        "--emulator",
        str(options.emulator),
        "--rootfs",
        str(options.rootfs),
        "--baseline",
        str(options.baseline),
        "--memory",
        options.memory,
        "--unit",
        "graphical.target",
        "--timeout",
        "0",
        "--no-auto-poweroff",
    ]
    for overlay_command in KDE_OVERLAY_COMMANDS:
        command.extend(("--command", overlay_command))
    if options.dry_run:
        command.append("--dry-run")
    command.extend(
        (
            "--",
            "--gpu",
            f"--gpu-vnc-bind={options.bind}",
            f"--gpu-vnc-port={options.port}",
            f"--gpu-width={options.width}",
            f"--gpu-height={options.height}",
        )
    )
    return command


def main(arguments: Sequence[str] | None = None) -> int:
    options = parse_arguments(arguments)
    if options.bind == "0.0.0.0":
        print(
            "VNC will listen without authentication on every IPv4 interface, "
            f"port {options.port}.",
            file=sys.stderr,
        )
        print(
            "Connect to this Mac's IPv4 address; use --bind 127.0.0.1 "
            "for local-only access.",
            file=sys.stderr,
        )
    else:
        print(
            "VNC will listen without authentication on "
            f"{options.bind}:{options.port}.",
            file=sys.stderr,
        )
    print("Press Ctrl-C in this terminal to stop the guest.", file=sys.stderr)
    os.execv(sys.executable, build_harness_command(options))
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
