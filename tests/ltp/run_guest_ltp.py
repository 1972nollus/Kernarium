#!/usr/bin/env python3
"""Run verified LTP cases in isolated Darwin UML guests."""

from __future__ import annotations

import argparse
import concurrent.futures
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
import threading
import time
import uuid
from collections.abc import Sequence


ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
SUMMARY_VALUE = re.compile(
    r"^(passed|failed|broken|skipped|warnings)\s+(\d+)\s*$",
    re.IGNORECASE | re.MULTILINE,
)
RESULT_TAG = re.compile(r"^.*?:\d+: (TPASS|TFAIL|TBROK|TCONF|TWARN):", re.MULTILINE)
LEGACY_RESULT_TAG = re.compile(
    r"^\S+\s+\d+\s+(TPASS|TFAIL|TBROK|TCONF|TWARN)\s*:", re.MULTILINE
)
LEGACY_CHILD_RESULT_TAG = re.compile(
    r"\bChild process returned (TPASS|TFAIL|TBROK|TCONF|TWARN)\b"
)
KERNEL_FATAL = re.compile(
    r"^(BUG:|Oops:|Kernel panic|Unable to handle kernel)", re.MULTILINE
)
VALID_EXPECTATIONS = {
    "exit-zero",
    "pass",
    "hardware-exempt",
    "hardware-failure-exempt",
    "libc-exempt",
    "static-kernel-exempt",
    "userspace-exempt",
    "userspace-broken-exempt",
    "userspace-error-exempt",
    "userspace-failure-exempt",
    "userspace-flaky-exempt",
    "userspace-legacy-matrix-exempt",
    "userspace-matrix-exempt",
    "userspace-missing-exempt",
    "userspace-partial-broken-exempt",
    "userspace-variable-matrix-exempt",
    "userspace-warning-exempt",
}
VALID_WORKSPACES = {"tmpfs", "ext2"}
HOST_PRESSURE_OPTION = "host-pressure"
SERIAL_OPTION = "serial"
CGROUP2_NSDELEGATE_OPTION = "cgroup2=nsdelegate"
KERNEL_ARGUMENT_PREFIX = "kernel-arg="
BOOT_TIMEOUT_SECONDS = 90
SIZE_VALUE = re.compile(r"^(\d+)([KMGT]?)$", re.IGNORECASE)
SWAP_IMAGE = re.compile(r"^uml-ltp-swap-(\d+)-.*\.img$")
CGROUP1_CONTROLLER = re.compile(r"^[a-z0-9_]+$")
LTP_ROOT = "/usr/lib/ltp-testsuite"
LTP_BIN = f"{LTP_ROOT}/testcases/bin"
LTP_TIMEOUT_MUL = 4
# Keep every ordinary guest single-vCPU.  Workloads which intentionally
# exercise more than one CPU are manifest-tagged host-pressure, retain the full
# guest topology, and run alone at the end of the suite.
DEFAULT_PARALLEL_GUEST_CPUS = 1
SYSTEMD_OVERLAY_BOOTSTRAP = (
    "set -eu; "
    "mount -t tmpfs -o mode=0755,nosuid,nodev tmpfs /mnt; "
    "mkdir -p /mnt/lower /mnt/upper /mnt/work /mnt/root; "
    "mount -o bind / /mnt/lower; "
    "mount -t overlay overlay "
    "-o lowerdir=/mnt/lower,upperdir=/mnt/upper,workdir=/mnt/work /mnt/root; "
    "mount -t devtmpfs devtmpfs /mnt/root/dev; "
    "mkdir -p /mnt/root/.uml-lower; "
    "cd /mnt/root; "
    "pivot_root . .uml-lower; "
    "rm -f /etc/resolv.conf; "
    "printf '%s\\n' 'nameserver 1.1.1.1' 'nameserver 8.8.8.8' "
    "'nameserver 10.0.2.3' > /etc/resolv.conf; "
    "exec /lib/systemd/systemd"
)
_ACTIVE_GUESTS: set["Guest"] = set()
_ACTIVE_GUESTS_LOCK = threading.Lock()


