#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot KDE Plasma from a read-only lower image in a native or VNC display."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import ipaddress
import os
from pathlib import Path
import sys
from typing import Sequence


REPO_ROOT = Path(__file__).resolve().parents[1]
GUEST_HARNESS = REPO_ROOT / "tests/integration/gpu/run_gpu_guest.py"
DEFAULT_EMULATOR = REPO_ROOT / ".build/presets/macos-integration/bin/uml-darwin"
DEFAULT_ROOTFS = (
    REPO_ROOT / ".build/rootfs/starter/debian-13-plasma-wayland.img"
)
# A starter image is a partitioned ext4 disk that owns its session and
# network policy: its getty@tty1 autologin starts Plasma, and it configures
# DHCP itself. A legacy image is the unified KDE filesystem, rootfs.erofs, or
# an ext4 derivative sealed from it: an unpartitioned filesystem whose SDDM
# autologin starts Plasma and whose gettys must stay off the display manager's
# VT. The filesystem type selects the usual profile; runners that accept
# legacy derivatives name the profile explicitly.
IMAGE_PROFILES = ("starter", "legacy")
DEFAULT_IMAGE_PROFILE_BY_TYPE = {
    "erofs": "legacy",
    "ext4": "starter",
}
ROOT_DEVICE_BY_IMAGE_PROFILE = {
    "legacy": "/dev/ubda",
    "starter": "/dev/ubda1",
}
CDROM_DEVICE = "/dev/ubdb"
CDROM_MOUNT_POINT = "/mnt/cdrom"
ISO_MOUNT_COMMAND = (
    f"install -d -m 0755 {CDROM_MOUNT_POINT}; "
    f"mount -t iso9660 -o ro {CDROM_DEVICE} {CDROM_MOUNT_POINT}; "
    f"findmnt -n -o SOURCE,FSTYPE,OPTIONS -T {CDROM_MOUNT_POINT}"
)

STARTER_KDE_OVERLAY_COMMANDS = (
    "getent passwd debian >/dev/null",
    # install -d gives the requested owner only to the directories it names,
    # so every parent below the home directory is named before its children.
    "install -d -m 0755 -o debian -g debian "
    "/home/debian/Desktop /home/debian/Documents /home/debian/Downloads "
    "/home/debian/Music /home/debian/Pictures /home/debian/Public "
    "/home/debian/Templates /home/debian/Videos /home/debian/.config "
    "/home/debian/.local /home/debian/.local/share "
    "/home/debian/.local/share/Trash "
    "/home/debian/.local/share/pki "
    "/home/debian/.local/share/Trash/files "
    "/home/debian/.local/share/Trash/info",
    "printf '%s\\n' "
    "'XDG_DESKTOP_DIR=\"$HOME/Desktop\"' "
    "'XDG_DOWNLOAD_DIR=\"$HOME/Downloads\"' "
    "'XDG_TEMPLATES_DIR=\"$HOME/Templates\"' "
    "'XDG_PUBLICSHARE_DIR=\"$HOME/Public\"' "
    "'XDG_DOCUMENTS_DIR=\"$HOME/Documents\"' "
    "'XDG_MUSIC_DIR=\"$HOME/Music\"' "
    "'XDG_PICTURES_DIR=\"$HOME/Pictures\"' "
    "'XDG_VIDEOS_DIR=\"$HOME/Videos\"' "
    "> /home/debian/.config/user-dirs.dirs",
    "chown debian:debian /home/debian/.config/user-dirs.dirs",
    # KInfoCenter 6.3 counts Mesa's CPU-only Lavapipe Vulkan device as the
    # guest GPU and labels an otherwise accelerated Virgl session "llvmpipe".
    # This OpenGL-only guest has no Vulkan render path, so hide that software
    # ICD in the disposable overlay.
    "rm -f /usr/share/vulkan/icd.d/lvp_icd.json",
    # The desktop:/ KIO worker crashes under UML. Keep Plasma's normal Folder
    # View containment, but use the stable local-file worker for this session.
    "sed -i "
    "'s#<default>desktop:/</default>#"
    "<default>file:///home/debian/Desktop</default>#' "
    "/usr/share/plasma/plasmoids/org.kde.desktopcontainment/"
    "contents/config/main.xml",
    "printf '%s\\n' '[Daemon]' 'Autolock=false' 'LockOnResume=false' "
    "'Timeout=0' > /home/debian/.config/kscreenlockerrc; "
    "chown debian:debian /home/debian/.config/kscreenlockerrc",
)

