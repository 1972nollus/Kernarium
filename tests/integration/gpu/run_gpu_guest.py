#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot the GPU test guest from the unified EROFS root filesystem."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import base64
import io
import json
import os
from pathlib import Path
import select
import shlex
import signal
import subprocess
import sys
import tarfile
import time
from typing import Any, BinaryIO, Sequence


REPO_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_EMULATOR = REPO_ROOT / ".build/presets/macos-integration/bin/uml-darwin"
DEFAULT_ROOTFS = REPO_ROOT / "rootfs.erofs"
DEFAULT_ARTIFACT_ROOT = REPO_ROOT / ".build/gpu-test-artifacts"
PASS_MARKER = "UML_GPU_OVERLAY_TEST: PASS"
FAIL_MARKER = "UML_GPU_OVERLAY_TEST: FAIL"
DOCKER_HELLO_WORLD_IMAGE = "uml/hello-world:local"
MAX_DOCKER_HELLO_WORLD_SIZE = 1024 * 1024
PROTECTED_ARGUMENTS = (
    "ubd0",
    "root=",
    "rootfstype=",
    "init=",
    "mem=",
    "console=",
    "systemd.unit=",
)


class SafetyError(RuntimeError):
    """Raised when a boot could risk modifying the base image."""


def validate_extra_arguments(arguments: Sequence[str]) -> None:
    for argument in arguments:
        if any(argument.startswith(prefix) for prefix in PROTECTED_ARGUMENTS):
            raise SafetyError(f"kernel argument is controlled by the harness: {argument}")


def load_docker_hello_world(path: Path) -> bytes:
    try:
        payload = path.read_bytes()
    except OSError as error:
        raise SafetyError(
            f"cannot read Docker hello-world payload {path}: {error}"
        ) from error
    if not payload.startswith(b"\x7fELF"):
        raise SafetyError(f"Docker hello-world payload is not an ELF file: {path}")
    if not payload or len(payload) > MAX_DOCKER_HELLO_WORLD_SIZE:
        raise SafetyError(
            "Docker hello-world payload has an invalid size: "
            f"{len(payload)} bytes"
        )
    return payload


def make_docker_rootfs_archive(payload: bytes) -> bytes:
    archive_stream = io.BytesIO()
    member = tarfile.TarInfo("hello")
    member.mode = 0o755
    member.uid = 0
    member.gid = 0
    member.uname = "root"
    member.gname = "root"
    member.mtime = int(time.time())
    member.size = len(payload)
    with tarfile.open(
        fileobj=archive_stream,
        mode="w",
        format=tarfile.USTAR_FORMAT,
    ) as archive:
        archive.addfile(member, io.BytesIO(payload))
    return archive_stream.getvalue()


def make_docker_hello_world_smoke(payload: bytes) -> str:
    encoded_archive = base64.b64encode(
        make_docker_rootfs_archive(payload)
    ).decode("ascii")
    encoded_lines = " \\\n".join(
        f"        '{encoded_archive[offset : offset + 512]}'"
        for offset in range(0, len(encoded_archive), 512)
    )
    return f"""docker_hello_world_image='{DOCKER_HELLO_WORLD_IMAGE}'
docker_hello_world_container='uml-hello-world-smoke'
docker_hello_world_archive='/run/uml-docker-hello-world.tar'
docker_hello_world_output='/run/uml-docker-hello-world.out'
docker_hello_world_cleanup()
{{
    timeout 10 docker rm -f \\
        "$docker_hello_world_container" >/dev/null 2>&1 || true
    timeout 10 docker image rm -f \\
        "$docker_hello_world_image" >/dev/null 2>&1 || true
    rm -f "$docker_hello_world_archive" "$docker_hello_world_output"
}}
trap docker_hello_world_cleanup EXIT HUP INT TERM
printf '%s' \\
{encoded_lines} | base64 -d > "$docker_hello_world_archive"
timeout 30 docker import \\
    "$docker_hello_world_archive" "$docker_hello_world_image" >/dev/null
docker image inspect "$docker_hello_world_image" >/dev/null
if ! timeout 30 docker run \\
    --name "$docker_hello_world_container" \\
    --rm --pull=never --network none \\
    "$docker_hello_world_image" /hello > "$docker_hello_world_output" 2>&1
then
    cat "$docker_hello_world_output"
    exit 1
fi
grep -Fxq 'Hello from Docker!' "$docker_hello_world_output"
cat "$docker_hello_world_output"
trap - EXIT HUP INT TERM
docker_hello_world_cleanup"""


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
        f"ubd0c={rootfs.resolve()}",
        f"root={root_device}",
        "rootfstype=erofs",
        "init=/bin/sh",
        f"mem={memory}",
        "console=tty0",
        "console=ttyS0",
        "ip=dhcp",
        f"systemd.unit={systemd_unit}",
        *extra_arguments,
    ]


def make_test_script(
    commands: Sequence[str],
    auto_poweroff: bool,
    docker_hello_world: bytes | None = None,
) -> str:
    command_parts = list(commands)
    if docker_hello_world is not None:
        command_parts.append(make_docker_hello_world_smoke(docker_hello_world))
    command_block = "\n".join(command_parts)
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
check_equal "$(findmnt -n -o FSTYPE -T /.uml-lower)" erofs "lower filesystem type"
check_equal "$(findmnt -n -o FSTYPE -T /.uml-lower/mnt)" tmpfs "upper backing type"
check_equal "$(findmnt -n -o FSTYPE -T /var/lib/docker)" tmpfs "Docker state filesystem type"
check_equal "$(findmnt -n -o FSTYPE -T /var/lib/containerd)" tmpfs "containerd state filesystem type"