@dataclasses.dataclass(frozen=True)
class Case:
    scenario: str
    test_id: str
    command: str
    expectation: str
    timeout_seconds: int
    reason: str
    memory: str | None = None
    workspace: str = "tmpfs"
    swap: str | None = None
    cgroup1: str | None = None
    cgroup2_nsdelegate: bool = False
    host_pressure: bool = False
    serial: bool = False
    kernel_arguments: tuple[str, ...] = ()

    @property
    def key(self) -> str:
        return f"{self.scenario}/{self.test_id}"


@dataclasses.dataclass(frozen=True)
class Result:
    case: Case
    return_code: int | None
    summary: dict[str, int]
    output: str
    error: str | None = None


@dataclasses.dataclass(frozen=True)
class CommandResult:
    return_code: int
    output: str


def emulator_arguments(
    emulator: pathlib.Path,
    rootfs: pathlib.Path,
    memory: str,
    workspace: str = "tmpfs",
    kernel_arguments: Sequence[str] = (),
    swap_path: pathlib.Path | None = None,
    init_path: str = "/sbin/uml-overlay-init",
) -> list[str]:
    arguments = [
        str(emulator),
        f"ubd0r={rootfs}",
        "ro",
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
    """Parse a manifest memory/disk size using binary K/M/G/T suffixes."""
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


def parse_result_counts(output: str) -> dict[str, int]:
    summary = {
        name.lower(): int(value) for name, value in SUMMARY_VALUE.findall(output)
    }
    if summary:
        return summary

    tag_to_name = {
        "TPASS": "passed",
        "TFAIL": "failed",
        "TBROK": "broken",
        "TCONF": "skipped",
        "TWARN": "warnings",
    }
    counts: dict[str, int] = {}
    tags = RESULT_TAG.findall(output) + LEGACY_RESULT_TAG.findall(output)
    if not tags:
        tags = LEGACY_CHILD_RESULT_TAG.findall(output)
    for tag in tags:
        name = tag_to_name[tag]
        counts[name] = counts.get(name, 0) + 1
    return counts


def kernel_failure(output: str) -> str | None:
    """Reject a nominal test pass if the guest reported kernel corruption."""
    match = KERNEL_FATAL.search(output)
    if match is None:
        return None
    line = output[match.start() :].splitlines()[0]
    return f"guest kernel reported a fatal diagnostic: {line}"


def marker_pattern(marker: str) -> re.Pattern[bytes]:
    """Match protocol markers even if a tty ioctl enables case conversion."""
    return re.compile(
        re.escape(marker.encode()) + rb":(\d+)\r?\n",
        re.IGNORECASE,
    )


def ltp_guest_command(command: str, tmpdir: str = "/tmp/uml-ltp-case") -> str:
    """Build the command environment used by LTP's scenario executor."""
    return (
        f"mkdir -p {tmpdir} {LTP_ROOT}/output && "
        f"chmod 1777 {tmpdir} {LTP_ROOT}/output && "
        f"cd {LTP_BIN} && "
        f"PATH={LTP_BIN}:$PATH "
        f"LTPROOT={LTP_ROOT} "
        f"TMPDIR={tmpdir} "
        f"LTP_TIMEOUT_MUL={LTP_TIMEOUT_MUL} "
        f"sh -c {shlex.quote(command)}"
    )


class Guest:
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
        systemd_overlay: bool = False,
    ) -> None:
        self._swap_path = create_swap_image(swap_size) if swap_size else None
        self._cgroup1 = cgroup1
        self._cgroup2_nsdelegate = cgroup2_nsdelegate
        self._systemd_overlay = systemd_overlay
        if systemd_overlay:
            kernel_arguments = (
                *kernel_arguments,
                "console=tty0",
                "ip=dhcp",
                "systemd.unit=multi-user.target",
            )
        master, slave = pty.openpty()
        self._master = master
        os.set_blocking(master, False)
        try:
            self._process = subprocess.Popen(
                emulator_arguments(
                    emulator, rootfs, memory, workspace, kernel_arguments,
                    self._swap_path,
                    "/bin/sh" if systemd_overlay else "/sbin/uml-overlay-init",
                ),
                stdin=slave,
                stdout=slave,
                stderr=slave,
                close_fds=True,
                start_new_session=True,
            )
        except BaseException:
            os.close(master)
            self._remove_swap_image()
            raise
        finally:
            os.close(slave)
        self._transcript = bytearray()

    @property
    def process_id(self) -> int:
        return self._process.pid

    def terminate(self) -> None:
        """Force a guest worker to leave a blocking console operation."""
        self._signal(signal.SIGKILL)

    def _signal(self, guest_signal: int) -> None:
        """Signal the guest session, falling back to its leader if required."""
        if self._process.poll() is None:
            try:
                os.killpg(self._process.pid, guest_signal)
            except ProcessLookupError:
                pass
            except PermissionError:
                # Some restricted host environments permit signalling the
                # direct child but reject process-group signalling. UML puts
                # all guest tasks in that one process, so killing the session
                # leader still releases every guest thread and backing map.
                try:
                    self._process.send_signal(guest_signal)
                except ProcessLookupError:
                    pass

    def boot_and_login(self, timeout_seconds: int = BOOT_TIMEOUT_SECONDS) -> None:
        if self._systemd_overlay:
            self._read_until(
                re.compile(rb"(?:^|\r?\n)# $"), timeout_seconds
            )
            self._write(SYSTEMD_OVERLAY_BOOTSTRAP + "\n")
            self._read_until(
                re.compile(rb" login:\s*root \(automatic login\)",
                           re.MULTILINE),
                timeout_seconds,
            )
            self._read_until(
                re.compile(rb"(?:^|\r?\n)[^\r\n]*# $"),
                timeout_seconds,
            )
        else:
            self._read_until(
                re.compile(rb" login:\s*$", re.MULTILINE), timeout_seconds
            )
            self._write("root\n")
            self._read_until(re.compile(rb"(?:^|\r?\n)# $"), timeout_seconds)
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
        match = self._read_until(
            marker_pattern(end), timeout_seconds, start
        )
        section = bytes(self._transcript[start : match.start()])
        begin_at = section.rfind(begin.encode())
        if begin_at >= 0:
            section = section[begin_at + len(begin) :]
        return CommandResult(
            int(match.group(1)), self._normal_output(section)
        )

    def run(self, case: Case) -> Result:
        token = uuid.uuid4().hex
        begin = f"__UML_LTP_BEGIN_{token}__"
        end = f"__UML_LTP_END_{token}__"
        guest_command = ltp_guest_command(case.command)
        wrapped = (
            f"printf '\\n{begin}\\n'; {guest_command}; rc=$?; "
            f"printf '\\n{end}:%s\\n' \"$rc\"\n"
        )
        start = len(self._transcript)
        self._write(wrapped)
        try:
            match = self._read_until(
                marker_pattern(end),
                case.timeout_seconds,
                start,
            )
        except TimeoutError as error:
            output = self._normal_output(bytes(self._transcript[start:]))
            return Result(case, None, {}, output, str(error))

        section = bytes(self._transcript[start : match.end()])
        begin_at = section.rfind(begin.encode(), 0, match.start() - start)
        if begin_at >= 0:
            section = section[begin_at + len(begin) :]
        output = self._normal_output(section)
        summary = parse_result_counts(output)
        health_error = kernel_failure(output) or self._health_check()
        return Result(
            case,
            int(match.group(1)),
            summary,
            output,
            health_error,
        )

    def _health_check(self, timeout_seconds: int = 30) -> str | None:
        """Verify that a completed test did not damage later guest execution."""
        token = uuid.uuid4().hex
        marker = f"__UML_HEALTH_{token}__"
        start = len(self._transcript)
        self._write(
            "stty sane 2>/dev/null; zcat /proc/config.gz >/dev/null; rc=$?; "
            f"printf '\\n{marker}:%s\\n' \"$rc\"\n"
        )
        try:
            match = self._read_until(
                marker_pattern(marker),
                timeout_seconds,
                start,
            )
        except (RuntimeError, TimeoutError) as error:
            return f"post-test guest health check failed: {error}"
        return_code = int(match.group(1))
        if return_code != 0:
            return (
                "post-test guest health check failed: "
                f"zcat /proc/config.gz exited {return_code}"
            )
        return None

    def close(self) -> None:
        try:
            if self._process.poll() is None:
                try:
                    self._write("sync; poweroff -f\n")
                    self._process.wait(timeout=5)
                except (BrokenPipeError, OSError, subprocess.TimeoutExpired):
                    pass
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
                os.close(self._master)
            finally:
                self._remove_swap_image()

    def _remove_swap_image(self) -> None:
        if self._swap_path is not None:
            self._swap_path.unlink(missing_ok=True)
            self._swap_path = None

    def output(self) -> str:
        return self._normal_output(bytes(self._transcript))

    def _write(self, text: str) -> None:
        os.write(self._master, text.encode())

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
            except BlockingIOError:
                return
            except OSError:
                return
            if not chunk:
                return
            self._transcript.extend(chunk)

    @staticmethod
    def _normal_output(output: bytes) -> str:
        decoded = output.decode("utf-8", errors="replace").replace("\r", "")
        return ANSI_ESCAPE.sub("", decoded).strip()