# Legacy images retain the compatibility setup owned by the unified KDE image.
# The default starter image uses the smaller overlay above and keeps its
# account, getty, and session policy image-owned.
LEGACY_KDE_OVERLAY_COMMANDS = (
    "getent passwd uml >/dev/null",
    "usermod -a -G video,render,input uml",
    # /dev/tty0 is the active-VT alias, while tty1 is the VT SDDM initially
    # claims before starting the Wayland session. A getty on either device can
    # cancel display-manager activation or follow the compositor onto its VT.
    # The attachable ttyS0 socket remains the interactive recovery console.
    "systemctl mask --now getty@tty0.service getty@tty1.service",
    "install -d -m 0755 -o uml -g uml "
    "/home/uml/Desktop /home/uml/Documents /home/uml/Downloads "
    "/home/uml/Music /home/uml/Pictures /home/uml/Public "
    "/home/uml/Templates /home/uml/Videos /home/uml/.config "
    "/home/uml/.local /home/uml/.local/share /home/uml/.local/share/Trash "
    "/home/uml/.local/share/pki "
    "/home/uml/.local/share/Trash/files "
    "/home/uml/.local/share/Trash/info "
    # SDDM redirects the Wayland session's stderr into this directory. When it
    # is absent the redirect fails, the session runs with a broken stderr, and
    # every reason a failed bring-up would have reported is discarded.
    "/home/uml/.local/share/sddm",
    "printf '%s\\n' "
    "'XDG_DESKTOP_DIR=\"$HOME/Desktop\"' "
    "'XDG_DOWNLOAD_DIR=\"$HOME/Downloads\"' "
    "'XDG_TEMPLATES_DIR=\"$HOME/Templates\"' "
    "'XDG_PUBLICSHARE_DIR=\"$HOME/Public\"' "
    "'XDG_DOCUMENTS_DIR=\"$HOME/Documents\"' "
    "'XDG_MUSIC_DIR=\"$HOME/Music\"' "
    "'XDG_PICTURES_DIR=\"$HOME/Pictures\"' "
    "'XDG_VIDEOS_DIR=\"$HOME/Videos\"' "
    "> /home/uml/.config/user-dirs.dirs",
    "chown uml:uml /home/uml/.config/user-dirs.dirs",
    # KInfoCenter 6.3 counts Mesa's CPU-only Lavapipe Vulkan device as the
    # guest GPU and labels an otherwise accelerated Virgl session "llvmpipe".
    # This OpenGL-only guest has no Vulkan render path, so hide that software
    # ICD in the tmpfs overlay. KInfoCenter then uses its OpenGL probe, which
    # reports the same Virgl renderer used by KWin and applications.
    "rm -f /usr/share/vulkan/icd.d/lvp_icd.json",
    # The desktop:/ KIO worker crashes under UML. Keep Plasma's normal Folder
    # View containment, but point it directly at the user's Desktop directory
    # so it uses the stable local-file worker instead. This edits only the
    # tmpfs overlay; the EROFS guest image remains unchanged.
    "sed -i "
    "'s#<default>desktop:/</default>#"
    "<default>file:///home/uml/Desktop</default>#' "
    "/usr/share/plasma/plasmoids/org.kde.desktopcontainment/"
    "contents/config/main.xml",
    "install -d -m 0755 /etc/sddm.conf.d",
    "printf '%s\\n' '[Autologin]' 'User=uml' 'Session=plasma' "
    "'Relogin=false' > /etc/sddm.conf.d/uml-autologin.conf",
    # A benchmark must remain visible and continuously rendered after its
    # warm-up period. Disable Plasma's idle lock in the disposable overlay so
    # long diagnostic or measurement runs cannot be replaced by the lock
    # screen.
    "printf '%s\\n' '[Daemon]' 'Autolock=false' 'LockOnResume=false' "
    "'Timeout=0' > /home/uml/.config/kscreenlockerrc; "
    "chown uml:uml /home/uml/.config/kscreenlockerrc",
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
    try:
        parsed = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("not an integer") from error
    if parsed < 0:
        raise argparse.ArgumentTypeError("must not be negative")
    if parsed > 65535:
        raise argparse.ArgumentTypeError("must be at most 65535")
    return parsed


def vnc_password(value: str) -> str:
    length = len(value.encode("utf-8"))
    if length == 0 or length > 8:
        raise argparse.ArgumentTypeError(
            "must contain between 1 and 8 UTF-8 bytes"
        )
    return value


def iso_image(value: str) -> Path:
    candidate = Path(value).expanduser()
    try:
        resolved = candidate.resolve(strict=True)
    except OSError as error:
        raise argparse.ArgumentTypeError(
            f"cannot access ISO image {candidate}: {error}"
        ) from error
    if not resolved.is_file() or not os.access(resolved, os.R_OK):
        raise argparse.ArgumentTypeError(
            f"not a readable regular file: {resolved}"
        )
    return resolved


def parse_arguments(arguments: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Boot KDE Plasma Wayland with Virgl in a native Metal window "
            "(or an external VNC server) until Ctrl-C"
        )
    )
    parser.add_argument(
        "--display", choices=("native", "vnc"), default="native",
        help="display backend (default: native)")
    parser.add_argument(
        "--bind",
        type=ipv4_address,
        default=None,
        help="VNC IPv4 bind address (default: 127.0.0.1)",
    )
    parser.add_argument(
        "--port", type=port_number, default=None,
        help="VNC TCP port (0 selects a dynamically bound port)")
    parser.add_argument(
        "--password",
        type=vnc_password,
        help=(
            "enable VNC password authentication (the VNC protocol accepts "
            "at most 8 bytes)"
        ),
    )
    parser.add_argument("--width", type=positive_integer, default=1280)
    parser.add_argument("--height", type=positive_integer, default=720)
    parser.add_argument(
        "--dpi",
        type=positive_integer,
        help="advertise an effective display density through Virtio-GPU EDID",
    )
    parser.add_argument("--cpus", type=positive_integer, default=10)
    parser.add_argument(
        "--engine",
        choices=("jit", "gadgets"),
        default="jit",
    )
    parser.add_argument("--memory", default="8G")
    parser.add_argument("--emulator", type=Path, default=DEFAULT_EMULATOR)
    parser.add_argument(
        "--rootfs",
        type=Path,
        default=DEFAULT_ROOTFS,
        help="read-only lower disk (default: codec-enabled Plasma starter)",
    )
    parser.add_argument(
        "--rootfs-type",
        choices=("erofs", "ext4"),
        default="ext4",
        help="lower filesystem type (default: ext4)",
    )
    parser.add_argument(
        "--image-profile",
        choices=IMAGE_PROFILES,
        help=(
            "session, getty, and network policy of the lower image "
            "(default: starter for ext4, legacy for erofs)"
        ),
    )
    parser.add_argument(
        "--root-device",
        help=(
            "guest root device (default: /dev/ubda1 for a starter image, "
            "/dev/ubda for a legacy image)"
        ),
    )
    parser.add_argument(
        "--iso",
        type=iso_image,
        help=(
            "attach an ISO image read-only and mount it at /mnt/cdrom in "
            "the guest"
        ),
    )
    parser.add_argument(
        "--serial-socket",
        type=Path,
        help=(
            "expose ttyS0 at this Unix-domain socket for an attachable "
            "interactive console"
        ),
    )
    parser.add_argument(
        "--virtio-console-socket",
        type=Path,
        help=(
            "attach guest hvc0 at this reconnectable Unix-domain socket"
        ),
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="print the emulator command without booting",
    )
    options = parser.parse_args(arguments)
    if options.display != "vnc" and any(
        value is not None
        for value in (options.bind, options.port, options.password)
    ):
        parser.error("--bind, --port, and --password require --display vnc")
    if options.bind is None:
        options.bind = "127.0.0.1"
    if options.port is None:
        options.port = 5900
    if options.image_profile is None:
        options.image_profile = DEFAULT_IMAGE_PROFILE_BY_TYPE[
            options.rootfs_type
        ]
    if options.image_profile == "starter" and options.rootfs_type != "ext4":
        parser.error("a starter image is an ext4 disk")
    if options.root_device is None:
        options.root_device = ROOT_DEVICE_BY_IMAGE_PROFILE[
            options.image_profile
        ]
    if (
        options.serial_socket is not None
        and options.virtio_console_socket is not None
        and options.serial_socket.resolve()
        == options.virtio_console_socket.resolve()
    ):
        parser.error("serial and Virtio console sockets must be different")
    return options


