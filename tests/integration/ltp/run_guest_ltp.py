#!/usr/bin/env python3
"""Run verified LTP cases in isolated Darwin UML guests."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import base64
import concurrent.futures
import dataclasses
import gzip
import json
import os
import pathlib
import re
import shlex
import signal
import sys
import threading
import time
import uuid


SUPPORT = pathlib.Path(__file__).resolve().parents[1] / "support"
sys.path.insert(0, str(SUPPORT))
from guest import (  # noqa: E402
    BOOT_TIMEOUT_SECONDS,
    OVERLAY_SHELL_BOOTSTRAP,
    SYSTEMD_OVERLAY_BOOTSTRAP,
    CommandResult,
    Guest as GuestHarness,
    cleanup_stale_swap_images,
    create_swap_image,
    emulator_arguments,
    marker_pattern,
    size_bytes,
)


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
CGROUP1_CONTROLLER = re.compile(r"^[a-z0-9_]+$")
LTP_ROOT = "/opt/ltp"
LTP_BIN = f"{LTP_ROOT}/testcases/bin"
LTP_OUTPUT = "/tmp/uml-ltp-output"
LTP_DISPOSABLE_TOOLS = "/run/uml-ltp-tools"
LTP_METADATA_TRANSPORT_ROOT = "/run/uml-ltp-metadata-transport"
LTP_METADATA_TRANSPORT_CHUNK_CHARS = 1024
LTP_METADATA_TRANSPORT_MAX_COMMAND_BYTES = 2048
LTP_TIMEOUT_MUL = 4
MODULES_BUILTIN_ENTRY = re.compile(
    r"^kernel/(?:[A-Za-z0-9._+-]+/)*[A-Za-z0-9._+-]+\.ko$"
)
KERNEL_RELEASE_VALUE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._+-]*$")
# Keep every ordinary guest single-vCPU.  Workloads which intentionally
# exercise more than one CPU are manifest-tagged host-pressure, retain the full
# guest topology, and run alone at the end of the suite.
DEFAULT_PARALLEL_GUEST_CPUS = 1
_ACTIVE_GUESTS: set["Guest"] = set()
_ACTIVE_GUESTS_LOCK = threading.Lock()


def validate_modules_builtin(data: bytes) -> tuple[str, ...]:
    """Validate canonical Kbuild ``modules.builtin`` bytes."""
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ValueError("modules.builtin is not UTF-8 text") from error
    if not text or not text.endswith("\n"):
        raise ValueError("modules.builtin must be nonempty and newline-terminated")
    entries = tuple(text.splitlines())
    if any(
        not MODULES_BUILTIN_ENTRY.fullmatch(entry)
        or any(part in (".", "..") for part in entry.split("/"))
        for entry in entries
    ):
        raise ValueError(
            "modules.builtin entries must be canonical kernel/.../*.ko paths"
        )
    if len(entries) != len(set(entries)):
        raise ValueError("modules.builtin contains duplicate entries")
    return entries


def load_modules_builtin(path: pathlib.Path) -> tuple[bytes, str]:
    """Load Kbuild metadata and its exact sibling kernel release."""
    try:
        data = path.read_bytes()
    except OSError as error:
        raise ValueError(f"cannot read modules.builtin: {path}: {error}") from error
    validate_modules_builtin(data)
    release_path = path.parent / "include/config/kernel.release"
    try:
        release_text = release_path.read_text(encoding="utf-8")
    except OSError as error:
        raise ValueError(
            f"cannot read sibling kernel release: {release_path}: {error}"
        ) from error
    release_lines = release_text.splitlines()
    if (
        len(release_lines) != 1
        or not KERNEL_RELEASE_VALUE.fullmatch(release_lines[0])
    ):
        raise ValueError(f"invalid sibling kernel release file: {release_path}")
    return data, release_lines[0]


def ltp_builtin_driver_metadata_setup(
    modules_builtin_data: bytes,
    expected_kernel_release: str,
    modules_root: str = "/lib/modules",
    kernel_config: str = "/proc/config.gz",
    config_reader: tuple[str, ...] = ("zcat",),
    release_reader: tuple[str, ...] = ("uname", "-r"),
    base64_decoder: tuple[str, ...] = ("base64", "-d"),
    tools_root: str = LTP_DISPOSABLE_TOOLS,
) -> str:
    """Expose authoritative Kbuild built-in metadata in disposable state.

    ``TST_NEEDS_DRIVERS`` searches ``modules.dep`` and ``modules.builtin`` for
    ``/<driver>.ko``. The complete metadata comes from the exact kernel build,
    and is accepted only when its sibling release matches the running guest.
    A fail-closed kmod shim recognizes only exact built-in names when loadable
    modules are disabled; unsupported operations and parameters fail.
    """
    entries = validate_modules_builtin(modules_builtin_data)
    module_names = tuple(
        dict.fromkeys(
            entry.rsplit("/", 1)[-1].removesuffix(".ko")
            for entry in entries
        )
    )
    if not KERNEL_RELEASE_VALUE.fullmatch(expected_kernel_release):
        raise ValueError("expected kernel release is invalid")
    for name, command in (
        ("config reader", config_reader),
        ("release reader", release_reader),
        ("base64 decoder", base64_decoder),
    ):
        if not command:
            raise ValueError(f"{name} command must be nonempty")
    reader = " ".join(shlex.quote(argument) for argument in config_reader)
    release_command = " ".join(
        shlex.quote(argument) for argument in release_reader
    )
    decoder = " ".join(shlex.quote(argument) for argument in base64_decoder)
    payload = base64.b64encode(modules_builtin_data).decode("ascii")
    payload_chunks = tuple(
        payload[offset : offset + 256]
        for offset in range(0, len(payload), 256)
    )
    lines = [
        "(",
        "set -eu",
        f"modules_root={shlex.quote(modules_root)}",
        f"expected_kernel_release={shlex.quote(expected_kernel_release)}",
        f"kernel_release=$({release_command})",
        'if [ "$kernel_release" != "$expected_kernel_release" ]; then',
        "printf '%s\\n' \"kernel release mismatch: expected "
        '$expected_kernel_release, running $kernel_release" >&2',
        "exit 1",
        "fi",
        f"kernel_config={shlex.quote(kernel_config)}",
        f"tools_root={shlex.quote(tools_root)}",
        'modules_dir="$modules_root/$kernel_release"',
        'modules_dep="$modules_dir/modules.dep"',
        'modules_builtin="$modules_dir/modules.builtin"',
        f'kernel_config_text=$({reader} "$kernel_config")',
        "modules_unset_count=$(printf '%s\\n' \"$kernel_config_text\" | "
        "grep -cx '# CONFIG_MODULES is not set' || true)",
        "modules_set_count=$(printf '%s\\n' \"$kernel_config_text\" | "
        "grep -c '^CONFIG_MODULES=' || true)",
        'if [ "$modules_unset_count" -ne 1 ] || '
        '[ "$modules_set_count" -ne 0 ]; then',
        "printf '%s\\n' 'LTP static-kmod overlay requires CONFIG_MODULES=n' >&2",
        "exit 1",
        "fi",
        'if [ -L "$modules_root" ] || [ -L "$modules_dir" ]; then',
        "printf '%s\\n' 'refusing symlinked LTP module metadata directory' >&2",
        "exit 1",
        "fi",
        'mkdir -p "$modules_dir"',
        'modules_builtin_tmp="$modules_dir/.modules.builtin.uml-ltp.$$"',
        'modules_dep_tmp="$modules_dir/.modules.dep.uml-ltp.$$"',
        'kmod_tmp="$tools_root/.kmod.uml-ltp.$$"',
        "trap 'rm -f \"$modules_builtin_tmp\" \"$modules_dep_tmp\" "
        "\"$kmod_tmp\"' 0 1 2 15",
        "{",
    ]
    lines.extend(
        f"printf '%s\\n' {shlex.quote(chunk)}" for chunk in payload_chunks
    )
    lines.extend([
        f'}} | {decoder} > "$modules_builtin_tmp"',
        'chmod 0644 "$modules_builtin_tmp"',
        ': > "$modules_dep_tmp"',
        'chmod 0644 "$modules_dep_tmp"',
        'mv -f "$modules_builtin_tmp" "$modules_builtin"',
        'mv -f "$modules_dep_tmp" "$modules_dep"',
    ])
    shim_lines = (
        "#!/bin/sh",
        "set -eu",
        f"modules_root={shlex.quote(modules_root)}",
        f"kernel_release={shlex.quote(expected_kernel_release)}",
        f"kernel_config={shlex.quote(kernel_config)}",
        f'kernel_config_text=$({reader} "$kernel_config")',
        "modules_unset_count=$(printf '%s\\n' \"$kernel_config_text\" | "
        "grep -cx '# CONFIG_MODULES is not set' || true)",
        "modules_set_count=$(printf '%s\\n' \"$kernel_config_text\" | "
        "grep -c '^CONFIG_MODULES=' || true)",
        'if [ "$modules_unset_count" -ne 1 ] || '
        '[ "$modules_set_count" -ne 0 ]; then',
        "exit 1",
        "fi",
        'modules_builtin="$modules_root/$kernel_release/modules.builtin"',
        '[ -r "$modules_builtin" ] || exit 1',
        'tool=${0##*/}',
        'if [ "$tool" = kmod ]; then',
        '[ "${1-}" = modprobe ] || exit 1',
        "shift",
        "tool=modprobe",
        "fi",
        '[ "$tool" = modprobe ] || exit 1',
        "load_all=0",
        'case "${1-}" in',
        "-a|--all) load_all=1; shift ;;",
        "-r|--remove) exit 1 ;;",
        "-*) exit 1 ;;",
        "esac",
        '[ "$#" -gt 0 ] || exit 1',
        '[ "$load_all" -eq 1 ] || [ "$#" -eq 1 ] || exit 1',
        "for driver do",
        'case "$driver" in',
        "''|-*|*[!A-Za-z0-9._+-]*) exit 1 ;;",
        "esac",
        'case "$driver" in',
    ) + tuple(
        f"{shlex.quote(module_name)}) ;;" for module_name in module_names
    ) + (
        "*) exit 1 ;;",
        "esac",
        "done",
        "exit 0",
    )
    lines.extend((
        'if [ -L "$tools_root" ]; then',
        "printf '%s\\n' 'refusing symlinked LTP tools directory' >&2",
        "exit 1",
        "fi",
        'mkdir -p "$tools_root"',
        'chmod 0755 "$tools_root"',
        'kmod_tool="$tools_root/kmod"',
        "{",
    ))
    lines.extend(
        f"printf '%s\\n' {shlex.quote(line)}" for line in shim_lines
    )
    lines.extend((
        '} > "$kmod_tmp"',
        'chmod 0755 "$kmod_tmp"',
        'mv -f "$kmod_tmp" "$kmod_tool"',
        'ln -sf kmod "$tools_root/modprobe"',
        'ln -sf kmod "$tools_root/rmmod"',
        ")",
    ))
    return "\n".join(lines)


@dataclasses.dataclass(frozen=True)
class MetadataTransport:
    initialize: str
    append_chunks: tuple[str, ...]
    execute: str
    cleanup: str


def ltp_metadata_transport(
    setup_script: str,
    transport_root: str = LTP_METADATA_TRANSPORT_ROOT,
    chunk_chars: int = LTP_METADATA_TRANSPORT_CHUNK_CHARS,
) -> MetadataTransport:
    """Build bounded, acknowledged commands for one setup script."""
    if not setup_script:
        raise ValueError("LTP metadata setup script must be nonempty")
    if chunk_chars < 1:
        raise ValueError("LTP metadata transport chunk size must be positive")
    compressed = gzip.compress(setup_script.encode("utf-8"))
    payload = base64.b64encode(compressed).decode("ascii")
    chunks = tuple(
        payload[offset : offset + chunk_chars]
        for offset in range(0, len(payload), chunk_chars)
    )
    root = shlex.quote(transport_root)
    initialize = (
        f"umask 077 && rm -rf -- {root} && mkdir {root} && "
        f": > {root}/setup.sh.gz.b64"
    )
    append_chunks = tuple(
        f"printf '%s' {shlex.quote(chunk)} >> {root}/setup.sh.gz.b64"
        for chunk in chunks
    )
    execute = " ".join((
        "(",
        "set -eu;",
        f"transport={root};",
        'payload="$transport/setup.sh.gz.b64";',
        'compressed="$transport/setup.sh.gz";',
        'script="$transport/setup.sh";',
        "trap 'rm -rf -- \"$transport\"' 0 1 2 15;",
        'if ! base64 -d < "$payload" > "$compressed.tmp"; then',
        "echo 'LTP metadata transport decode failed' >&2; exit 1; fi;",
        'mv -f "$compressed.tmp" "$compressed";',
        'if ! gzip -dc "$compressed" > "$script.tmp"; then',
        "echo 'LTP metadata transport decompress failed' >&2; exit 1; fi;",
        'chmod 0700 "$script.tmp";',
        'mv -f "$script.tmp" "$script";',
        'if /bin/sh "$script"; then :; else',
        "rc=$?; echo 'LTP metadata setup script failed' >&2; exit \"$rc\"; fi;",
        ")",
    ))
    cleanup = f"rm -rf -- {root} && test ! -e {root}"
    commands = (initialize, *append_chunks, execute, cleanup)
    if any(
        len(command.encode("utf-8")) > LTP_METADATA_TRANSPORT_MAX_COMMAND_BYTES
        for command in commands
    ):
        raise ValueError("LTP metadata transport command exceeds safe PTY size")
    return MetadataTransport(initialize, append_chunks, execute, cleanup)


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
    engine: str = "jit"

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


def ltp_guest_command(command: str, tmpdir: str = "/tmp/uml-ltp-case") -> str:
    """Build the command environment used by LTP's scenario executor."""
    return (
        f"mkdir -p {tmpdir} {LTP_OUTPUT} && "
        f"chmod 1777 {tmpdir} {LTP_OUTPUT} && "
        f"cd {LTP_BIN} && "
        f"PATH={LTP_DISPOSABLE_TOOLS}:{LTP_BIN}:$PATH "
        f"LTPROOT={LTP_ROOT} "
        f"TMPDIR={tmpdir} "
        f"LTP_TIMEOUT_MUL={LTP_TIMEOUT_MUL} "
        f"sh -c {shlex.quote(command)}"
    )