def load_manifest(path: pathlib.Path) -> list[Case]:
    cases: list[Case] = []
    keys: set[str] = set()
    with path.open(encoding="utf-8") as manifest:
        for line_number, raw_line in enumerate(manifest, 1):
            line = raw_line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            fields = line.split("\t")
            if len(fields) < 6:
                raise ValueError(
                    f"{path}:{line_number}: expected at least six "
                    "tab-separated fields"
                )
            scenario, test_id, command, expectation, timeout, reason = fields[:6]
            memory = None
            workspace = "tmpfs"
            swap = None
            cgroup1 = None
            cgroup2_nsdelegate = False
            host_pressure = False
            serial = False
            kernel_arguments: list[str] = []
            for extra in fields[6:]:
                if extra in VALID_WORKSPACES:
                    workspace = extra
                elif extra == HOST_PRESSURE_OPTION:
                    if host_pressure:
                        raise ValueError(
                            f"{path}:{line_number}: duplicate "
                            f"{HOST_PRESSURE_OPTION!r} option"
                        )
                    host_pressure = True
                elif extra == SERIAL_OPTION:
                    if serial:
                        raise ValueError(
                            f"{path}:{line_number}: duplicate "
                            f"{SERIAL_OPTION!r} option"
                        )
                    serial = True
                elif extra.startswith("swap=") and swap is None:
                    swap = extra.removeprefix("swap=")
                    size_bytes(swap)
                elif extra.startswith("cgroup1=") and cgroup1 is None:
                    cgroup1 = extra.removeprefix("cgroup1=")
                    if CGROUP1_CONTROLLER.fullmatch(cgroup1) is None:
                        raise ValueError(
                            f"{path}:{line_number}: invalid cgroup v1 "
                            f"controller {cgroup1!r}"
                        )
                elif extra == CGROUP2_NSDELEGATE_OPTION:
                    if cgroup2_nsdelegate:
                        raise ValueError(
                            f"{path}:{line_number}: duplicate "
                            f"{CGROUP2_NSDELEGATE_OPTION!r} option"
                        )
                    cgroup2_nsdelegate = True
                elif extra.startswith(KERNEL_ARGUMENT_PREFIX):
                    kernel_argument = extra.removeprefix(KERNEL_ARGUMENT_PREFIX)
                    if not kernel_argument:
                        raise ValueError(
                            f"{path}:{line_number}: empty kernel argument"
                        )
                    if kernel_argument in kernel_arguments:
                        raise ValueError(
                            f"{path}:{line_number}: duplicate kernel argument "
                            f"{kernel_argument!r}"
                        )
                    kernel_arguments.append(kernel_argument)
                elif extra and memory is None:
                    memory = extra
                else:
                    raise ValueError(
                        f"{path}:{line_number}: invalid manifest option {extra!r}"
                    )
            if cgroup1 is not None and cgroup2_nsdelegate:
                raise ValueError(
                    f"{path}:{line_number}: cgroup v1 and cgroup v2 "
                    "resource options are mutually exclusive"
                )
            if host_pressure and serial:
                raise ValueError(
                    f"{path}:{line_number}: {HOST_PRESSURE_OPTION!r} and "
                    f"{SERIAL_OPTION!r} options are mutually exclusive"
                )
            if expectation not in VALID_EXPECTATIONS:
                raise ValueError(
                    f"{path}:{line_number}: invalid expectation {expectation!r}"
                )
            case = Case(
                scenario=scenario,
                test_id=test_id,
                command=command,
                expectation=expectation,
                timeout_seconds=int(timeout),
                reason=reason,
                memory=memory,
                workspace=workspace,
                swap=swap,
                cgroup1=cgroup1,
                cgroup2_nsdelegate=cgroup2_nsdelegate,
                host_pressure=host_pressure,
                serial=serial,
                kernel_arguments=tuple(kernel_arguments),
            )
            if case.key in keys:
                raise ValueError(f"{path}:{line_number}: duplicate case {case.key}")
            keys.add(case.key)
            cases.append(case)
    if not cases:
        raise ValueError(f"{path}: manifest contains no verified cases")
    return cases


