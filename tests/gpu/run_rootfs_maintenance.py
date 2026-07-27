#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Perform the explicitly authorized package-maintenance boot of rootfs.img."""

from __future__ import annotations

import argparse
import base64
import fcntl
import hashlib
import json
import os
from pathlib import Path
import select
import stat
import subprocess
import sys
import time
from typing import Any, Sequence


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_EMULATOR = REPO_ROOT / ".build/cmake-macos/bin/uml-darwin"
DEFAULT_ROOTFS = REPO_ROOT / "rootfs.img"
DEFAULT_BASELINE = Path(__file__).with_name("rootfs-baseline.json")
DEFAULT_ARTIFACT_ROOT = REPO_ROOT / ".build/gpu-rootfs-maintenance"
PASS_MARKER = "UML_GPU_ROOTFS_MAINTENANCE: PASS"
FAIL_MARKER = "UML_GPU_ROOTFS_MAINTENANCE: FAIL"
FROZEN_MODE = 0o444
MAINTENANCE_MODE = 0o644


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb", buffering=0) as stream:
        while chunk := stream.read(8 * 1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def snapshot(path: Path) -> dict[str, Any]:
    image_stat = path.stat()
    return {
        "path": str(path.resolve()),
        "sha256": sha256_file(path),
        "size": image_stat.st_size,
        "inode": image_stat.st_ino,
        "mtime_ns": image_stat.st_mtime_ns,
        "mode": stat.S_IMODE(image_stat.st_mode),
    }


def load_baseline(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if value.get("schema_version") != 1:
        raise RuntimeError(f"unsupported baseline schema in {path}")
    return value


def maintenance_script() -> str:
    # Keep the complete Virgl OpenGL userspace family on one backports version.
    # Mixing GBM/EGL/Gallium versions makes KDE validation ambiguous.
    packages = (
        "mesa-libgallium",
        "libgl1-mesa-dri",
        "libgbm1",
        "libgbm-dev",
        "libegl-mesa0",
        "libegl1-mesa-dev",
        "libglx-mesa0",
    )
    package_arguments = " \\\n        ".join(packages)
    return f"""#!/bin/sh
set -u
status=0

(
    set -e
    export DEBIAN_FRONTEND=noninteractive
    cat > /etc/apt/sources.list.d/trixie-backports.sources <<'EOF'
Types: deb
URIs: http://deb.debian.org/debian
Suites: trixie-backports
Components: main contrib non-free-firmware
Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg
EOF
    apt-get update
    apt-get install -y -t trixie-backports \\
        {package_arguments}
    apt-get install -y chromium chromium-sandbox
    mesa_version="$(dpkg-query -W -f='${{Version}}' mesa-libgallium)"
    case "$mesa_version" in
        26.*) ;;
        *) echo "Expected Mesa 26, installed $mesa_version"; exit 1 ;;
    esac
    chromium_version="$(dpkg-query -W -f='${{Version}}' chromium)"
    test -n "$chromium_version"
    install -d -m 0755 /etc/chromium.d
    cat > /etc/chromium.d/uml-virgl <<'EOF'
# ANGLE's GLES backend runs on Mesa Virgl, while WebGL is allowed to fall
# back to its accelerated ES 2 context.
CHROMIUM_FLAGS="${{CHROMIUM_FLAGS}} --ozone-platform=x11"
CHROMIUM_FLAGS="${{CHROMIUM_FLAGS}} --use-gl=angle --use-angle=gles-egl"
CHROMIUM_FLAGS="${{CHROMIUM_FLAGS}} --ignore-gpu-blocklist --enable-webgl"
CHROMIUM_FLAGS="${{CHROMIUM_FLAGS}} --disable-features=FallbackToSWIfGLES3NotSupported,Vulkan,WebGPUService,WebGPUCompatibilityMode"
EOF
    chmod 0644 /etc/chromium.d/uml-virgl
    dpkg-query -W -f='installed ${{binary:Package}}=${{Version}}\\n' \\
        {package_arguments} chromium chromium-sandbox
    grep -F -- '--use-angle=gles-egl' /etc/chromium.d/uml-virgl
    apt-get clean
) || status=$?

# The maintenance unit is deliberately transient. Keep only the backports
# source, installed packages, and Chromium Virgl configuration in the image.
rm -f /etc/systemd/system/uml-gpu-rootfs-maintenance.service
rm -f /usr/local/libexec/uml-gpu-rootfs-maintenance
sync

if [ "$status" -eq 0 ]; then
    echo "{PASS_MARKER}"
else
    echo "{FAIL_MARKER} status=$status"
fi
echo "UML_GPU_ROOTFS_MAINTENANCE: requesting orderly systemd poweroff"
timeout 10 systemctl poweroff --no-wall --no-block || kill -RTMIN+4 1
exit "$status"
"""


def bootstrap(script: str) -> bytes:
    encoded = base64.b64encode(script.encode("utf-8")).decode("ascii")
    return f"""set -eu
mount -o remount,rw /
mkdir -p /usr/local/libexec /etc/systemd/system
printf '%s' '{encoded}' | base64 -d > /usr/local/libexec/uml-gpu-rootfs-maintenance
chmod 0755 /usr/local/libexec/uml-gpu-rootfs-maintenance
cat > /etc/systemd/system/uml-gpu-rootfs-maintenance.service <<'EOF'
[Unit]
Description=Install the frozen-image GPU and Chromium userspace
Wants=network-online.target
After=network-online.target

[Service]
Type=oneshot
ExecStart=/usr/local/libexec/uml-gpu-rootfs-maintenance
StandardInput=null
StandardOutput=journal+console
StandardError=journal+console
EOF
sync
exec /lib/systemd/systemd
""".encode("utf-8")


def run(
    command: Sequence[str], input_data: bytes, log_path: Path, timeout: float
) -> tuple[int, bool, bool]:
    process = subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
        start_new_session=True,
    )
    assert process.stdin is not None
    assert process.stdout is not None
    process.stdin.write(input_data)
    process.stdin.write(b"\n")
    process.stdin.flush()

    saw_pass = False
    saw_fail = False
    pending = bytearray()
    deadline = time.monotonic() + timeout if timeout > 0 else None
    with log_path.open("wb") as log:
        while process.poll() is None:
            if deadline is not None and time.monotonic() >= deadline:
                # Never terminate a writable-root guest. Once the warning has
                # fired, keep supervising until systemd completes its orderly
                # shutdown.
                print(
                    f"Maintenance exceeded {timeout:.0f}s; continuing to wait "
                    "for orderly poweroff",
                    file=sys.stderr,
                )
                deadline = None
            readable, _, _ = select.select([process.stdout], [], [], 0.25)
            if not readable:
                continue
            data = os.read(process.stdout.fileno(), 65536)
            if not data:
                break
            log.write(data)
            log.flush()
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()
            pending.extend(data)
            if len(pending) > 16384:
                del pending[:-16384]
            text = pending.decode("utf-8", errors="replace")
            saw_pass = saw_pass or PASS_MARKER in text
            saw_fail = saw_fail or FAIL_MARKER in text
    return process.wait(timeout=10), saw_pass, saw_fail


def parse_arguments(arguments: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--emulator", type=Path, default=DEFAULT_EMULATOR)
    parser.add_argument("--rootfs", type=Path, default=DEFAULT_ROOTFS)
    parser.add_argument("--baseline", type=Path, default=DEFAULT_BASELINE)
    parser.add_argument("--artifact-root", type=Path, default=DEFAULT_ARTIFACT_ROOT)
    parser.add_argument(
        "--timeout",
        type=float,
        default=900.0,
        help="warn after this many seconds, but never kill a writable-root guest",
    )
    return parser.parse_args(arguments)


def main(arguments: Sequence[str] | None = None) -> int:
    options = parse_arguments(arguments)
    emulator = options.emulator.resolve()
    rootfs = options.rootfs.resolve()
    baseline = load_baseline(options.baseline.resolve())
    before = snapshot(rootfs)
    if before["sha256"] != baseline.get("sha256"):
        raise RuntimeError("rootfs.img does not match the frozen baseline")
    if before["size"] != baseline.get("size"):
        raise RuntimeError("rootfs.img size does not match the frozen baseline")
    if before["mode"] != FROZEN_MODE:
        raise RuntimeError(
            f"rootfs.img mode is {before['mode']:04o}, expected {FROZEN_MODE:04o}"
        )

    options.artifact_root.mkdir(parents=True, exist_ok=True)
    artifact = options.artifact_root / time.strftime("%Y%m%d-%H%M%S")
    artifact.mkdir()
    result_path = artifact / "result.json"
    log_path = artifact / "console.log"
    # Prove that no existing maintenance process holds the image, then release
    # the advisory lock before launch. UML's UBD driver takes its own exclusive
    # lock and must be the sole owner for the duration of the writable boot.
    descriptor = os.open(rootfs, os.O_RDONLY)
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
    finally:
        os.close(descriptor)
    command = [
        str(emulator),
        f"ubd0={rootfs}",
        "root=/dev/ubda1",
        "rw",
        "init=/bin/sh",
        "mem=8G",
        "console=tty0",
        "ip=dhcp",
        "systemd.unit=uml-gpu-rootfs-maintenance.service",
    ]
    result: dict[str, Any] = {"command": command, "rootfs_before": before}
    try:
        os.chmod(rootfs, MAINTENANCE_MODE)
        return_code, saw_pass, saw_fail = run(
            command, bootstrap(maintenance_script()), log_path, options.timeout
        )
        result.update(
            {
                "emulator_return_code": return_code,
                "saw_pass_marker": saw_pass,
                "saw_fail_marker": saw_fail,
            }
        )
        if return_code != 0 or not saw_pass or saw_fail:
            raise RuntimeError("rootfs package maintenance service did not pass")
    except BaseException as error:
        result["failure"] = f"{type(error).__name__}: {error}"
        raise
    finally:
        os.chmod(rootfs, FROZEN_MODE)
        after = snapshot(rootfs)
        result["rootfs_after"] = after
        result_path.write_text(
            json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        print(f"Frozen rootfs snapshot: {json.dumps(after, sort_keys=True)}")
        print(f"Maintenance artifacts: {artifact}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, TimeoutError, json.JSONDecodeError) as error:
        print(f"run_rootfs_maintenance.py: {error}", file=sys.stderr)
        raise SystemExit(1)
