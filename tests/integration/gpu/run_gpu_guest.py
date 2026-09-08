#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot the GPU test guest from a read-only lower filesystem plus tmpfs."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import base64
import io
import json
import os
from pathlib import Path
import pty
import select
import shlex
import signal
import socket
import stat
import subprocess
import sys
import tarfile
import time
import tty
from typing import Any, BinaryIO, Sequence

import guest_process


REPO_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_EMULATOR = REPO_ROOT / ".build/presets/macos-integration/bin/uml-darwin"
DEFAULT_ROOTFS = REPO_ROOT / "rootfs.erofs"
DEFAULT_ARTIFACT_ROOT = REPO_ROOT / ".build/gpu-test-artifacts"
PASS_MARKER = "UML_GPU_OVERLAY_TEST: PASS"
FAIL_MARKER = "UML_GPU_OVERLAY_TEST: FAIL"
DOCKER_HELLO_WORLD_IMAGE = "uml/hello-world:local"
MAX_DOCKER_HELLO_WORLD_SIZE = 1024 * 1024
MAX_SERIAL_CLIENT_BACKLOG = 1024 * 1024
MAX_SERIAL_INPUT_BACKLOG = 1024 * 1024
SERIAL_WRITE_CHUNK_BYTES = 128
SERIAL_WRITE_INTERVAL_SECONDS = 0.01
BOOTSTRAP_PRIMARY_PROMPT = b"UML_BOOTSTRAP_READY> "
BOOTSTRAP_CONTINUATION_PROMPT = b"UML_BOOTSTRAP_CONTINUE> "
BOOTSTRAP_PROMPT_COMMAND = (
    b"PS1='UML_BOOTSTRAP_READY> '; "
    b"PS2='UML_BOOTSTRAP_CONTINUE> '\n"
)
PROTECTED_ARGUMENTS = (
    "ubd0",
    "root=",
    "rootfstype=",
    "init=",
    "mem=",
    "console=",
    "systemd.unit=",
    "--virtio-console-socket=",
    "--virtio-serial-socket=",
)
ROOTFS_TYPES = ("erofs", "ext4")


class SafetyError(RuntimeError):
    """Raised when a boot could risk modifying the base image."""