def load_disabled_manifest(path: pathlib.Path) -> dict[str, str]:
    """Load cases intentionally omitted from this emulator's LTP campaign."""
    disabled: dict[str, str] = {}
    with path.open(encoding="utf-8") as manifest:
        for line_number, raw_line in enumerate(manifest, 1):
            line = raw_line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            fields = line.split("\t")
            if len(fields) != 3:
                raise ValueError(
                    f"{path}:{line_number}: expected three tab-separated fields"
                )
            scenario, test_id, reason = fields
            key = f"{scenario}/{test_id}"
            if not scenario or not test_id or not reason:
                raise ValueError(
                    f"{path}:{line_number}: disabled case fields must be nonempty"
                )
            if key in disabled:
                raise ValueError(
                    f"{path}:{line_number}: duplicate disabled case {key}"
                )
            disabled[key] = reason
    return disabled


def run_case(
    emulator: pathlib.Path, rootfs: pathlib.Path, memory: str, case: Case
) -> Result:
    guest: Guest | None = None
    try:
        guest = Guest(
            emulator, rootfs, memory, case.workspace,
            kernel_arguments=case.kernel_arguments,
            swap_size=case.swap, cgroup1=case.cgroup1,
            cgroup2_nsdelegate=case.cgroup2_nsdelegate,
        )
        with _ACTIVE_GUESTS_LOCK:
            _ACTIVE_GUESTS.add(guest)
        guest.boot_and_login()
        return guest.run(case)
    except (OSError, RuntimeError, TimeoutError) as error:
        output = guest.output() if guest is not None else ""
        return Result(case, None, {}, output, str(error))
    finally:
        if guest is not None:
            try:
                guest.close()
            finally:
                with _ACTIVE_GUESTS_LOCK:
                    _ACTIVE_GUESTS.discard(guest)


