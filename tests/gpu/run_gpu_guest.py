#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot the GPU test guest without ever writing the base root filesystem."""

from __future__ import annotations

import argparse
import base64
import fcntl
import hashlib
import json
import os
from pathlib import Path
import select
import shlex
import signal
import stat
import subprocess
import sys
import time
from typing import Any, Sequence


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_EMULATOR = REPO_ROOT / ".build/cmake-macos/bin/uml-darwin"
DEFAULT_ROOTFS = REPO_ROOT / "rootfs.img"
DEFAULT_BASELINE = Path(__file__).with_name("rootfs-baseline.json")
DEFAULT_ARTIFACT_ROOT = REPO_ROOT / ".build/gpu-test-artifacts"
PASS_MARKER = "UML_GPU_OVERLAY_TEST: PASS"
FAIL_MARKER = "UML_GPU_OVERLAY_TEST: FAIL"
PROTECTED_ARGUMENTS = ("ubd0", "root=", "init=", "mem=", "console=", "systemd.unit=")


class SafetyError(RuntimeError):
    """Raised when a boot could risk modifying the base image."""


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb", buffering=0) as stream:
        while chunk := stream.read(8 * 1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def image_snapshot(path: Path) -> dict[str, Any]:
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
    try:
        baseline = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise SafetyError(f"cannot read rootfs baseline {path}: {error}") from error
    if baseline.get("schema_version") != 1:
        raise SafetyError(f"unsupported rootfs baseline schema in {path}")
    if not isinstance(baseline.get("sha256"), str):
        raise SafetyError(f"rootfs baseline has no SHA-256 in {path}")
    if not isinstance(baseline.get("size"), int):
        raise SafetyError(f"rootfs baseline has no image size in {path}")
    return baseline


def enforce_image_immutable(
    rootfs: Path, baseline: dict[str, Any]
) -> tuple[int, dict[str, Any]]:
    if not rootfs.is_file():
        raise SafetyError(f"rootfs is not a regular file: {rootfs}")
    image_stat = rootfs.stat()
    mode = stat.S_IMODE(image_stat.st_mode)
    if mode & (stat.S_IWUSR | stat.S_IWGRP | stat.S_IWOTH):
        raise SafetyError(
            f"rootfs has writable permission bits ({mode:04o}); expected a frozen image"
        )

    descriptor = os.open(rootfs, os.O_RDONLY)
    try:
        fcntl.flock(descriptor, fcntl.LOCK_SH | fcntl.LOCK_NB)
        snapshot = image_snapshot(rootfs)
        if snapshot["size"] != baseline["size"]:
            raise SafetyError(
                f"rootfs size changed: {snapshot['size']} != {baseline['size']}"
            )
        if snapshot["sha256"] != baseline["sha256"]:
            raise SafetyError(
                "rootfs SHA-256 does not match the frozen installed-image baseline: "
                f"{snapshot['sha256']} != {baseline['sha256']}"
            )
        return descriptor, snapshot
    except BaseException:
        os.close(descriptor)
        raise


def validate_extra_arguments(arguments: Sequence[str]) -> None:
    for argument in arguments:
        if argument == "rw":
            raise SafetyError("the writable-root kernel argument 'rw' is forbidden")
        if argument == "ro":
            raise SafetyError("'ro' is supplied by the harness and must not be repeated")
        if any(argument.startswith(prefix) for prefix in PROTECTED_ARGUMENTS):
            raise SafetyError(f"kernel argument is controlled by the harness: {argument}")


def build_command(
    emulator: Path,
    rootfs: Path,
    root_device: str,
    memory: str,
    systemd_unit: str,
    extra_arguments: Sequence[str],
) -> list[str]:
    validate_extra_arguments(extra_arguments)
    return [
        str(emulator.resolve()),
        f"ubd0r={rootfs.resolve()}",
        f"root={root_device}",
        "ro",
        "init=/bin/sh",
        f"mem={memory}",
        "console=tty0",
        "ip=dhcp",
        f"systemd.unit={systemd_unit}",
        *extra_arguments,
    ]


def make_test_script(commands: Sequence[str], auto_poweroff: bool) -> str:
    command_block = "\n".join(commands)
    poweroff_block = """
echo "UML_GPU_OVERLAY_TEST: requesting normal poweroff"
# Ask systemd to enter poweroff.target without waiting for wall(1) delivery.
# SIGRTMIN+4 is systemd's documented orderly-poweroff request and avoids
# leaving a test guest running if its D-Bus request path is unhealthy.
timeout 10 systemctl poweroff --no-wall --no-block || kill -RTMIN+4 1
""" if auto_poweroff else """
echo "UML_GPU_OVERLAY_TEST: guest left running by request"
"""
    return f"""#!/bin/sh
set -u
status=0
exec </dev/null >/dev/console 2>&1

check_equal()
{{
    actual="$1"
    expected="$2"
    description="$3"
    if [ "$actual" != "$expected" ]; then
        echo "overlay check failed: $description: $actual != $expected"
        status=1
    fi
}}

check_equal "$(findmnt -n -o FSTYPE -T /)" overlay "root filesystem type"
case ",$(findmnt -n -o OPTIONS -T /)," in
    *,rw,*) ;;
    *) echo "overlay check failed: root overlay is not writable"; status=1 ;;
esac
check_equal "$(findmnt -n -o FSTYPE -T /.uml-lower)" ext4 "lower filesystem type"
case ",$(findmnt -n -o OPTIONS -T /.uml-lower)," in
    *,ro,*) ;;
    *) echo "overlay check failed: lower filesystem is not read-only"; status=1 ;;
esac
check_equal "$(findmnt -n -o FSTYPE -T /.uml-lower/mnt)" tmpfs "upper backing type"
check_equal "$(cat /sys/block/ubda/ro)" 1 "base block device read-only flag"

probe="/etc/.uml-overlay-write-probe"
if ! : > "$probe"; then
    echo "overlay check failed: upper layer is not writable"
    status=1
elif [ -e "/.uml-lower$probe" ]; then
    echo "overlay check failed: write reached the lower root"
    status=1
fi
rm -f "$probe"

if [ "$status" -eq 0 ]; then
    # Keep command-level shell failures (including expansion errors caused by
    # malformed test input) inside a subshell so the immutable-root result and
    # orderly-poweroff footer always run.
    (
{command_block if command_block else "        :"}
    )
    status=$?
fi

if [ "$status" -eq 0 ]; then
    echo "{PASS_MARKER}"
else
    echo "{FAIL_MARKER} status=$status"
fi
{poweroff_block}
exit "$status"
"""


def make_bootstrap(test_script: str) -> bytes:
    encoded_test = base64.b64encode(test_script.encode("utf-8")).decode("ascii")
    # The bootstrap is typed into the UML init shell through its tty. Keep each
    # physical input line comfortably below the tty's canonical-input limit so
    # large diagnostic commands cannot be truncated before systemd starts.
    encoded_arguments = " \\\n".join(
        f"    '{encoded_test[offset : offset + 512]}'"
        for offset in range(0, len(encoded_test), 512)
    )
    bootstrap = f"""set -eu
fail()
{{
    echo "Unable to construct the tmpfs overlay root: $*" >&2
    poweroff -f
}}
mount -t tmpfs -o mode=0755,nosuid,nodev tmpfs /mnt || fail "tmpfs mount"
mkdir -p /mnt/lower /mnt/upper /mnt/work /mnt/root || fail "overlay workspace"
mount -o bind / /mnt/lower || fail "read-only lower bind"
mount -t overlay overlay -o lowerdir=/mnt/lower,upperdir=/mnt/upper,workdir=/mnt/work /mnt/root ||
    fail "overlay mount"
mount -t devtmpfs devtmpfs /mnt/root/dev || fail "devtmpfs mount"
mkdir -p /mnt/root/.uml-lower || fail "old-root directory"
cd /mnt/root || fail "new-root chdir"
pivot_root . .uml-lower || fail "pivot_root"
rm -f /etc/resolv.conf
printf '%s\\n' \\
    '# Darwin UML user-mode network' \\
    'nameserver 1.1.1.1' \\
    'nameserver 8.8.8.8' \\
    'nameserver 10.0.2.3' \\
    'options timeout:1 attempts:3 rotate' > /etc/resolv.conf
mkdir -p /usr/local/libexec /etc/systemd/system/multi-user.target.wants
printf '%s' \\
{encoded_arguments} | base64 -d > /usr/local/libexec/uml-gpu-overlay-test
chmod 0755 /usr/local/libexec/uml-gpu-overlay-test
cat > /etc/systemd/system/uml-gpu-overlay-test.service <<'EOF'
[Unit]
Description=Darwin UML immutable-root GPU test guard
After=local-fs.target
Before=display-manager.service

[Service]
Type=oneshot
ExecStart=/usr/local/libexec/uml-gpu-overlay-test
RemainAfterExit=yes
StandardInput=null
StandardOutput=journal+console
StandardError=journal+console

[Install]
WantedBy=multi-user.target
EOF
ln -sf ../uml-gpu-overlay-test.service \
    /etc/systemd/system/multi-user.target.wants/uml-gpu-overlay-test.service
echo "Darwin UML root overlay: lower=read-only, upper=tmpfs"
exec /lib/systemd/systemd
"""
    return bootstrap.encode("utf-8")


def terminate_process(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=10)
    except (ProcessLookupError, subprocess.TimeoutExpired):
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=5)


