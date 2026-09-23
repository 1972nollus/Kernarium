"""Shared serial-console guest harness for integration and performance runners."""

from __future__ import annotations
# Imported support must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import dataclasses
import os
import pathlib
import pty
import re
import select
import shlex
import signal
import subprocess
import sys
import tempfile
import time
import uuid
from collections.abc import Sequence


ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
BOOT_TIMEOUT_SECONDS = 90
CONSOLE_WRITE_TIMEOUT_SECONDS = 30
SIZE_VALUE = re.compile(r"^(\d+)([KMGT]?)$", re.IGNORECASE)
SWAP_IMAGE = re.compile(r"^uml-ltp-swap-(\d+)-.*\.img$")
OVERLAY_READY_MARKER = "__UML_OVERLAY_READY__"
OVERLAY_SHELL_BOOTSTRAP = "\n".join(
    (
        "{",
        "set -eu",
        "mount -t tmpfs -o mode=0755,nosuid,nodev tmpfs /mnt",
        "mkdir -p /mnt/lower /mnt/upper /mnt/work /mnt/root",
        "mount -o bind / /mnt/lower",
        "mount -t overlay overlay "
        "-o lowerdir=/mnt/lower,upperdir=/mnt/upper,workdir=/mnt/work /mnt/root",
        "mount -t devtmpfs devtmpfs /mnt/root/dev",
        "mkdir -p /mnt/root/.uml-lower",
        "cd /mnt/root",
        "pivot_root . .uml-lower",
        # The kernel starts init without PATH. The shell searches its own
        # default, but programs that locate themselves through PATH (gcc)
        # need it in the environment.
        "export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
        "rm -f /etc/resolv.conf",
        "printf '%s\\n' 'nameserver 1.1.1.1' 'nameserver 8.8.8.8' "
        "'nameserver 10.0.2.3' > /etc/resolv.conf",
        "mount -t proc proc /proc",
        "mount -t sysfs sysfs /sys",
        "mkdir -p /run",
        "mount -t tmpfs -o mode=0755,nosuid,nodev tmpfs /run",
        "mount -t tmpfs -o mode=1777,nosuid,nodev tmpfs /tmp",
        # An init would provide these; POSIX shm, ptys and loopback sockets
        # otherwise fail with ENOENT or ENETUNREACH.
        "mkdir -p /dev/shm /dev/pts",
        "mount -t tmpfs -o mode=1777,nosuid,nodev tmpfs /dev/shm",
        "mount -t devpts -o gid=5,mode=620,ptmxmode=000 devpts /dev/pts",
        "mount -t securityfs securityfs /sys/kernel/security",
        # systemd makes every mount shared; unshare(1) relies on it.
        "mount --make-rshared /",
        "ip link set lo up",
        # The SLiRP provider's fixed lease; the kernel has no IP_PNP.
        "if ip link show eth0 >/dev/null 2>&1; then",
        "ip link set eth0 up",
        "ip addr add 10.0.2.15/24 dev eth0",
        "ip route add default via 10.0.2.2",
        "fi",
        'case " $(cat /proc/cmdline) " in',
        '*" uml_ltp_workspace=ext2 "*)',
        "workspace_image=/run/uml-ltp-workspace.ext2",
        'dd if=/dev/zero of="$workspace_image" bs=1M count=0 seek=512 2>/dev/null',
        "workspace_loop=$(losetup -f)",
        'losetup "$workspace_loop" "$workspace_image"',
        "if command -v mkfs.ext2 >/dev/null 2>&1; then",
        "workspace_mkfs=mkfs.ext2",
        "elif command -v mke2fs >/dev/null 2>&1; then",
        "workspace_mkfs=mke2fs",
        "else",
        "echo 'LTP ext2 workspace requires mkfs.ext2 or mke2fs' >&2",
        "exit 127",
        "fi",
        '"$workspace_mkfs" -q -t ext2 "$workspace_loop"',
        'mount -t ext2 -o noatime "$workspace_loop" /tmp',
        "chmod 1777 /tmp",
        ";;",
        "esac",
        "set +e",
        "set +u",
        ":",
        "}",
        f"printf '\\n{OVERLAY_READY_MARKER}\\n'",
    )
)
SYSTEMD_OVERLAY_BOOTSTRAP = "\n".join(
    (
        "{",
        "set -eu",
        "mount -t tmpfs -o mode=0755,nosuid,nodev tmpfs /mnt",
        "mkdir -p /mnt/lower /mnt/upper /mnt/work /mnt/root",
        "mount -o bind / /mnt/lower",
        "mount -t overlay overlay "
        "-o lowerdir=/mnt/lower,upperdir=/mnt/upper,workdir=/mnt/work /mnt/root",
        "mount -t devtmpfs devtmpfs /mnt/root/dev",
        "mkdir -p /mnt/root/.uml-lower",
        "cd /mnt/root",
        "pivot_root . .uml-lower",
        "rm -f /etc/resolv.conf",
        "printf '%s\\n' 'nameserver 1.1.1.1' 'nameserver 8.8.8.8' "
        "'nameserver 10.0.2.3' > /etc/resolv.conf",
        "mkdir -p /var/lib/docker",
        "mount -t tmpfs -o mode=0711,nosuid,nodev tmpfs /var/lib/docker",
        "mkdir -p /var/lib/containerd",
        "mount -t tmpfs -o mode=0711,nosuid,nodev tmpfs /var/lib/containerd",
        "if [ -x /lib/systemd/systemd ]; then exec /lib/systemd/systemd; fi",
        "exec /usr/lib/systemd/systemd",
        "}",
    )
)