def build_harness_command(options: argparse.Namespace) -> list[str]:
    command = [
        sys.executable,
        str(GUEST_HARNESS),
        "--emulator",
        str(options.emulator),
        "--rootfs",
        str(options.rootfs),
        "--rootfs-type",
        options.rootfs_type,
        "--root-device",
        options.root_device,
        "--memory",
        options.memory,
        "--unit",
        "graphical.target",
        "--timeout",
        "0",
        "--no-auto-poweroff",
    ]
    overlay_commands = (
        STARTER_KDE_OVERLAY_COMMANDS
        if options.image_profile == "starter"
        else LEGACY_KDE_OVERLAY_COMMANDS
    )
    for overlay_command in overlay_commands:
        command.extend(("--command", overlay_command))
    if options.iso is not None:
        command.extend(("--command", ISO_MOUNT_COMMAND))
    if options.serial_socket is not None:
        command.extend(("--serial-socket", str(options.serial_socket)))
    if options.virtio_console_socket is not None:
        command.extend(
            ("--virtio-console-socket", str(options.virtio_console_socket))
        )
    if options.dry_run:
        command.append("--dry-run")
    command.extend((
        "--",
        f"ncpus={options.cpus}",
        f"--execution-engine={options.engine}",
    ))
    if options.image_profile == "starter":
        # Starter images own DHCP/DNS. This later argument overrides the GPU
        # harness's legacy kernel-DHCP default. Their Plasma session starts
        # from the image's getty@tty1 autologin, so its getties stay unmasked.
        command.append("ip=off")
    else:
        # Legacy SDDM images must keep a getty off the display manager's VT
        # from the start of boot, as their overlay setup explains. Their
        # getty@tty0 autologin otherwise opens the VT that SDDM has just given
        # the Plasma session and hangs it up.
        command.extend((
            "systemd.mask=getty@tty0.service",
            "systemd.mask=getty@tty1.service",
        ))
    if options.serial_socket is not None:
        command.append("systemd.wants=serial-getty@ttyS0.service")
    if options.virtio_console_socket is None:
        command.append("systemd.mask=serial-getty@hvc0.service")
    else:
        command.append("systemd.wants=serial-getty@hvc0.service")
    if options.iso is not None:
        command.append(f"ubd1r={options.iso}")
    command.extend(("--audio", "--gpu", f"--gpu-display={options.display}"))
    if options.display == "vnc":
        command.extend((
            f"--gpu-vnc-bind={options.bind}",
            f"--gpu-vnc-port={options.port}",
        ))
        if options.password is not None:
            command.append(f"--gpu-vnc-password={options.password}")
    command.extend(
        (
            f"--gpu-width={options.width}",
            f"--gpu-height={options.height}",
        )
    )
    if options.dpi is not None:
        command.append(f"--gpu-dpi={options.dpi}")
    return command