class SerialConsoleSocket:
    """One reconnectable Unix client for the runner-owned serial stream."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.client: socket.socket | None = None
        self.output = bytearray()
        self._identity: tuple[int, int] | None = None
        if path.exists() or path.is_symlink():
            self.listener.close()
            raise SafetyError(f"serial socket path already exists: {path}")
        if not path.parent.is_dir():
            self.listener.close()
            raise SafetyError(
                f"serial socket parent is not a directory: {path.parent}"
            )
        try:
            self.listener.bind(str(path))
            details = path.lstat()
            self._identity = (details.st_dev, details.st_ino)
            path.chmod(0o600)
            self.listener.listen(1)
            self.listener.setblocking(False)
        except OSError as error:
            self.close()
            raise SafetyError(f"cannot create serial socket {path}: {error}") from error

    def close_client(self) -> None:
        if self.client is not None:
            self.client.close()
            self.client = None
        self.output.clear()

    def accept(self, history: bytes) -> None:
        try:
            client, _ = self.listener.accept()
        except BlockingIOError:
            return
        client.setblocking(False)
        self.close_client()
        self.client = client
        self.output.extend(history)

    def read_input(self) -> bytes:
        if self.client is None:
            return b""
        try:
            data = self.client.recv(65536)
        except BlockingIOError:
            return b""
        except OSError:
            self.close_client()
            return b""
        if not data:
            self.close_client()
            return b""
        return data

    def queue_output(self, data: bytes) -> None:
        if self.client is None:
            return
        if len(self.output) + len(data) > MAX_SERIAL_CLIENT_BACKLOG:
            self.close_client()
            return
        self.output.extend(data)

    def flush_output(self) -> None:
        if self.client is None or not self.output:
            return
        try:
            sent = self.client.send(self.output)
        except BlockingIOError:
            return
        except OSError:
            self.close_client()
            return
        if sent > 0:
            del self.output[:sent]

    def close(self) -> None:
        self.close_client()
        self.listener.close()
        if self._identity is None:
            return
        try:
            details = self.path.lstat()
        except FileNotFoundError:
            return
        if (
            stat.S_ISSOCK(details.st_mode)
            and (details.st_dev, details.st_ino) == self._identity
        ):
            self.path.unlink()

    def __enter__(self) -> SerialConsoleSocket:
        return self

    def __exit__(self, *_: object) -> None:
        self.close()


def validate_extra_arguments(arguments: Sequence[str]) -> None:
    for argument in arguments:
        if any(argument.startswith(prefix) for prefix in PROTECTED_ARGUMENTS):
            raise SafetyError(f"kernel argument is controlled by the harness: {argument}")


def validate_rootfs_path(path: Path, rootfs_type: str) -> None:
    if rootfs_type not in ROOTFS_TYPES:
        raise SafetyError(f"unsupported root filesystem type: {rootfs_type}")
    if not path.is_file():
        raise SafetyError(f"rootfs is not a regular file: {path}")
    if rootfs_type == "ext4" and path.stat().st_mode & (
        stat.S_IWUSR | stat.S_IWGRP | stat.S_IWOTH
    ):
        raise SafetyError(
            "derivative ext4 rootfs still has host write permission"
        )


def validate_console_socket_paths(
    serial_socket: Path | None,
    virtio_console_socket: Path | None,
) -> None:
    if (
        serial_socket is not None
        and virtio_console_socket is not None
        and serial_socket == virtio_console_socket
    ):
        raise SafetyError("serial and Virtio console sockets must be different")


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
    rootfs_type: str = "erofs",
    virtio_console_socket: Path | None = None,
) -> list[str]:
    validate_extra_arguments(extra_arguments)
    if rootfs_type not in ROOTFS_TYPES:
        raise SafetyError(f"unsupported root filesystem type: {rootfs_type}")
    ubd_flags = "c" if rootfs_type == "erofs" else "cr"
    return [
        str(emulator.resolve()),
        f"ubd0{ubd_flags}={rootfs.resolve()}",
        f"root={root_device}",
        f"rootfstype={rootfs_type}",
        "init=/bin/sh",
        f"mem={memory}",
        "console=tty0",
        "console=ttyS0",
        "ip=dhcp",
        f"systemd.unit={systemd_unit}",
        *extra_arguments,
        *(
            [f"--virtio-console-socket={virtio_console_socket.resolve()}"]
            if virtio_console_socket is not None
            else []
        ),
    ]


def make_test_script(
    commands: Sequence[str],
    auto_poweroff: bool,
    docker_hello_world: bytes | None = None,
    lower_filesystem_type: str = "erofs",
    root_device: str = "/dev/ubda",
) -> str:
    if lower_filesystem_type not in ROOTFS_TYPES:
        raise SafetyError(
            f"unsupported root filesystem type: {lower_filesystem_type}"
        )
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
    block_read_only_check = ""
    if lower_filesystem_type == "ext4":
        block_read_only_check = (
            f'check_equal "$(blockdev --getro {shlex.quote(root_device)})" '
            '1 "lower block-device read-only state"\n'
        )
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
check_equal "$(findmnt -n -o FSTYPE -T /.uml-lower)" {lower_filesystem_type} "lower filesystem type"
case ",$(findmnt -n -o OPTIONS -T /.uml-lower)," in
    *,ro,*) ;;
    *) echo "overlay check failed: lower filesystem is not read-only"; status=1 ;;
esac
{block_read_only_check}check_equal "$(findmnt -n -o FSTYPE -T /.uml-lower/mnt)" tmpfs "upper backing type"
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
    # malformed test input) inside a subshell so the overlay-root result and
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


def make_bootstrap(
    test_script: str,
    lower_filesystem_type: str = "erofs",
) -> bytes:
    if lower_filesystem_type not in ROOTFS_TYPES:
        raise SafetyError(
            f"unsupported root filesystem type: {lower_filesystem_type}"
        )
    encoded_test = base64.b64encode(test_script.encode("utf-8")).decode("ascii")
    # The bootstrap is typed into the UML init shell through its tty. Keep each
    # physical input line comfortably below the tty's canonical-input limit so
    # large diagnostic commands cannot be truncated before systemd starts.
    encoded_arguments = " \\\n".join(
        f"    '{encoded_test[offset : offset + 512]}'"
        for offset in range(0, len(encoded_test), 512)
    )
    bootstrap = f"""stty -echo