class Guest(GuestHarness):
    """Shared guest harness with LTP result and health interpretation."""

    builtin_driver_metadata_setup: str | None = None

    def _run_metadata_transport_command(
        self,
        command: str,
        stage: str,
        timeout_seconds: int,
    ) -> None:
        try:
            result = self.run_command(command, timeout_seconds)
        except Exception as error:
            raise RuntimeError(
                f"LTP metadata {stage} was not acknowledged: {error}"
            ) from error
        if result.return_code != 0:
            raise RuntimeError(
                f"LTP metadata {stage} failed: {result.output}"
            )

    def boot_and_login(
        self, timeout_seconds: int = BOOT_TIMEOUT_SECONDS
    ) -> None:
        super().boot_and_login(timeout_seconds)
        if not self._overlay_shell:
            return
        if self.builtin_driver_metadata_setup is None:
            raise RuntimeError("LTP built-in driver metadata was not configured")
        transport = ltp_metadata_transport(
            self.builtin_driver_metadata_setup
        )
        primary_error: Exception | None = None
        finalization_errors: list[Exception] = []
        restore_echo = False
        cleanup_transport = False
        try:
            restore_echo = True
            self._run_metadata_transport_command(
                "stty -echo", "console-echo disable", timeout_seconds
            )
            cleanup_transport = True
            self._run_metadata_transport_command(
                transport.initialize, "transport initialization", timeout_seconds
            )
            total_chunks = len(transport.append_chunks)
            for index, command in enumerate(transport.append_chunks, start=1):
                self._run_metadata_transport_command(
                    command,
                    f"transport chunk {index}/{total_chunks}",
                    timeout_seconds,
                )
            self._run_metadata_transport_command(
                transport.execute,
                "decode/decompress/setup execution",
                timeout_seconds,
            )
        except Exception as error:
            primary_error = error

        if cleanup_transport:
            try:
                self._run_metadata_transport_command(
                    transport.cleanup, "transport cleanup", timeout_seconds
                )
            except Exception as error:
                finalization_errors.append(error)
        if restore_echo:
            try:
                self._run_metadata_transport_command(
                    "stty echo", "console-echo restore", timeout_seconds
                )
            except Exception as error:
                finalization_errors.append(error)

        if primary_error is not None:
            if finalization_errors:
                details = "; ".join(str(error) for error in finalization_errors)
                raise RuntimeError(
                    f"{primary_error}; metadata finalization also failed: {details}"
                ) from primary_error
            raise primary_error
        if finalization_errors:
            details = "; ".join(str(error) for error in finalization_errors)
            raise RuntimeError(f"LTP metadata finalization failed: {details}")

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
            cgroup2_nsdelegate=case.cgroup2_nsdelegate, overlay_shell=True,
            engine=case.engine,
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
        clean_pass = (
            result.return_code == 0
            and result.summary.get("passed", 0) > 0
            and failed == 0
            and broken == 0
            and skipped == 0
            and result.summary.get("warnings", 0) == 0
        )
        warning_only = (
            result.return_code == 4
            and result.summary.get("passed", 0) > 0
            and failed == 0
            and broken == 0
            and skipped == 0
            and result.summary.get("warnings", 0) > 0
        )
        mixed_result = (
            result.return_code != 0
            and result.summary.get("passed", 0) > 0
            and (failed > 0 or broken > 0)
        )
        return clean_pass or warning_only or mixed_result
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
        return result.return_code in (0, 32)
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
        "--modules-builtin",
        required=True,
        type=pathlib.Path,
        help=(
            "authoritative modules.builtin from the exact kernel build; "
            "include/config/kernel.release must be present beside it"
        ),
    )
    parser.add_argument(
        "--engine",
        choices=("gadgets", "jit"),
        default="jit",
        help="userspace execution engine for every guest (default: jit)",
    )
    parser.add_argument(
        "--results-dir",
        type=pathlib.Path,
        help="write raw per-case output and a machine-readable campaign summary",
    )
    parser.add_argument(
        "--disabled-manifest",
        type=pathlib.Path,
        default=pathlib.Path(__file__).with_name("disabled-tests.tsv"),
        help=(
            "cases intentionally omitted for this emulator "
            "(default: tests/integration/ltp/disabled-tests.tsv)"
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
        arguments.modules_builtin,
    ):
        if not path.is_file():
            print(f"missing required file: {path}", file=sys.stderr)
            return 2
    try:
        modules_builtin_data, expected_kernel_release = load_modules_builtin(
            arguments.modules_builtin
        )
        Guest.builtin_driver_metadata_setup = ltp_builtin_driver_metadata_setup(
            modules_builtin_data, expected_kernel_release
        )
    except ValueError as error:
        print(str(error), file=sys.stderr)
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
    cases = [dataclasses.replace(case, engine=arguments.engine) for case in cases]
    if skipped_pressure_count:
        print(
            f"Skipping {skipped_pressure_count} host-pressure cases by default; "
            "pass --run-host-pressure to include them",
            flush=True,
        )

    failures = 0
    failed_cases: list[Result] = []
    completed = 0
    run_started = time.monotonic()
    results_directory = (
        arguments.results_dir.resolve() if arguments.results_dir else None
    )
    campaign_records: list[dict[str, object]] = []
    if results_directory is not None:
        results_directory.mkdir(parents=True, exist_ok=True)
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
        result_accepted = accepted(result)
        outcome = "PASS" if result_accepted else "FAIL"
        campaign_records.append(
            {
                "case": case.key,
                "engine": arguments.engine,
                "accepted": result_accepted,
                "return_code": result.return_code,
                "summary": result.summary,
                "error": result.error,
            }
        )
        if results_directory is not None:
            artifact_name = case.key.replace("/", "__") + ".log"
            (results_directory / artifact_name).write_text(
                result.output, encoding="utf-8"
            )
        if outcome == "FAIL" and arguments.quiet and result.output:
            print(result.output)
        print(
            f"{outcome} {case.key}: rc={result.return_code} "
            f"summary={result.summary}",
            flush=True,
        )
        failures += outcome == "FAIL"
        if outcome == "FAIL":
            failed_cases.append(result)
        if outcome == "FAIL" and arguments.fail_fast:
            print("Stopping after first failure (--fail-fast)", flush=True)
            break
    if failed_cases:
        print(
            "Rejected cases: "
            + ", ".join(result.case.key for result in failed_cases),
            flush=True,
        )
        for result in failed_cases:
            detail = (
                f"rc={result.return_code} summary={result.summary}"
            )
            if result.error:
                detail += f" error={result.error}"
            print(
                f"Rejected case detail: {result.case.key}: {detail}",
                flush=True,
            )
    print(
        f"Completed {completed} cases: {completed - failures} accepted, "
        f"{failures} failed in {time.monotonic() - run_started:.1f}s",
        flush=True,
    )
    if results_directory is not None:
        campaign_summary = {
            "schema_version": 1,
            "engine": arguments.engine,
            "elapsed_seconds": time.monotonic() - run_started,
            "completed": completed,
            "failures": failures,
            "cases": campaign_records,
        }
        (results_directory / "summary.json").write_text(
            json.dumps(campaign_summary, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