def guest_argument_separator(command: Sequence[str]) -> int:
    """Locate the GPU harness kernel-argument separator."""
    separators = [index for index, argument in enumerate(command) if argument == "--"]
    if not separators:
        raise ValueError("GPU harness command has no kernel-argument separator")
    return separators[-1]


def main(arguments: Sequence[str] | None = None) -> int:
    options = parse_arguments(arguments)
    if options.display == "native":
        print("A native Metal display window will open.", file=sys.stderr)
    authentication = (
        "with password authentication"
        if options.password is not None
        else "without authentication"
    )
    if options.display == "vnc":
        print(
            f"VNC will listen {authentication} on "
            f"{options.bind}:{options.port}.",
            file=sys.stderr,
        )
    if options.display == "vnc" and options.bind == "0.0.0.0":
        print(
            "The server is reachable on every IPv4 interface; use "
            "--bind 127.0.0.1 for local-only access.",
            file=sys.stderr,
        )
    if options.serial_socket is not None:
        print(
            f"Serial console will listen on {options.serial_socket}.",
            file=sys.stderr,
        )
    if options.virtio_console_socket is not None:
        print(
            "Virtio console will expose hvc0 on "
            f"{options.virtio_console_socket}.",
            file=sys.stderr,
        )
    if options.iso is not None:
        print(
            f"ISO {options.iso} will be mounted read-only at "
            f"{CDROM_MOUNT_POINT}.",
            file=sys.stderr,
        )
    print("Press Ctrl-C in this terminal to stop the guest.", file=sys.stderr)
    command = build_harness_command(options)
    os.execv(command[0], command)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
