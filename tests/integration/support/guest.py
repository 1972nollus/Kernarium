"""Shared serial-console guest harness for integration and performance runners."""

from __future__ import annotations
# Imported support must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import dataclasses
import functools
import os
import pathlib
import pty
import re
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import uuid
from collections.abc import Mapping, Sequence
from typing import BinaryIO


ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
BOOT_TIMEOUT_SECONDS = 90
CONSOLE_WRITE_TIMEOUT_SECONDS = 30
# A failing session prints this much of its transcript's end.
FAILURE_TRANSCRIPT_CHARACTERS = 16384
SIZE_VALUE = re.compile(r"^(\d+)([KMGT]?)$", re.IGNORECASE)
SWAP_IMAGE = re.compile(r"^uml-ltp-swap-(\d+)-.*\.img$")
STAGING_PREFIX = "uml-guest-files-"
STAGING_DIRECTORY = re.compile(rf"^{STAGING_PREFIX}(\d+)-.+$")
REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
# The launcher boots every guest through its base archive and a per-run
# initramfs (tools/rootfs/boot-ramdisk/README.md); this session prepares the
# shell the harness drives.
SESSION_SCRIPT = REPOSITORY / "tools" / "rootfs" / "boot-ramdisk" / "session"
SESSION_GUEST_PATH = "/usr/local/libexec/uml-session"
SESSION_PARAMETERS_GUEST_PATH = "/etc/uml-session.env"
SESSION_READY_MARKER = "UML_SESSION_READY:"
SWAP_DEVICE = "/dev/ubdb"
# The session mounts fresh filesystems over these, and the kernel owns the
# rest, so a payload placed there would never be seen.
HIDDEN_PAYLOAD_ROOTS = ("/tmp", "/run", "/dev", "/proc", "/sys")
# The directories an emulator makes for its run, named after its process: the
# guest console's and the launcher's initrd scratch.
EMULATOR_RUN_DIRECTORIES = (
    re.compile(r"^uml-kern(\d+)$"),
    re.compile(r"^uml-guest-initrd-(\d+)-.+$"),
)


@dataclasses.dataclass(frozen=True)
class CommandResult:
    return_code: int
    output: str


class CommandTimeout(TimeoutError):
    """A command's end marker did not arrive in time.

    ``output`` holds what the command printed before the deadline."""

    def __init__(self, message: str, output: str) -> None:
        super().__init__(message)
        self.output = output


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
    kernel_arguments: Sequence[str] = (),
    swap_path: pathlib.Path | None = None,
    init_path: str | None = None,
    engine: str = "jit",
    core_class: str = "performance",
    guest_files: Sequence[tuple[str, pathlib.Path]] = (),
) -> list[str]:
    """The launcher command for one guest.

    ``guest_files`` become the per-run initramfs the launcher builds, whose
    /init mounts the image. A guest that boots straight into an init on its
    EROFS image names it with ``init_path`` instead, and the launcher then
    leaves the boot to the kernel."""
    if init_path is not None and guest_files:
        raise ValueError("a guest boots either its own init or guest files")
    direct_boot = (
        ("root=/dev/ubda", "rootfstype=erofs", f"init={init_path}")
        if init_path is not None
        else ()
    )
    arguments = [
        *core_class_prefix(core_class),
        str(emulator),
        f"--execution-engine={engine}",
        *(f"--guest-file={guest}={host}" for guest, host in guest_files),
        f"ubd0c={rootfs}",
        *direct_boot,
        f"mem={memory}",
    ]
    if swap_path is not None:
        arguments.insert(-1, f"ubd1={swap_path}")
    arguments.extend(kernel_arguments)
    return arguments


@dataclasses.dataclass(frozen=True)
class GuestFile:
    """A file the per-run initramfs installs in the guest before it starts."""

    contents: bytes
    mode: int = 0o644


def validate_payload_path(path: str) -> None:
    """Payload paths are absolute, clean and outside what the session hides."""
    parts = path.split("/")
    if (
        not path.startswith("/")
        or any(part in ("", ".", "..") for part in parts[1:])
        or "=" in path
    ):
        raise ValueError(f"invalid guest payload path {path!r}")
    for hidden in HIDDEN_PAYLOAD_ROOTS:
        if path == hidden or path.startswith(hidden + "/"):
            raise ValueError(
                f"guest payload {path!r} would be hidden by {hidden}"
            )