def terminate_active_guests() -> None:
    """Stop every currently running worker guest after host interruption."""
    with _ACTIVE_GUESTS_LOCK:
        guests = tuple(_ACTIVE_GUESTS)
    for guest in guests:
        guest.terminate()


def limit_parallel_guest_cpus(case: Case, cpu_count: int) -> Case:
    """Give an ordinary guest exactly the requested single-CPU topology."""
    kernel_arguments = tuple(
        argument for argument in case.kernel_arguments
        if not argument.startswith("ncpus=")
    )
    return dataclasses.replace(
        case,
        kernel_arguments=kernel_arguments + (f"ncpus={cpu_count}",),
    )


def cases_from(cases: list[Case], key: str) -> list[Case]:
    """Return the manifest suffix beginning at key."""
    for index, case in enumerate(cases):
        if case.key == key:
            return cases[index:]
    raise ValueError(f"unknown start case: {key}")


def select_host_pressure_cases(
    cases: list[Case], run_host_pressure: bool
) -> tuple[list[Case], int]:
    """Exclude host-pressure cases unless they were explicitly requested."""
    pressure_count = sum(case.host_pressure for case in cases)
    if run_host_pressure:
        return cases, 0
    return [case for case in cases if not case.host_pressure], pressure_count


def run_cases(
    emulator: pathlib.Path,
    rootfs: pathlib.Path,
    memory: str,
    cases: list[Case],
    jobs: int,
    worker=run_case,
    parallel_guest_cpus: int = DEFAULT_PARALLEL_GUEST_CPUS,
):
    regular_cases = [
        case for case in cases if not case.host_pressure and not case.serial
    ]
    serial_cases = [case for case in cases if case.serial]
    pressure_cases = [case for case in cases if case.host_pressure]
    if jobs == 1:
        for index, case in enumerate(regular_cases, 1):
            case = limit_parallel_guest_cpus(case, parallel_guest_cpus)
            yield index, worker(emulator, rootfs, case.memory or memory, case)
        first_serial_index = len(regular_cases) + 1
        for index, case in enumerate(serial_cases, first_serial_index):
            yield index, worker(emulator, rootfs, case.memory or memory, case)
        first_pressure_index = len(regular_cases) + len(serial_cases) + 1
        for index, case in enumerate(pressure_cases, first_pressure_index):
            yield index, worker(emulator, rootfs, case.memory or memory, case)
        return

    if regular_cases:
        executor = concurrent.futures.ThreadPoolExecutor(
            max_workers=jobs, thread_name_prefix="guest-ltp"
        )
        try:
            futures = {
                executor.submit(
                    worker, emulator, rootfs, case.memory or memory,
                    limit_parallel_guest_cpus(case, parallel_guest_cpus),
                ): index
                for index, case in enumerate(regular_cases, 1)
            }
            for future in concurrent.futures.as_completed(futures):
                yield futures[future], future.result()
        except BaseException:
            for future in futures:
                future.cancel()
            terminate_active_guests()
            executor.shutdown(wait=True, cancel_futures=True)
            raise
        else:
            executor.shutdown(wait=True)

    first_serial_index = len(regular_cases) + 1
    for index, case in enumerate(serial_cases, first_serial_index):
        yield index, worker(emulator, rootfs, case.memory or memory, case)

    first_pressure_index = len(regular_cases) + len(serial_cases) + 1
    for index, case in enumerate(pressure_cases, first_pressure_index):
        yield index, worker(emulator, rootfs, case.memory or memory, case)