probe="/etc/.uml-overlay-write-probe"
if ! : > "$probe"; then
    echo "overlay check failed: upper layer is not writable"
    status=1
fi
rm -f "$probe"

if [ "$status" -eq 0 ]; then
    # Keep command-level shell failures (including expansion errors caused by
    # malformed test input) inside a subshell so the EROFS-root result and
    # orderly-poweroff footer always run.
    (
        set -e
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
mount -o bind / /mnt/lower || fail "EROFS lower bind"
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
mkdir -p /var/lib/docker
mount -t tmpfs -o mode=0711,nosuid,nodev tmpfs /var/lib/docker || fail "Docker state tmpfs"
mkdir -p /var/lib/containerd
mount -t tmpfs -o mode=0711,nosuid,nodev tmpfs /var/lib/containerd || fail "containerd state tmpfs"
mkdir -p /usr/local/libexec /etc/systemd/system/multi-user.target.wants
printf '%s' \\
{encoded_arguments} | base64 -d > /usr/local/libexec/uml-gpu-overlay-test
chmod 0755 /usr/local/libexec/uml-gpu-overlay-test
cat > /etc/systemd/system/uml-gpu-overlay-test.service <<'EOF'
[Unit]
Description=Darwin UML EROFS-root GPU test guard
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
echo "Darwin UML root overlay: lower=erofs, upper=tmpfs"
exec /lib/systemd/systemd
"""
    return bootstrap.encode("utf-8")


def terminate_process(process: subprocess.Popen[bytes]) -> None:
    """Stop a guest that shares this runner's process group.

    The guest runs attached, so a runner killed outright takes it along instead
    of leaving an orphan. That matters because an orphaned guest boots to idle
    and still burns one to two host cores indefinitely.
    """
    if process.poll() is not None:
        return
    try:
        process.send_signal(signal.SIGTERM)
        process.wait(timeout=10)
    except (ProcessLookupError, subprocess.TimeoutExpired):
        if process.poll() is None:
            process.send_signal(signal.SIGKILL)
            process.wait(timeout=5)


def write_all(stream: BinaryIO, payload: bytes) -> None:
    """Write an entire init-shell payload even when the pipe short-writes."""
    offset = 0
    while offset < len(payload):
        try:
            written = stream.write(payload[offset:])
        except InterruptedError:
            continue
        except BlockingIOError:
            select.select([], [stream], [], 0.25)
            continue
        if written is None or written <= 0:
            raise RuntimeError("guest bootstrap pipe stopped accepting input")
        offset += written


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
    )
    assert process.stdin is not None
    assert process.stdout is not None
    saw_pass = False
    saw_fail = False
    deadline = time.monotonic() + timeout if timeout > 0 else None
    pending = bytearray()

    try:
        write_all(process.stdin, bootstrap + b"\n")
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
        description="Boot a tmpfs-overlay GPU guest from rootfs.erofs"
    )
    parser.add_argument("--emulator", type=Path, default=DEFAULT_EMULATOR)
    parser.add_argument("--rootfs", type=Path, default=DEFAULT_ROOTFS)
    parser.add_argument("--root-device", default="/dev/ubda")
    parser.add_argument("--memory", default="8G")
    parser.add_argument("--unit", default="multi-user.target")
    parser.add_argument("--artifact-root", type=Path, default=DEFAULT_ARTIFACT_ROOT)
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument(
        "--command",
        action="append",
        default=[],
        help="shell command to run after the EROFS-overlay checks (repeatable)",
    )
    parser.add_argument(
        "--docker-hello-world-binary",
        type=Path,
        help=(
            "freestanding arm64/Linux payload to import and execute in the "
            "offline Docker smoke"
        ),
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
    if not emulator.is_file() or not os.access(emulator, os.X_OK):
        raise SafetyError(f"emulator is not executable: {emulator}")
    if not rootfs.is_file():
        raise SafetyError(f"rootfs is not a regular file: {rootfs}")

    docker_hello_world = None
    if options.docker_hello_world_binary is not None:
        docker_hello_world = load_docker_hello_world(
            options.docker_hello_world_binary.resolve()
        )
    command = build_command(
        emulator,
        rootfs,
        options.root_device,
        options.memory,
        options.unit,
        options.kernel_arguments,
    )
    test_script = make_test_script(
        options.command,
        auto_poweroff=not options.no_auto_poweroff,
        docker_hello_world=docker_hello_world,
    )
    bootstrap = make_bootstrap(test_script)
    result = {
        "command": command,
        "rootfs": str(rootfs),
        "auto_poweroff": not options.no_auto_poweroff,
    }
    if options.docker_hello_world_binary is not None:
        result["docker_hello_world"] = str(
            options.docker_hello_world_binary.resolve()
        )
    if options.dry_run:
        print(shlex.join(command))
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
    write_json(artifact_directory / "run.json", result)
    if not saw_pass or saw_fail:
        raise RuntimeError("guest EROFS-overlay verification did not pass")
    return 0


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