def run_guest(
    command: Sequence[str],
    bootstrap: bytes,
    console_log: Path,
    timeout: float,
    expect_poweroff: bool,
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
    saw_pass = False
    saw_fail = False
    deadline = time.monotonic() + timeout if timeout > 0 else None
    pending = bytearray()

    try:
        process.stdin.write(bootstrap)
        process.stdin.write(b"\n")
        process.stdin.flush()
        with console_log.open("wb") as log:
            while process.poll() is None:
                if deadline is not None and time.monotonic() >= deadline:
                    raise TimeoutError(f"guest did not finish within {timeout:.0f}s")
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
                if len(pending) > 8192:
                    del pending[:-8192]
                decoded = pending.decode("utf-8", errors="replace")
                saw_pass = saw_pass or PASS_MARKER in decoded
                saw_fail = saw_fail or FAIL_MARKER in decoded
            return_code = process.wait(timeout=5)
    finally:
        terminate_process(process)

    if expect_poweroff and return_code != 0:
        raise RuntimeError(f"emulator exited with status {return_code}")
    return return_code, saw_pass, saw_fail


def write_json(path: Path, value: Any) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def parse_arguments(arguments: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Boot a tmpfs-overlay GPU guest while protecting rootfs.img"
    )
    parser.add_argument("--emulator", type=Path, default=DEFAULT_EMULATOR)
    parser.add_argument("--rootfs", type=Path, default=DEFAULT_ROOTFS)
    parser.add_argument("--baseline", type=Path, default=DEFAULT_BASELINE)
    parser.add_argument("--root-device", default="/dev/ubda1")
    parser.add_argument("--memory", default="8G")
    parser.add_argument("--unit", default="multi-user.target")
    parser.add_argument("--artifact-root", type=Path, default=DEFAULT_ARTIFACT_ROOT)
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument(
        "--command",
        action="append",
        default=[],
        help="shell command to run after the immutable-overlay checks (repeatable)",
    )
    parser.add_argument(
        "--no-auto-poweroff",
        action="store_true",
        help="leave the guest running after its checks instead of shutting down normally",
    )
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument(
        "kernel_arguments",
        nargs="*",
        help="additional non-root kernel arguments after '--'",
    )
    return parser.parse_args(arguments)


def main(arguments: Sequence[str] | None = None) -> int:
    options = parse_arguments(arguments)
    emulator = options.emulator.resolve()
    rootfs = options.rootfs.resolve()
    baseline = load_baseline(options.baseline.resolve())
    if not emulator.is_file() or not os.access(emulator, os.X_OK):
        raise SafetyError(f"emulator is not executable: {emulator}")

    descriptor = -1
    before: dict[str, Any] | None = None
    artifact_directory: Path | None = None
    result: dict[str, Any] = {}
    failure: BaseException | None = None
    try:
        descriptor, before = enforce_image_immutable(rootfs, baseline)
        command = build_command(
            emulator,
            rootfs,
            options.root_device,
            options.memory,
            options.unit,
            options.kernel_arguments,
        )
        test_script = make_test_script(
            options.command, auto_poweroff=not options.no_auto_poweroff
        )
        bootstrap = make_bootstrap(test_script)
        result = {
            "command": command,
            "rootfs_before": before,
            "baseline": str(options.baseline.resolve()),
            "auto_poweroff": not options.no_auto_poweroff,
        }
        if options.dry_run:
            print(shlex.join(command))
            print(f"rootfs SHA-256 verified: {before['sha256']}")
            return 0

        timestamp = time.strftime("%Y%m%d-%H%M%S")
        artifact_directory = options.artifact_root.resolve() / (
            f"{timestamp}-{os.getpid()}"
        )
        artifact_directory.mkdir(parents=True, exist_ok=False)
        write_json(artifact_directory / "run.json", result)
        return_code, saw_pass, saw_fail = run_guest(
            command,
            bootstrap,
            artifact_directory / "console.log",
            options.timeout,
            expect_poweroff=not options.no_auto_poweroff,
        )
        result.update(
            {
                "emulator_return_code": return_code,
                "saw_pass_marker": saw_pass,
                "saw_fail_marker": saw_fail,
            }
        )
        if not saw_pass or saw_fail:
            raise RuntimeError("guest immutable-overlay verification did not pass")
        return 0
    except BaseException as error:
        failure = error
        raise
    finally:
        if before is not None:
            after = image_snapshot(rootfs)
            result["rootfs_after"] = after
            unchanged = all(
                after[key] == before[key]
                for key in ("sha256", "size", "inode", "mtime_ns")
            )
            result["rootfs_unchanged"] = unchanged
            if artifact_directory is not None:
                if failure is not None:
                    result["failure"] = f"{type(failure).__name__}: {failure}"
                write_json(artifact_directory / "run.json", result)
            if not unchanged and failure is None:
                raise SafetyError("rootfs changed while the guest was running")
        if descriptor >= 0:
            os.close(descriptor)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print(
            f"{Path(sys.argv[0]).name}: interrupted; guest stopped",
            file=sys.stderr,
        )
        raise SystemExit(130) from None
    except (SafetyError, RuntimeError, TimeoutError) as error:
        print(f"{Path(sys.argv[0]).name}: {error}", file=sys.stderr)
        raise SystemExit(1) from error