def accepted(result: Result) -> bool:
    if result.error is not None or result.return_code is None:
        return False
    failed = result.summary.get("failed", 0)
    broken = result.summary.get("broken", 0)
    skipped = result.summary.get("skipped", 0)
    if result.case.expectation == "exit-zero":
        return (
            result.return_code == 0
            and failed == 0
            and broken == 0
            and skipped == 0
        )
    if result.case.expectation == "pass":
        warnings = result.summary.get("warnings", 0)
        warning_only_status = (
            result.return_code == 4 and warnings > 0
        )
        return (
            (result.return_code == 0 or warning_only_status)
            and failed == 0
            and broken == 0
            and skipped == 0
            and result.summary.get("passed", 0) > 0
        )
    if result.case.expectation == "userspace-failure-exempt":
        return result.return_code != 0 and failed > 0 and broken == 0
    if result.case.expectation == "hardware-failure-exempt":
        return result.return_code != 0 and failed > 0 and broken == 0
    if result.case.expectation == "userspace-matrix-exempt":
        return (
            result.return_code != 0
            and failed > 0
            and broken > 0
        )
    if result.case.expectation == "userspace-legacy-matrix-exempt":
        return (
            result.return_code == 0
            and result.summary.get("passed", 0) > 0
            and failed > 0
            and broken == 0
            and skipped == 0
        )
    if result.case.expectation == "userspace-partial-broken-exempt":
        return (
            result.return_code != 0
            and result.summary.get("passed", 0) > 0
            and failed == 0
            and broken > 0
        )
    if result.case.expectation == "userspace-variable-matrix-exempt":
        return (
            result.return_code != 0
            and result.summary.get("passed", 0) > 0
            and (failed > 0 or broken > 0)
        )
    if result.case.expectation == "userspace-warning-exempt":
        return (
            result.return_code == 4
            and result.summary.get("passed", 0) > 0
            and failed == 0
            and broken == 0
            and skipped == 0
            and result.summary.get("warnings", 0) > 0
        )
    if result.case.expectation == "userspace-flaky-exempt":
        warning_only = (
            result.return_code == 4
            and result.summary.get("passed", 0) > 0
            and failed == 0
            and broken == 0
            and skipped == 0
            and result.summary.get("warnings", 0) > 0
        )
        partial_break = (
            result.return_code != 0
            and result.summary.get("passed", 0) > 0
            and failed == 0
            and broken > 0
        )
        return warning_only or partial_break
    if result.case.expectation == "userspace-broken-exempt":
        return (
            result.return_code == 2
            and result.summary.get("passed", 0) == 0
            and failed == 0
            and broken > 0
            and skipped == 0
        )
    if result.case.expectation == "userspace-error-exempt":
        return result.return_code not in (0, 124) and not result.summary
    if result.case.expectation == "userspace-missing-exempt":
        return result.return_code == 127 and not result.summary
    if failed != 0 or broken != 0:
        return False
    if skipped > 0:
        return True
    return (
        result.case.expectation == "userspace-exempt"
        and result.return_code == 0
        and not result.summary
    )