@dataclasses.dataclass(frozen=True)
class CommandResult:
    return_code: int
    output: str


CORE_CLASSES = ("performance", "efficiency")
TASKPOLICY = "/usr/sbin/taskpolicy"


def core_class_prefix(core_class: str) -> list[str]:
    """Return the launcher prefix that selects a host core class.

    Apple's scheduler places a process-wide background QoS clamp on the
    efficiency cores, so ``taskpolicy -c background`` is how a measurement runs
    on the core type that dominates iOS.  The clamp is a task ceiling, so the
    emulator's vCPU threads inherit it even though they are created later.
    """
    if core_class not in CORE_CLASSES:
        raise ValueError(f"unknown core class {core_class!r}")
    if core_class == "performance":
        return []
    if sys.platform != "darwin" or not os.path.exists(TASKPOLICY):
        raise RuntimeError(
            "efficiency-core measurement requires Darwin's taskpolicy"
        )
    return [TASKPOLICY, "-c", "background"]


def emulator_arguments(
    emulator: pathlib.Path,
    rootfs: pathlib.Path,
    memory: str,
    workspace: str = "tmpfs",
    kernel_arguments: Sequence[str] = (),
    swap_path: pathlib.Path | None = None,
    init_path: str = "/sbin/init",
    engine: str = "jit",
    core_class: str = "performance",
    readonly_root: bool = False,
) -> list[str]:
    arguments = [
        *core_class_prefix(core_class),
        str(emulator),
        f"--execution-engine={engine}",
        f"ubd0{'rc' if readonly_root else 'c'}={rootfs}",
        "root=/dev/ubda",
        "rootfstype=erofs",
        f"init={init_path}",
        f"mem={memory}",
    ]
    if workspace == "ext2":
        arguments.insert(-1, "uml_ltp_workspace=ext2")
    if swap_path is not None:
        arguments.insert(-1, f"ubd1={swap_path}")
    arguments.extend(kernel_arguments)
    return arguments


def size_bytes(value: str) -> int:
    """Parse a memory or disk size using binary K/M/G/T suffixes."""
    match = SIZE_VALUE.fullmatch(value)
    if match is None:
        raise ValueError(f"invalid size {value!r}")
    suffix_shift = {"": 0, "K": 10, "M": 20, "G": 30, "T": 40}
    size = int(match.group(1)) << suffix_shift[match.group(2).upper()]
    if size < 4096:
        raise ValueError(f"size must be at least 4096 bytes: {value!r}")
    return size


def cleanup_stale_swap_images() -> None:
    """Remove sparse swap backings left by terminated harness processes."""
    for path in pathlib.Path(tempfile.gettempdir()).glob("uml-ltp-swap-*.img"):
        match = SWAP_IMAGE.fullmatch(path.name)
        if match is None:
            continue
        try:
            os.kill(int(match.group(1)), 0)
        except ProcessLookupError:
            path.unlink(missing_ok=True)
        except PermissionError:
            pass


def create_swap_image(value: str) -> pathlib.Path:
    """Create a per-guest sparse raw disk without allocating its data blocks."""
    descriptor, name = tempfile.mkstemp(
        prefix=f"uml-ltp-swap-{os.getpid()}-", suffix=".img"
    )
    try:
        os.ftruncate(descriptor, size_bytes(value))
    except BaseException:
        pathlib.Path(name).unlink(missing_ok=True)
        raise
    finally:
        os.close(descriptor)
    return pathlib.Path(name)