def session_parameters(
    *,
    overlay_shell: bool,
    workspace: str,
    swap: bool,
    cgroup1: str | None,
    cgroup2_nsdelegate: bool,
    network: str = "user-network",
) -> str:
    """The session file for a harness shell; see tools/rootfs/boot-ramdisk.

    ``network`` is how an overlay shell gets its address and resolver: the
    SLiRP lease by default, or whatever the image configures itself, which
    for a guest with no network is nothing."""
    if network not in ("user-network", "image"):
        raise ValueError(f"unknown network {network!r}")
    if workspace not in ("tmpfs", "ext2"):
        raise ValueError(f"unknown workspace {workspace!r}")
    if workspace != "tmpfs" and not overlay_shell:
        raise ValueError("a workspace needs the overlay shell environment")
    lines = ["UML_SESSION_ENTRY=/bin/sh"]
    if overlay_shell:
        lines += [
            f"UML_SESSION_RESOLVER={network}",
            "UML_SESSION_RUNTIME_MOUNTS=1",
            f"UML_SESSION_NETWORK={network}",
            f"UML_SESSION_WORKSPACE={workspace}",
        ]
    if swap:
        lines.append(f"UML_SESSION_SWAP={SWAP_DEVICE}")
    if cgroup2_nsdelegate:
        lines.append("UML_SESSION_CGROUP=v2-nsdelegate")
    elif cgroup1 is not None:
        lines.append(f"UML_SESSION_CGROUP=v1:{cgroup1}")
    return "".join(f"{line}\n" for line in lines)


def stage_guest_files(
    directory: pathlib.Path,
    session: str,
    payloads: Mapping[str, GuestFile],
    root_type: str | None = None,
) -> list[tuple[str, pathlib.Path]]:
    """Write a harness session's initramfs inputs and return their pairs.

    The root on ``/dev/ubda`` is an EROFS image unless ``root_type`` says it
    is an unpartitioned ext4 filesystem."""
    if root_type is not None and root_type not in ("erofs", "ext4"):
        raise ValueError(f"unknown root type {root_type!r}")
    boot = directory / "boot.env"
    boot_lines = [f"UML_BOOT_ENTRY={SESSION_GUEST_PATH}\n"]
    if root_type is not None:
        boot_lines.append(f"UML_BOOT_ROOT_TYPE={root_type}\n")
    boot.write_text("".join(boot_lines))
    parameters = directory / "session.env"
    parameters.write_text(session)
    files = [
        ("/boot.env", boot),
        (f"/payload{SESSION_GUEST_PATH}", SESSION_SCRIPT),
        (f"/payload{SESSION_PARAMETERS_GUEST_PATH}", parameters),
    ]
    payload_directory = directory / "payload"
    for index, (path, payload) in enumerate(sorted(payloads.items())):
        validate_payload_path(path)
        if path in (SESSION_GUEST_PATH, SESSION_PARAMETERS_GUEST_PATH):
            raise ValueError(f"guest payload {path!r} replaces the session")
        payload_directory.mkdir(exist_ok=True)
        host = payload_directory / f"{index:04d}"
        host.write_bytes(payload.contents)
        host.chmod(payload.mode)
        files.append((f"/payload{path}", host))
    return files


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


def abandoned_host_files(pattern: re.Pattern[str]) -> list[pathlib.Path]:
    """Return the temporary files ``pattern`` names whose process has exited.

    A harness names each per-run host file after its process. One killed by a
    signal it cannot handle runs no cleanup, so later harnesses remove what it
    left, as the launcher does for its initrd scratch. A process that is alive,
    or that this user may not signal, keeps its files."""
    abandoned = []
    for path in pathlib.Path(tempfile.gettempdir()).glob("uml-*"):
        match = pattern.fullmatch(path.name)
        if match is None:
            continue
        try:
            os.kill(int(match.group(1)), 0)
        except ProcessLookupError:
            abandoned.append(path)
        except (PermissionError, OverflowError):
            pass
    return abandoned