def parse_arguments(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--manifest", required=True, type=pathlib.Path)
    parser.add_argument(
        "--disabled-manifest",
        type=pathlib.Path,
        default=pathlib.Path(__file__).with_name("disabled-tests.tsv"),
        help=(
            "cases intentionally omitted for this emulator "
            "(default: tests/ltp/disabled-tests.tsv)"
        ),
    )
    parser.add_argument(
        "--memory",
        default="1024M",
        help="guest RAM used for each isolated case (default: 1024M)",
    )
    parser.add_argument(
        "--case",
        action="append",
        default=[],
        help="run only SCENARIO/TEST_ID (repeatable)",
    )
    parser.add_argument(
        "--start-at",
        help="run the manifest suffix beginning at SCENARIO/TEST_ID",
    )
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="print full guest output only for failed cases",
    )
    parser.add_argument(
        "--fail-fast",
        action="store_true",
        help="terminate active guests and stop after the first rejected result",
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=1,
        help="number of isolated guest cases to run concurrently (default: 1)",
    )
    parser.add_argument(
        "--parallel-guest-cpus",
        type=int,
        default=DEFAULT_PARALLEL_GUEST_CPUS,
        help=(
            "vCPUs per ordinary guest; "
            "serial and host-pressure cases retain their configured SMP "
            f"topology (default: {DEFAULT_PARALLEL_GUEST_CPUS})"
        ),
    )
    parser.add_argument(
        "--run-host-pressure",
        action="store_true",
        help=(
            "include cases tagged host-pressure; these are excluded by default "
            "and run serially after ordinary and serial-only cases"
        ),
    )
    parser.add_argument(
        "--candidate",
        nargs=3,
        metavar=("SCENARIO", "TEST_ID", "COMMAND"),
        help="run one not-yet-verified case without adding it to the manifest",
    )
    parser.add_argument(
        "--candidate-expectation",
        choices=sorted(VALID_EXPECTATIONS),
        default="pass",
        help="expected outcome for --candidate (default: pass)",
    )
    parser.add_argument("--candidate-timeout", type=int, default=300)
    parser.add_argument(
        "--candidate-memory",
        help="override guest RAM for the single --candidate case",
    )
    parser.add_argument(
        "--candidate-workspace",
        choices=sorted(VALID_WORKSPACES),
        default="tmpfs",
        help="volatile /tmp filesystem for --candidate (default: tmpfs)",
    )
    parser.add_argument(
        "--candidate-swap",
        help="attach and enable a disposable sparse swap disk for the candidate",
    )
    parser.add_argument(
        "--candidate-cgroup1",
        help="mount one legacy cgroup v1 controller for the candidate",
    )
    parser.add_argument(
        "--candidate-cgroup2-nsdelegate",
        action="store_true",
        help="mount a cgroup v2 hierarchy with nsdelegate for the candidate",
    )
    parser.add_argument(
        "--candidate-kernel-argument",
        action="append",
        default=[],
        help=(
            "append one kernel command-line argument for the candidate "
            "(repeatable)"
        ),
    )
    return parser.parse_args(argv)