set -eu
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
Description=Darwin UML read-only-root GPU test guard
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
echo "Darwin UML root overlay: lower={lower_filesystem_type}, upper=tmpfs"
exec /lib/systemd/systemd
"""
    return bootstrap.encode("utf-8")


def terminate_process(process: subprocess.Popen[bytes]) -> None:
    guest_process.stop_process(process, guest_process.EMULATOR_STOP)


def write_pending(
    stream: BinaryIO,
    pending: bytearray,
    maximum_bytes: int | None = None,
) -> None:
    """Drain a bounded amount of queued input into a nonblocking tty."""
    remaining = maximum_bytes
    while pending and (remaining is None or remaining > 0):
        payload = pending if remaining is None else pending[:remaining]
        try:
            written = stream.write(payload)
        except InterruptedError:
            continue
        except BlockingIOError:
            return
        if written is None:
            return
        if written <= 0:
            raise RuntimeError("guest console stopped accepting input")
        del pending[:written]
        if remaining is not None:
            remaining -= written


def run_guest(
    command: Sequence[str],
    bootstrap: bytes,
    console_log: Path,
    timeout: float,
    expect_poweroff: bool,
    serial_socket: Path | None = None,
) -> tuple[int, bool, bool]:
    artifact_socket = guest_process.SocketOwnership(command)
    serial = (
        SerialConsoleSocket(serial_socket) if serial_socket is not None else None
    )
    try:
        master, slave = pty.openpty()
    except BaseException:
        if serial is not None:
            serial.close()
        raise
    try:
        tty.setraw(slave)
    except BaseException:
        os.close(master)
        os.close(slave)
        if serial is not None:
            serial.close()
        raise
    console = None

    def stop_emulator(process: subprocess.Popen[bytes]) -> None:
        try:
            artifact_socket.observe()
        finally:
            terminate_process(process)

    def close_resources() -> None:
        try:
            if serial is not None:
                serial.close()
        finally:
            try:
                if console is not None:
                    console.close()
                else:
                    os.close(master)
            finally:
                try:
                    if slave is not None:
                        os.close(slave)
                finally:
                    artifact_socket.close()

    with guest_process.managed_child(
        command, stop=stop_emulator, cleanup=close_resources,
        journal_setting=os.environ.get(guest_process.OWNERSHIP_ENV),
        stdin=slave, stdout=slave, stderr=slave, bufsize=0, close_fds=True,
    ) as process:
        os.close(slave)
        slave = None
        console = os.fdopen(master, "r+b", buffering=0)
        os.set_blocking(console.fileno(), False)
        saw_pass = False
        saw_fail = False
        deadline = time.monotonic() + timeout if timeout > 0 else None
        pending = bytearray()
        guest_input = bytearray()
        deferred_input = bytearray()
        bootstrap_payload = bootstrap if bootstrap.endswith(b"\n") else bootstrap + b"\n"
        bootstrap_lines = bootstrap_payload.splitlines(keepends=True)
        bootstrap_line = 0
        bootstrap_stage = "initial-prompt"
        bootstrap_prompt_output = bytearray()
        next_guest_write = 0.0
        if serial is not None:
            print(f"Serial console socket: {serial.path}", file=sys.stderr)
        with console_log.open("wb") as log:
            while process.poll() is None:
                artifact_socket.observe()
                if deadline is not None and time.monotonic() >= deadline:
                    raise TimeoutError(f"guest did not finish within {timeout:.0f}s")
                now = time.monotonic()
                guest_write_ready = bool(guest_input) and now >= next_guest_write
                read_descriptors: list[Any] = [console]
                write_descriptors: list[Any] = (
                    [console] if guest_write_ready else []
                )
                if serial is not None:
                    read_descriptors.append(serial.listener)
                    if serial.client is not None:
                        read_descriptors.append(serial.client)
                        if serial.output:
                            write_descriptors.append(serial.client)
                select_timeout = 0.25
                if guest_input and not guest_write_ready:
                    select_timeout = min(
                        select_timeout,
                        max(0.0, next_guest_write - now),
                    )
                readable, writable, _ = select.select(
                    read_descriptors, write_descriptors, [], select_timeout
                )
                if serial is not None and serial.listener in readable:
                    serial.accept(bytes(pending))
                if (
                    serial is not None
                    and serial.client is not None
                    and serial.client in readable
                ):
                    data = serial.read_input()
                    if data:
                        input_queue = (
                            guest_input
                            if bootstrap_stage == "complete"
                            else deferred_input
                        )
                        if len(input_queue) + len(data) > MAX_SERIAL_INPUT_BACKLOG:
                            serial.close_client()
                        else:
                            input_queue.extend(data)
                if (
                    serial is not None
                    and serial.client is not None
                    and serial.client in writable
                ):
                    serial.flush_output()
                if console in writable:
                    write_pending(
                        console,
                        guest_input,
                        maximum_bytes=SERIAL_WRITE_CHUNK_BYTES,
                    )
                    next_guest_write = (
                        time.monotonic() + SERIAL_WRITE_INTERVAL_SECONDS
                    )
                if console not in readable:
                    continue
                try:
                    data = os.read(console.fileno(), 65536)
                except OSError:
                    if process.poll() is not None:
                        break
                    raise
                if not data:
                    break
                log.write(data)
                log.flush()
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
                pending.extend(data)
                if len(pending) > 8192:
                    del pending[:-8192]
                bootstrap_prompt_output.extend(data.replace(b"\r", b""))
                if len(bootstrap_prompt_output) > 8192:
                    del bootstrap_prompt_output[:-8192]
                if (
                    bootstrap_stage == "initial-prompt"
                    and b"\n# " in bootstrap_prompt_output
                ):
                    # The init shell has echo enabled. Disable it before installing
                    # unique prompts so the prompt strings in the setup command
                    # cannot be mistaken for acknowledgements.
                    guest_input.extend(b"stty -echo\n")
                    bootstrap_stage = "echo-off"
                    bootstrap_prompt_output.clear()
                elif (
                    bootstrap_stage == "echo-off"
                    and b"\n# " in bootstrap_prompt_output
                ):
                    guest_input.extend(BOOTSTRAP_PROMPT_COMMAND)
                    bootstrap_stage = "line-prompt"
                    bootstrap_prompt_output.clear()
                elif bootstrap_stage == "line-prompt" and (
                    BOOTSTRAP_PRIMARY_PROMPT in bootstrap_prompt_output
                    or BOOTSTRAP_CONTINUATION_PROMPT in bootstrap_prompt_output
                ):
                    if bootstrap_line >= len(bootstrap_lines):
                        raise RuntimeError(
                            "guest requested input after the complete bootstrap"
                        )
                    guest_input.extend(bootstrap_lines[bootstrap_line])
                    bootstrap_line += 1
                    bootstrap_prompt_output.clear()
                    if bootstrap_line == len(bootstrap_lines):
                        bootstrap_stage = "complete"
                        guest_input.extend(deferred_input)
                        deferred_input.clear()
                decoded = pending.decode("utf-8", errors="replace")
                saw_pass = saw_pass or PASS_MARKER in decoded
                saw_fail = saw_fail or FAIL_MARKER in decoded
                if serial is not None:
                    serial.queue_output(data)
            return_code = process.wait(timeout=5)


    if return_code != 0:
        raise RuntimeError(f"emulator exited with status {return_code}")
    return return_code, saw_pass, saw_fail


def write_json(path: Path, value: Any) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def parse_arguments(arguments: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Boot a tmpfs-overlay GPU guest from a read-only lower image"
    )
    parser.add_argument("--emulator", type=Path, default=DEFAULT_EMULATOR)
    parser.add_argument("--rootfs", type=Path, default=DEFAULT_ROOTFS)
    parser.add_argument(
        "--rootfs-type",
        choices=ROOTFS_TYPES,
        default="erofs",
        help=(
            "lower filesystem type; ext4 images are attached block-read-only "
            "and without the host-file lock"
        ),
    )
    parser.add_argument("--root-device", default="/dev/ubda")
    parser.add_argument("--memory", default="8G")
    parser.add_argument("--unit", default="multi-user.target")
    parser.add_argument("--artifact-root", type=Path, default=DEFAULT_ARTIFACT_ROOT)
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument(
        "--command",
        action="append",
        default=[],
        help="shell command to run after the root-overlay checks (repeatable)",
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
    parser.add_argument(
        "--serial-socket",
        type=Path,
        help=(
            "expose the bidirectional serial console at this Unix-domain "
            "socket while retaining the normal log/stdout copy"
        ),
    )
    parser.add_argument(
        "--virtio-console-socket",
        type=Path,
        help=(
            "attach a Virtio console and expose guest hvc0 at this "
            "reconnectable Unix-domain socket"
        ),
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
    validate_rootfs_path(rootfs, options.rootfs_type)

    serial_socket = (
        options.serial_socket.resolve()
        if options.serial_socket is not None
        else None
    )
    virtio_console_socket = (
        options.virtio_console_socket.resolve()
        if options.virtio_console_socket is not None
        else None
    )
    validate_console_socket_paths(serial_socket, virtio_console_socket)

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
        rootfs_type=options.rootfs_type,
        virtio_console_socket=virtio_console_socket,
    )
    test_script = make_test_script(
        options.command,
        auto_poweroff=not options.no_auto_poweroff,
        docker_hello_world=docker_hello_world,
        lower_filesystem_type=options.rootfs_type,
        root_device=options.root_device,
    )
    bootstrap = make_bootstrap(test_script, options.rootfs_type)
    result = {
        "command": command,
        "rootfs": str(rootfs),
        "rootfs_type": options.rootfs_type,
        "auto_poweroff": not options.no_auto_poweroff,
    }
    if serial_socket is not None:
        result["serial_socket"] = str(serial_socket)
    if virtio_console_socket is not None:
        result["virtio_console_socket"] = str(virtio_console_socket)
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
        serial_socket=serial_socket,
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
        raise RuntimeError("guest root-overlay verification did not pass")
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