def cleanup_stale_swap_images() -> None:
    """Remove sparse swap backings left by terminated harness processes."""
    for path in abandoned_host_files(SWAP_IMAGE):
        path.unlink(missing_ok=True)


def cleanup_stale_guest_files() -> None:
    """Remove the staged guest files of terminated harness processes."""
    for path in abandoned_host_files(STAGING_DIRECTORY):
        if path.is_dir() and not path.is_symlink():
            shutil.rmtree(path, ignore_errors=True)


@functools.lru_cache(maxsize=None)
def darwin_user_temporary_directory() -> pathlib.Path | None:
    """Darwin's per-user temporary directory, which tempfile never consults."""
    try:
        result = subprocess.run(
            ["/usr/bin/getconf", "DARWIN_USER_TEMP_DIR"],
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            timeout=10,
            check=True,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    directory = result.stdout.strip()
    return pathlib.Path(directory) if directory.startswith("/") else None


def remove_emulator_run_directories(process_id: int) -> None:
    """Remove the run directories of an emulator that has exited.

    An emulator removes them itself on every exit it observes, and a later
    emulator removes those of runs since killed outright. A harness that kills
    one removes them at once, so its test leaves nothing behind. They are in
    $TMPDIR, or in Darwin's per-user temporary directory when $TMPDIR is unset.
    Nothing is removed while the process exists."""
    try:
        os.kill(process_id, 0)
        return
    except ProcessLookupError:
        pass
    except (PermissionError, OverflowError):
        return
    directories = {pathlib.Path(tempfile.gettempdir())}
    if (user := darwin_user_temporary_directory()) is not None:
        directories.add(user)
    for directory in directories:
        for path in directory.glob(f"uml-*{process_id}*"):
            for pattern in EMULATOR_RUN_DIRECTORIES:
                match = pattern.fullmatch(path.name)
                if (
                    match is not None
                    and int(match.group(1)) == process_id
                    and path.is_dir()
                    and not path.is_symlink()
                ):
                    shutil.rmtree(path, ignore_errors=True)


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


class ConsoleProcess:
    """One emulator process driven through its serial console.

    Output goes to a PTY, which keeps it terminal-shaped and unbuffered;
    input goes through a pipe, because Darwin can miss PTY readiness while
    every guest CPU is busy and a pipe stays pollable at guest timer
    boundaries. Writes have their own deadline. Commands are wrapped in
    unique markers and read back from the ANSI-normalized transcript.
    """

    # How long a guest has to exit after SIGTERM before it is killed.
    STOP_GRACE_SECONDS = 2.0
    KILL_WAIT_SECONDS = 30.0

    def __init__(
        self,
        arguments: Sequence[str],
        *,
        environment: Mapping[str, str] | None = None,
        pass_fds: Sequence[int] = (),
        console_sink: BinaryIO | None = None,
    ) -> None:
        self._transcript = bytearray()
        # Receives every console byte as the harness reads it, so a caller
        # can stream a long command's output instead of waiting for its end.
        self._console_sink = console_sink
        self.first_output_at: float | None = None
        master, slave = pty.openpty()
        self._master = master
        os.set_blocking(master, False)
        try:
            self._process = subprocess.Popen(
                list(arguments),
                env=None if environment is None else dict(environment),
                stdin=subprocess.PIPE,
                stdout=slave,
                stderr=slave,
                close_fds=True,
                # Descriptors a kernel argument names, such as an external
                # vhost-user backend's end of a socket pair.
                pass_fds=tuple(pass_fds),
            )
        except BaseException:
            os.close(master)
            raise
        finally:
            os.close(slave)
        assert self._process.stdin is not None
        self._input = self._process.stdin
        os.set_blocking(self._input.fileno(), False)

    def __enter__(self) -> ConsoleProcess:
        return self

    def __exit__(self, exception_type, exception, traceback) -> None:
        """Print the transcript's end when an exception propagates, then close."""
        try:
            if exception_type is not None:
                print(
                    self.output()[-FAILURE_TRANSCRIPT_CHARACTERS:],
                    file=sys.stderr,
                )
        finally:
            self.close()

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

    def run_command(
        self,
        command: str,
        timeout_seconds: float = 30,
        *,
        preamble: str = "",
        single_write: bool = False,
        stdin: str = "",
    ) -> CommandResult:
        """Run a shell command through the serial console and capture it.

        ``preamble`` is sent ahead of the command as part of the same input;
        its output is not captured. ``stdin`` follows the command line, so the
        command reads it from the console. ``single_write`` delivers all of
        that input in one host write, so the guest receives it as one burst
        rather than as the pipe accepts it. A command still running after
        ``timeout_seconds`` raises CommandTimeout with what it printed.
        """
        token = uuid.uuid4().hex
        begin = f"__UML_COMMAND_BEGIN_{token}__"
        end = f"__UML_COMMAND_END_{token}__"
        wrapped = (
            f"{preamble}printf '\\n{begin}\\n'; {command}; rc=$?; "
            f"printf '\\n{end}:%s\\n' \"$rc\"\n"
        )
        start = len(self._transcript)
        if single_write:
            self._write_once(wrapped + stdin)
        else:
            self._write(wrapped + stdin)

        def output(section: bytes) -> str:
            begin_at = section.rfind(begin.encode())
            if begin_at >= 0:
                section = section[begin_at + len(begin) :]
            return self._normal_output(section)

        try:
            match = self._read_until(marker_pattern(end), timeout_seconds, start)
        except TimeoutError as error:
            raise CommandTimeout(
                str(error), output(bytes(self._transcript[start:]))
            ) from error
        return CommandResult(
            int(match.group(1)),
            output(bytes(self._transcript[start : match.start()])),
        )

    def check_command(
        self,
        command: str,
        timeout_seconds: float = 30,
        *,
        expected: str | None = None,
    ) -> str:
        """Run a command, raise unless it exits zero, and return its output.

        With ``expected``, the stripped output must also equal it."""
        result = self.run_command(command, timeout_seconds)
        if result.return_code != 0:
            raise RuntimeError(
                f"guest command failed ({result.return_code}): {command}\n"
                f"{result.output}"
            )
        if expected is not None and result.output.strip() != expected:
            raise RuntimeError(
                f"guest command {command!r} returned {result.output!r}, "
                f"expected {expected!r}"
            )
        return result.output

    def wait_for_exit(self, timeout_seconds: float) -> bool:
        """Keep reading the console until the emulator exits or time runs out."""
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
        return True

    def close(self) -> None:
        """Stop the emulator if it still runs, release the console and remove
        what an emulator killed outright left."""
        try:
            if self._process.poll() is None:
                self._signal(signal.SIGTERM)
                if not self.wait_for_exit(self.STOP_GRACE_SECONDS):
                    self._signal(signal.SIGKILL)
                    self._process.wait(timeout=self.KILL_WAIT_SECONDS)
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
            except OSError:
                pass
            finally:
                try:
                    os.close(self._master)
                except OSError:
                    pass
                if self._process.poll() is not None:
                    remove_emulator_run_directories(self._process.pid)

    def output(self) -> str:
        return self._normal_output(bytes(self._transcript))

    def _exited_without_output(self) -> None:
        """Explain an emulator that exited before the awaited output.

        Raise a more specific error than the generic exit status; the default
        adds nothing."""

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

    def _write_once(self, text: str) -> None:
        payload = text.encode()
        try:
            written = os.write(self._input.fileno(), payload)
        except BlockingIOError:
            written = 0
        if written != len(payload):
            raise RuntimeError(
                f"guest console accepted {written} of {len(payload)} bytes "
                "in one write"
            )

    def _read_until(
        self,
        pattern: re.Pattern[bytes],
        timeout_seconds: float,
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
                self._exited_without_output()
                raise RuntimeError(
                    f"guest exited with status {self._process.returncode}"
                )
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(
                    f"guest operation exceeded {timeout_seconds:g}s"
                )
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
            if self.first_output_at is None:
                self.first_output_at = time.monotonic()
            self._transcript.extend(chunk)
            if self._console_sink is not None:
                self._console_sink.write(chunk)
                self._console_sink.flush()

    @staticmethod
    def _normal_output(output: bytes) -> str:
        decoded = output.decode("utf-8", errors="replace").replace("\r", "")
        return ANSI_ESCAPE.sub("", decoded).strip()


class Guest(ConsoleProcess):
    """A harness shell on the unified image, booted through the ramdisk.

    The launcher's per-run initramfs mounts the image beneath a tmpfs
    overlay, installs ``payloads`` and the session, and the session starts a
    root shell on the console. ``overlay_shell`` gives that shell what an
    init would: runtime mounts, the SLiRP address and resolver, and the
    ``workspace`` under /tmp. ``init_shell`` gives a bare shell on the same
    overlay. Swap and cgroup hierarchies are ready before the prompt.

    The root is an EROFS image on /dev/ubda; ``root_type="ext4"`` makes it
    an ext4 filesystem instead, which the overlay still mounts read-only.
    ``network="image"`` leaves the guest without the SLiRP address and
    resolver, as an image without ``ip`` or a measurement that must not touch
    a network needs.
    """

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
        engine: str = "jit",
        core_class: str = "performance",
        pass_fds: Sequence[int] = (),
        console_sink: BinaryIO | None = None,
        payloads: Mapping[str, GuestFile] = {},
        root_type: str | None = None,
        network: str = "user-network",
    ) -> None:
        if init_shell == overlay_shell:
            raise ValueError(
                "a guest boots exactly one of init_shell or overlay_shell"
            )
        self._swap_path: pathlib.Path | None = None
        self._staging: pathlib.Path | None = None
        self._core_class = core_class
        try:
            session = session_parameters(
                overlay_shell=overlay_shell,
                workspace=workspace,
                swap=swap_size is not None,
                cgroup1=cgroup1,
                cgroup2_nsdelegate=cgroup2_nsdelegate,
                network=network,
            )
            self._swap_path = create_swap_image(swap_size) if swap_size else None
            cleanup_stale_guest_files()
            self._staging = pathlib.Path(
                tempfile.mkdtemp(prefix=f"{STAGING_PREFIX}{os.getpid()}-")
            )
            guest_files = stage_guest_files(
                self._staging, session, payloads, root_type
            )
            super().__init__(
                emulator_arguments(
                    emulator,
                    rootfs,
                    memory,
                    (*kernel_arguments, "console=tty0", "console=ttyS0"),
                    self._swap_path,
                    engine=engine,
                    core_class=core_class,
                    guest_files=guest_files,
                ),
                pass_fds=pass_fds,
                console_sink=console_sink,
            )
        except BaseException:
            self._remove_host_files()
            raise

    def boot_to_shell(self, timeout_seconds: float = BOOT_TIMEOUT_SECONDS) -> None:
        """Reach the root shell the session starts."""
        ready = self._read_until(
            re.compile(
                re.escape(f"{SESSION_READY_MARKER}/bin/sh".encode()) + rb"\r?\n"
            ),
            timeout_seconds,
        )
        # The search starts on the marker's line feed: a pattern's ``^`` does
        # not match where a search starts.
        self._read_until(
            re.compile(LINE_START + rb"# $"), timeout_seconds, ready.end() - 1
        )

    def boot_and_login(self, timeout_seconds: float = BOOT_TIMEOUT_SECONDS) -> None:
        self.boot_to_shell(timeout_seconds)

    def shutdown(self, timeout_seconds: float = 30) -> bool:
        """Request an orderly halt and wait for the emulator to exit."""
        if self._process.poll() is not None:
            return True
        try:
            self._write("sync; poweroff -f\n")
            return self.wait_for_exit(timeout_seconds)
        except (BrokenPipeError, OSError):
            return self._process.poll() is not None

    def close(self) -> None:
        try:
            try:
                self.shutdown(5)
            finally:
                super().close()
        finally:
            self._remove_host_files()

    def _remove_host_files(self) -> None:
        if self._swap_path is not None:
            self._swap_path.unlink(missing_ok=True)
            self._swap_path = None
        if self._staging is not None:
            shutil.rmtree(self._staging, ignore_errors=True)
            self._staging = None