def main() -> int:
    arguments = parse_arguments()
    cleanup_stale_swap_images()
    for path in (
        arguments.emulator,
        arguments.rootfs,
        arguments.manifest,
        arguments.disabled_manifest,
    ):
        if not path.is_file():
            print(f"missing required file: {path}", file=sys.stderr)
            return 2
    cases = load_manifest(arguments.manifest)
    disabled = load_disabled_manifest(arguments.disabled_manifest)
    if arguments.jobs < 1:
        print("--jobs must be at least 1", file=sys.stderr)
        return 2
    if arguments.parallel_guest_cpus < 1:
        print("--parallel-guest-cpus must be at least 1", file=sys.stderr)
        return 2
    if sum(bool(value) for value in (
        arguments.candidate, arguments.case, arguments.start_at
    )) > 1:
        print(
            "--candidate, --case, and --start-at are mutually exclusive",
            file=sys.stderr,
        )
        return 2
    if arguments.candidate:
        if (
            arguments.candidate_cgroup1
            and arguments.candidate_cgroup2_nsdelegate
        ):
            print(
                "--candidate-cgroup1 and --candidate-cgroup2-nsdelegate "
                "are mutually exclusive",
                file=sys.stderr,
            )
            return 2
        if (
            any(not value for value in arguments.candidate_kernel_argument)
            or len(set(arguments.candidate_kernel_argument))
            != len(arguments.candidate_kernel_argument)
        ):
            print(
                "candidate kernel arguments must be nonempty and unique",
                file=sys.stderr,
            )
            return 2
        scenario, test_id, command = arguments.candidate
        candidate_key = f"{scenario}/{test_id}"
        if candidate_key in disabled:
            print(
                f"disabled case {candidate_key}: {disabled[candidate_key]}",
                file=sys.stderr,
            )
            return 2
        cases = [
            Case(
                scenario=scenario,
                test_id=test_id,
                command=command,
                expectation=arguments.candidate_expectation,
                timeout_seconds=arguments.candidate_timeout,
                reason="candidate awaiting review",
                memory=arguments.candidate_memory,
                workspace=arguments.candidate_workspace,
                swap=arguments.candidate_swap,
                cgroup1=arguments.candidate_cgroup1,
                cgroup2_nsdelegate=arguments.candidate_cgroup2_nsdelegate,
                kernel_arguments=tuple(arguments.candidate_kernel_argument),
            )
        ]
    elif arguments.case:
        selected = set(arguments.case)
        disabled_selected = selected & disabled.keys()
        if disabled_selected:
            details = ", ".join(
                f"{key} ({disabled[key]})" for key in sorted(disabled_selected)
            )
            print(f"disabled cases requested: {details}", file=sys.stderr)
            return 2
        cases = [case for case in cases if case.key in selected]
        missing = selected - {case.key for case in cases}
        if missing:
            print(f"unknown cases: {', '.join(sorted(missing))}", file=sys.stderr)
            return 2
    else:
        disabled_present = [case for case in cases if case.key in disabled]
        if disabled_present:
            cases = [case for case in cases if case.key not in disabled]
            print(
                f"Skipping {len(disabled_present)} emulator-disabled cases",
                flush=True,
            )
        if arguments.start_at:
            try:
                cases = cases_from(cases, arguments.start_at)
            except ValueError as error:
                print(str(error), file=sys.stderr)
                return 2

    cases, skipped_pressure_count = select_host_pressure_cases(
        cases, arguments.run_host_pressure
    )
    if skipped_pressure_count:
        print(
            f"Skipping {skipped_pressure_count} host-pressure cases by default; "
            "pass --run-host-pressure to include them",
            flush=True,
        )

    failures = 0
    completed = 0
    run_started = time.monotonic()
    emulator = arguments.emulator.resolve()
    rootfs = arguments.rootfs.resolve()
    if arguments.jobs > 1 and cases:
        regular_count = sum(
            not case.host_pressure and not case.serial for case in cases
        )
        serial_count = sum(case.serial for case in cases)
        pressure_count = sum(case.host_pressure for case in cases)
        print(
            f"Running {regular_count} regular isolated guests with "
            f"{arguments.jobs} worker threads and "
            f"{arguments.parallel_guest_cpus} vCPU per guest",
            flush=True,
        )
        if serial_count:
            print(
                f"Running {serial_count} serial-only cases one at a time "
                "after regular cases",
                flush=True,
            )
        if pressure_count:
            print(
                f"Running {pressure_count} host-pressure cases serially "
                "after regular cases",
                flush=True,
            )
    for index, result in run_cases(
        emulator,
        rootfs,
        arguments.memory,
        cases,
        arguments.jobs,
        parallel_guest_cpus=arguments.parallel_guest_cpus,
    ):
        completed += 1
        case = result.case
        print(f"[{index}/{len(cases)}] {case.key}: {case.command}", flush=True)
        if result.output and not arguments.quiet:
            print(result.output)
        if result.error:
            print(f"HARNESS ERROR: {result.error}")
        outcome = "PASS" if accepted(result) else "FAIL"
        if outcome == "FAIL" and arguments.quiet and result.output:
            print(result.output)
        print(
            f"{outcome} {case.key}: rc={result.return_code} "
            f"summary={result.summary}",
            flush=True,
        )
        failures += outcome == "FAIL"
        if outcome == "FAIL" and arguments.fail_fast:
            print("Stopping after first failure (--fail-fast)", flush=True)
            break
    print(
        f"Completed {completed} cases: {completed - failures} accepted, "
        f"{failures} failed in {time.monotonic() - run_started:.1f}s",
        flush=True,
    )
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