def marker_pattern(marker: str) -> re.Pattern[bytes]:
    """Match protocol markers even if a tty ioctl enables case conversion."""
    return re.compile(
        re.escape(marker.encode()) + rb":(\d+)\r?\n",
        re.IGNORECASE,
    )


# A console prompt does not always begin a line with CRLF. When bash execs a
# replacement shell it first emits its bracketed-paste reset, and the terminal
# then returns the carriage without a line feed, so the sequence reaching the
# transcript is `\r\n<ESC>[?2004l\r# `. Anchoring on `\r?\n` alone cannot match
# that prompt at all, because `[^\r\n]*` may not cross the bare carriage
# return, so `exec sudo -n /bin/sh` timed out with the prompt already on screen.
# Accept a lone `\r` as a line start as well.
LINE_START = rb"(?:^|\r?\n|\r)"


class Guest:
    """One emulator process controlled through its serial console."""

    def __init__(
        self,
        emulator: pathlib.Path,
        rootfs: pathlib.Path,
        memory: str,
        workspace: str,
        kernel_arguments: Sequence[str] = (),
        swap_size: str | None = None,
        cgroup1: str | None = None,
        cgroup2_nsdelegate: bool = False,
        init_shell: bool = False,
        overlay_shell: bool = False,
        systemd_overlay: bool = False,
        engine: str = "jit",
        core_class: str = "performance",
        systemd_overlay_size: str | None = None,
        systemd_image_init: str | None = None,
    ) -> None:
        if sum((init_shell, overlay_shell, systemd_overlay, bool(systemd_image_init))) > 1:
            raise ValueError("guest boot modes are mutually exclusive")
        if systemd_overlay_size and not systemd_overlay:
            raise ValueError("a disk overlay requires systemd overlay boot")
        self._swap_path = None
        self._overlay_path = None
        if systemd_overlay_size:
            descriptor, name = tempfile.mkstemp(prefix="uml-systemd-overlay-", suffix=".ext4")
            self._overlay_path = pathlib.Path(name)
            try:
                os.ftruncate(descriptor, size_bytes(systemd_overlay_size))
            except BaseException:
                self._overlay_path.unlink(missing_ok=True)
                raise
            finally:
                os.close(descriptor)
            kernel_arguments = (*kernel_arguments, f"ubd2={self._overlay_path}")
        self._core_class = core_class
        try:
            self._swap_path = create_swap_image(swap_size) if swap_size else None
        except BaseException:
            self._remove_swap_image()
            raise
        self._cgroup1 = cgroup1
        self._cgroup2_nsdelegate = cgroup2_nsdelegate
        self._init_shell = init_shell
        self._overlay_shell = overlay_shell
        self._systemd_overlay = systemd_overlay
        self._systemd_image_init = systemd_image_init
        if init_shell or overlay_shell or systemd_overlay:
            kernel_arguments = (
                *kernel_arguments,
                "console=tty0",
                "console=ttyS0",
                "ip=dhcp",
            )
        if systemd_overlay:
            kernel_arguments = (*kernel_arguments, "systemd.unit=multi-user.target")
        if systemd_image_init:
            kernel_arguments = (*kernel_arguments, "console=ttyS0", "ip=off", "quiet", "loglevel=3",
                                "systemd.show_status=false", "systemd.unit=multi-user.target")
        try:
            master, slave = pty.openpty()
        except BaseException:
            self._remove_swap_image()
            raise
        self._master = master
        os.set_blocking(master, False)
        try:
            self._process = subprocess.Popen(
                emulator_arguments(
                    emulator,
                    rootfs,
                    memory,
                    workspace,
                    kernel_arguments,
                    self._swap_path,
                    "/bin/sh"
                    if init_shell or overlay_shell or systemd_overlay
                    else systemd_image_init or "/sbin/init",
                    engine,
                    core_class,
                    bool(systemd_image_init),
                ),
                # Darwin can miss PTY readiness while all guest CPUs are
                # busy. A pipe keeps input pollable at guest timer boundaries;
                # retain the PTY for terminal-shaped, unbuffered output.
                stdin=subprocess.PIPE,
                stdout=slave,
                stderr=slave,
                close_fds=True,
            )
        except BaseException:
            os.close(master)
            self._remove_swap_image()
            raise
        finally:
            os.close(slave)
        assert self._process.stdin is not None
        self._input = self._process.stdin
        os.set_blocking(self._input.fileno(), False)
        self._transcript = bytearray()

    @property
    def process_id(self) -> int:
        return self._process.pid

    @property
    def return_code(self) -> int | None:
        """Return the emulator status once it exits, otherwise ``None``."""
        return self._process.poll()

    def terminate(self) -> None:
        """Force a guest worker to leave a blocking console operation."""
        self._signal(signal.SIGKILL)

    def _signal(self, guest_signal: int) -> None:
        """Signal the emulator, which shares this harness's process group.

        The emulator is started attached rather than in a session of its own, so
        whatever kills the harness's group takes the guest with it. That is the
        property worth having: a harness killed outright runs no cleanup code,
        and a guest that survives one boots to idle and still burns one to two
        host cores indefinitely, quietly oversubscribing every later benchmark
        on the same host.
        """
        if self._process.poll() is None:
            try:
                self._process.send_signal(guest_signal)
            except ProcessLookupError:
                pass

    def boot_to_shell(self, timeout_seconds: int = BOOT_TIMEOUT_SECONDS) -> None:
        """Reach a root shell supplied by an explicit init-shell boot mode."""
        if not (self._init_shell or self._overlay_shell):
            raise RuntimeError("Guest was not constructed with an init-shell mode")
        self._read_until(re.compile(LINE_START + rb"# $"), timeout_seconds)
        if self._overlay_shell:
            search_from = len(self._transcript)
            self._write(OVERLAY_SHELL_BOOTSTRAP + "\n")
            self._read_until(
                re.compile(
                    LINE_START
                    + re.escape(OVERLAY_READY_MARKER.encode())
                    + rb"\r?\n"
                ),
                timeout_seconds,
                search_from,
            )

    def boot_and_login(self, timeout_seconds: int = BOOT_TIMEOUT_SECONDS) -> None:
        if self._systemd_overlay or self._systemd_image_init:
            if self._systemd_overlay:
                self._read_until(re.compile(LINE_START + rb"# $"), timeout_seconds)
                bootstrap = SYSTEMD_OVERLAY_BOOTSTRAP
                if self._overlay_path is not None:
                    bootstrap = bootstrap.replace(
                        "mount -t tmpfs -o mode=0755,nosuid,nodev tmpfs /mnt",
                        "mkfs.ext4 -q -E lazy_itable_init=1,lazy_journal_init=1 /dev/ubdc\n"
                        "mount -t ext4 -o noatime,nosuid,nodev /dev/ubdc /mnt",
                    )
                self._write(bootstrap + "\n")
            login = self._read_until(
                re.compile(rb" login:\s*uml \(automatic login\)", re.MULTILINE),
                timeout_seconds,
            )
            self._read_until(
                re.compile(LINE_START + rb"[^\r\n]*\$ $"),
                timeout_seconds,
                login.end(),
            )
            sudo = self.run_command(
                'test "$(id -un)" = uml && sudo -n true', timeout_seconds
            )
            if sudo.return_code != 0:
                raise RuntimeError(
                    "image-owned uml login lacks noninteractive sudo: "
                    + sudo.output
                )
            search_from = len(self._transcript)
            self._write("exec sudo -n /bin/sh\n")
            self._read_until(
                re.compile(LINE_START + rb"[^\r\n]*# $"),
                timeout_seconds,
                search_from,
            )
            if self._overlay_path is not None:
                # The emulator has opened the disk before mounting it. Keep
                # its sparse backing only through that open descriptor, so
                # even an abrupt emulator exit releases all allocated blocks.
                self._overlay_path.unlink(missing_ok=True)
        elif self._init_shell or self._overlay_shell:
            self.boot_to_shell(timeout_seconds)
        else:
            self._read_until(
                re.compile(rb" login:\s*$", re.MULTILINE), timeout_seconds
            )
            self._write("root\n")
            self._read_until(re.compile(LINE_START + rb"# $"), timeout_seconds)
        if self._swap_path is not None:
            result = self.run_command(
                "mkswap /dev/ubdb >/dev/null && swapon /dev/ubdb",
                timeout_seconds,
            )
            if result.return_code != 0:
                raise RuntimeError(
                    "could not enable disposable guest swap: " + result.output
                )
        if self._cgroup2_nsdelegate:
            result = self.run_command(
                "mkdir -p /sys/fs/cgroup && "
                "mount -t cgroup2 -o nsdelegate cgroup2 /sys/fs/cgroup",
                timeout_seconds,
            )
            if result.return_code != 0:
                raise RuntimeError(
                    "could not mount disposable cgroup v2 nsdelegate hierarchy: "
                    + result.output
                )
        elif self._cgroup1 is not None:
            mountpoint = f"/sys/fs/cgroup/{self._cgroup1}"
            command = (
                "mount -t tmpfs -o mode=755 cgroup-root /sys/fs/cgroup && "
                f"mkdir -p {shlex.quote(mountpoint)} && "
                f"mount -t cgroup -o {shlex.quote(self._cgroup1)} "
                f"{shlex.quote(self._cgroup1)} {shlex.quote(mountpoint)}"
            )
            result = self.run_command(command, timeout_seconds)
            if result.return_code != 0:
                raise RuntimeError(
                    "could not mount disposable cgroup v1 hierarchy: "
                    + result.output
                )

    def run_command(
        self, command: str, timeout_seconds: int = 30
    ) -> CommandResult:
        """Run a shell command through the serial console and capture it."""
        token = uuid.uuid4().hex
        begin = f"__UML_COMMAND_BEGIN_{token}__"
        end = f"__UML_COMMAND_END_{token}__"
        wrapped = (
            f"printf '\\n{begin}\\n'; {command}; rc=$?; "
            f"printf '\\n{end}:%s\\n' \"$rc\"\n"
        )
        start = len(self._transcript)
        self._write(wrapped)
        match = self._read_until(marker_pattern(end), timeout_seconds, start)
        section = bytes(self._transcript[start : match.start()])
        begin_at = section.rfind(begin.encode())
        if begin_at >= 0:
            section = section[begin_at + len(begin) :]
        return CommandResult(int(match.group(1)), self._normal_output(section))

    def shutdown(self, timeout_seconds: int = 30) -> bool:
        """Request an orderly halt and wait for the emulator to exit."""
        if self._process.poll() is not None:
            return True
        try:
            self._write("sync; poweroff -f\n")
            deadline = time.monotonic() + timeout_seconds
            while self._process.poll() is None:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return False
                readable, _, _ = select.select(
                    [self._master], [], [], min(remaining, 0.25)
                )
                if readable:
                    self._drain()
            self._drain()
        except (BrokenPipeError, OSError):
            return self._process.poll() is not None
        return True

    def close(self) -> None:
        try:
            self.shutdown(5)
            if self._process.poll() is None:
                try:
                    self._signal(signal.SIGTERM)
                    self._process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    pass
            if self._process.poll() is None:
                self._signal(signal.SIGKILL)
                self._process.wait()
        except KeyboardInterrupt:
            if self._process.poll() is None:
                self._signal(signal.SIGKILL)
            try:
                self._process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                pass
            raise
        finally:
            try:
                self._input.close()
            finally:
                try:
                    os.close(self._master)
                finally:
                    self._remove_swap_image()

    def _remove_swap_image(self) -> None:
        if self._overlay_path is not None:
            self._overlay_path.unlink(missing_ok=True)
            self._overlay_path = None
        if self._swap_path is not None:
            self._swap_path.unlink(missing_ok=True)
            self._swap_path = None

    def output(self) -> str:
        return self._normal_output(bytes(self._transcript))

    def _write(self, text: str) -> None:
        payload = memoryview(text.encode())
        input_descriptor = self._input.fileno()
        offset = 0
        deadline = time.monotonic() + CONSOLE_WRITE_TIMEOUT_SECONDS
        while offset < len(payload):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("guest console write exceeded 30s")
            try:
                written = os.write(input_descriptor, payload[offset:])
            except InterruptedError:
                continue
            except BlockingIOError:
                written = 0
            if written > 0:
                offset += written
                continue
            if self._process.poll() is not None:
                raise BrokenPipeError("guest exited during console write")
            readable, writable, _ = select.select(
                [self._master], [input_descriptor], [], min(remaining, 0.25)
            )
            if readable:
                self._drain()
            if not writable:
                continue

    def _read_until(
        self,
        pattern: re.Pattern[bytes],
        timeout_seconds: int,
        search_from: int = 0,
    ) -> re.Match[bytes]:
        deadline = time.monotonic() + timeout_seconds
        while True:
            match = pattern.search(self._transcript, search_from)
            if match is not None:
                return match
            if self._process.poll() is not None:
                self._drain()
                match = pattern.search(self._transcript, search_from)
                if match is not None:
                    return match
                raise RuntimeError(
                    f"guest exited with status {self._process.returncode}"
                )
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"guest operation exceeded {timeout_seconds}s")
            readable, _, _ = select.select(
                [self._master], [], [], min(remaining, 0.25)
            )
            if readable:
                self._drain()

    def _drain(self) -> None:
        while True:
            try:
                chunk = os.read(self._master, 65536)
            except (BlockingIOError, OSError):
                return
            if not chunk:
                return
            self._transcript.extend(chunk)

    @staticmethod
    def _normal_output(output: bytes) -> str:
        decoded = output.decode("utf-8", errors="replace").replace("\r", "")
        return ANSI_ESCAPE.sub("", decoded).strip()
