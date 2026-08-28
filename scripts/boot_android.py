#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot the accepted Android guest on macOS and manage its ADB transport."""

from __future__ import annotations

# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
from pathlib import Path
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import time
from typing import NoReturn, Sequence


REPO_ROOT = Path(__file__).resolve().parents[1]
ACCEPTED_RELEASE = (
    REPO_ROOT / "android/aosp-17/metadata/accepted-release.env"
)
RELEASES_ROOT = REPO_ROOT / ".build/android/releases"
INSTANCES_ROOT = REPO_ROOT / ".build/android/instances"
DEFAULT_EMULATOR = (
    REPO_ROOT / ".build/presets/macos-android17/bin/uml-darwin-android17"
)
SYSTEM_TRUNCATE = Path("/usr/bin/truncate")
HOMEBREW_SGDISK_CANDIDATES = (
    Path("/opt/homebrew/bin/sgdisk"),
    Path("/usr/local/bin/sgdisk"),
)
USERDATA_PARTITION_NUMBER = 10
USERDATA_FIRST_LBA = 3_796_992
USERDATA_TYPE_GUID = "0FC63DAF-8483-4772-8E79-3D69D8477DE4"
USERDATA_UNIQUE_GUID = "523ABB36-1F98-5D08-92D0-31C3C8A47304"
GPT_TRAILING_FREE_BYTES = 1024**2
GPT_LOGICAL_BLOCK_BYTES = 512
DEFAULT_ADB_PORT = 5555
GUEST_ADB_PORT = 5555
DEFAULT_DISPLAY_WIDTH = 720
DEFAULT_DISPLAY_HEIGHT = 1280
MINIMUM_DISK_SIZE_GIB = 4
MAXIMUM_DISK_SIZE_GIB = 64
DEFAULT_DISK_SIZE_GIB = 8


class LauncherError(RuntimeError):
    """A user-facing launcher failure."""


class RequestedShutdown(Exception):
    def __init__(self, signum: int) -> None:
        super().__init__(signum)
        self.signum = signum


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


def disk_capacity(value: str) -> int:
    parsed = positive_integer(value)
    if not MINIMUM_DISK_SIZE_GIB <= parsed <= MAXIMUM_DISK_SIZE_GIB:
        raise argparse.ArgumentTypeError(
            f"must be between {MINIMUM_DISK_SIZE_GIB} and "
            f"{MAXIMUM_DISK_SIZE_GIB} GiB"
        )
    return parsed


def parse_arguments(arguments: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "boot the accepted Android 17 image on macOS, wait for "
            "ADB, and disconnect ADB when the guest exits"
        )
    )
    parser.add_argument(
        "--release",
        type=Path,
        help="installed Android release (default: accepted release)",
    )
    parser.add_argument(
        "--instance",
        type=Path,
        help=(
            "persistent writable COW (default: a release-specific file below "
            ".build/android/instances)"
        ),
    )
    parser.add_argument(
        "--emulator",
        type=Path,
        default=DEFAULT_EMULATOR,
        help="Android macOS executable produced by the macos-android17 preset",
    )
    parser.add_argument(
        "--adb",
        default="adb",
        help="ADB executable name or path (default: adb from PATH or Android SDK)",
    )
    parser.add_argument("--adb-port", type=port_number, default=DEFAULT_ADB_PORT)
    parser.add_argument(
        "--adb-timeout",
        type=positive_integer,
        default=300,
        help="seconds to wait for the guest ADB daemon (default: 300)",
    )
    parser.add_argument("--cpus", type=positive_integer, default=10)
    parser.add_argument("--memory", default="8G")
    parser.add_argument(
        "--disk-size-gib",
        type=disk_capacity,
        default=DEFAULT_DISK_SIZE_GIB,
        help="expanded Android disk capacity in GiB (default: 8)",
    )
    parser.add_argument(
        "--width",
        type=positive_integer,
        default=DEFAULT_DISPLAY_WIDTH,
        help=f"guest display width in pixels (default: {DEFAULT_DISPLAY_WIDTH})",
    )
    parser.add_argument(
        "--height",
        type=positive_integer,
        default=DEFAULT_DISPLAY_HEIGHT,
        help=f"guest display height in pixels (default: {DEFAULT_DISPLAY_HEIGHT})",
    )
    parser.add_argument(
        "--engine",
        choices=("gadgets", "jit", "interpreter"),
        default="gadgets",
    )
    boot_mode = parser.add_mutually_exclusive_group()
    boot_mode.add_argument(
        "--boot-mode", choices=("auto", "normal", "recovery"), default="normal"
    )
    boot_mode.add_argument(
        "--recovery",
        action="store_const",
        const="recovery",
        dest="boot_mode",
        help="boot the accepted release into recovery",
    )
    parser.add_argument(
        "--build",
        action="store_true",
        help="incrementally rebuild the macos-android17 executable before boot",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="print build, emulator, and ADB commands without changing state",
    )
    options = parser.parse_args(arguments)
    if not options.memory or any(character.isspace() for character in options.memory):
        parser.error("--memory must be one kernel argument without whitespace")
    return options


def accepted_directory(descriptor: Path = ACCEPTED_RELEASE) -> str:
    try:
        lines = descriptor.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise LauncherError(
            f"cannot read accepted Android release descriptor {descriptor}: {error}"
        ) from error
    key = "ANDROID17_ACCEPTED_DIRECTORY_NAME"
    for line in lines:
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        name, separator, encoded = stripped.partition("=")
        if separator != "=" or name != key:
            continue
        try:
            values = shlex.split(encoded, posix=True)
        except ValueError as error:
            raise LauncherError(f"invalid {key} in {descriptor}: {error}") from error
        if len(values) != 1:
            raise LauncherError(f"invalid {key} in {descriptor}")
        value = values[0]
        if value in ("", ".", "..") or Path(value).name != value:
            raise LauncherError(f"unsafe {key} in {descriptor}")
        return value
    raise LauncherError(f"{descriptor} does not define {key}")


def release_path(
    requested: Path | None,
    *,
    descriptor: Path = ACCEPTED_RELEASE,
    releases_root: Path = RELEASES_ROOT,
) -> Path:
    if requested is None:
        candidate = releases_root / accepted_directory(descriptor)
    else:
        candidate = requested.expanduser()
        if not candidate.is_absolute():
            candidate = Path.cwd() / candidate
    try:
        release = candidate.resolve(strict=True)
    except OSError as error:
        raise LauncherError(f"Android release is unavailable: {candidate}: {error}") from error
    if not release.is_dir():
        raise LauncherError(f"Android release is not a directory: {release}")
    required = (
        release / "release-manifest.json",
        release / "boot/kernel-command-line.txt",
        release / "guest/android-17-uml-arm64.img",
    )
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        raise LauncherError("Android release is incomplete: " + ", ".join(missing))
    return release


def instance_path(requested: Path | None, release: Path) -> Path:
    candidate = (
        INSTANCES_ROOT / f"{release.name}.cow"
        if requested is None
        else requested.expanduser()
    )
    if not candidate.is_absolute():
        candidate = Path.cwd() / candidate
    return candidate.resolve(strict=False)


def instance_base_path(instance: Path) -> Path:
    return instance.with_suffix(".base.img")


def prepare_instance_base(release: Path, instance: Path, base: Path) -> None:
    source = release / "guest/android-17-uml-arm64.img"
    if base.exists():
        if base.is_symlink() or not base.is_file() or not os.access(base, os.W_OK):
            raise LauncherError(f"Android instance base is unsafe: {base}")
        return
    if instance.exists():
        raise LauncherError(
            f"Android instance has no matching mutable base: {instance}"
        )
    base.parent.mkdir(parents=True, exist_ok=True)
    temporary = base.with_name(base.name + ".tmp")
    if temporary.exists() or temporary.is_symlink():
        raise LauncherError(f"stale Android base staging path: {temporary}")
    clone_file(source, temporary)
    temporary.chmod(temporary.stat().st_mode | 0o200)
    temporary.replace(base)


def clone_file(source: Path, destination: Path) -> None:
    cloned = subprocess.run(
        ["cp", "-c", str(source), str(destination)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    if cloned.returncode != 0:
        if destination.exists():
            destination.unlink()
        shutil.copyfile(source, destination)


def homebrew_sgdisk(
    candidates: Sequence[Path] = HOMEBREW_SGDISK_CANDIDATES,
) -> Path:
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate.resolve()
    raise LauncherError(
        "Homebrew GPT fdisk is unavailable; install it with "
        "`brew install gptfdisk`"
    )


def run_sgdisk(
    sgdisk: Path, *arguments: str | Path
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(sgdisk), *(str(argument) for argument in arguments)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )


def gpt_layout_matches(sgdisk: Path, image: Path, capacity_bytes: int) -> bool:
    info = run_sgdisk(sgdisk, "--info=10", image)
    if info.returncode != 0:
        return False
    expected_disk_lbas = capacity_bytes // GPT_LOGICAL_BLOCK_BYTES
    expected_last_lba = (
        expected_disk_lbas
        - GPT_TRAILING_FREE_BYTES // GPT_LOGICAL_BLOCK_BYTES
        - 1
    )
    required = (
        f"Partition GUID code: {USERDATA_TYPE_GUID}",
        f"Partition unique GUID: {USERDATA_UNIQUE_GUID}",
        f"First sector: {USERDATA_FIRST_LBA} ",
        f"Last sector: {expected_last_lba} ",
        "Partition name: 'userdata'",
    )
    if any(value not in info.stdout for value in required):
        return False
    verified = run_sgdisk(sgdisk, "--verify", image)
    return verified.returncode == 0 and "No problems found." in verified.stdout


def resize_instance_base(
    base: Path,
    instance: Path,
    capacity_gib: int,
    *,
    truncate: Path = SYSTEM_TRUNCATE,
    sgdisk: Path | None = None,
) -> bool:
    requested = capacity_gib * 1024**3
    current = base.stat().st_size
    selected_sgdisk = homebrew_sgdisk() if sgdisk is None else sgdisk
    if not selected_sgdisk.is_file() or not os.access(selected_sgdisk, os.X_OK):
        raise LauncherError(f"Homebrew sgdisk is unavailable: {selected_sgdisk}")
    if current == requested and gpt_layout_matches(
        selected_sgdisk, base, requested
    ):
        return False
    if instance.exists():
        raise LauncherError(
            "Android disk capacity cannot change after the instance was created"
        )
    if current > requested:
        raise LauncherError(
            f"Android base disk cannot shrink from {current} to {requested} bytes"
        )
    if not truncate.is_file() or not os.access(truncate, os.X_OK):
        raise LauncherError(f"macOS truncate utility is unavailable: {truncate}")
    temporary = base.with_name(base.name + ".resize.tmp")
    if temporary.exists() or temporary.is_symlink():
        raise LauncherError(f"stale Android disk resize path: {temporary}")
    try:
        clone_file(base, temporary)
        subprocess.run(
            [str(truncate), "-s", str(requested), str(temporary)],
            check=True,
        )
        actual = temporary.stat().st_size
        if actual != requested:
            raise LauncherError(
                "Android base disk resize produced "
                f"{actual}, expected {requested} bytes"
            )
        disk_lbas = requested // GPT_LOGICAL_BLOCK_BYTES
        userdata_last_lba = (
            disk_lbas
            - GPT_TRAILING_FREE_BYTES // GPT_LOGICAL_BLOCK_BYTES
            - 1
        )
        mutation = run_sgdisk(
            selected_sgdisk,
            "--move-second-header",
            f"--delete={USERDATA_PARTITION_NUMBER}",
            f"--new={USERDATA_PARTITION_NUMBER}:{USERDATA_FIRST_LBA}:"
            f"{userdata_last_lba}",
            f"--typecode={USERDATA_PARTITION_NUMBER}:8300",
            f"--partition-guid={USERDATA_PARTITION_NUMBER}:"
            f"{USERDATA_UNIQUE_GUID}",
            f"--change-name={USERDATA_PARTITION_NUMBER}:userdata",
            temporary,
        )
        if mutation.returncode != 0:
            detail = mutation.stderr.strip() or mutation.stdout.strip()
            raise LauncherError(f"Homebrew sgdisk could not resize GPT: {detail}")
        if not gpt_layout_matches(selected_sgdisk, temporary, requested):
            raise LauncherError("Homebrew sgdisk produced an invalid Android GPT")
        temporary.replace(base)
    finally:
        if temporary.exists():
            temporary.unlink()
    return True


def resolve_executable(value: str | Path, *, sdk_fallback: bool = False) -> Path:
    text = os.fspath(value)
    expanded = Path(text).expanduser()
    if expanded.parent != Path(".") or expanded.is_absolute():
        candidate = expanded if expanded.is_absolute() else Path.cwd() / expanded
        resolved = candidate.resolve(strict=False)
        if resolved.is_file() and os.access(resolved, os.X_OK):
            return resolved
        raise LauncherError(f"executable is unavailable: {resolved}")
    discovered = shutil.which(text)
    if discovered is not None:
        return Path(discovered).resolve()
    if sdk_fallback:
        fallback = Path.home() / "Library/Android/sdk/platform-tools/adb"
        if fallback.is_file() and os.access(fallback, os.X_OK):
            return fallback.resolve()
    raise LauncherError(f"cannot locate executable: {text}")


def build_commands(cmake: Path) -> list[list[str]]:
    return [
        [str(cmake), "--preset", "macos-android17"],
        [str(cmake), "--build", "--preset", "macos-android17"],
    ]


def emulator_command(
    options: argparse.Namespace,
    release: Path,
    instance: Path,
    emulator: Path,
    base: Path | None = None,
) -> list[str]:
    if base is None:
        base = instance_base_path(instance)
    return [
        str(emulator),
        f"--android-release={release}",
        f"--android-instance={instance}",
        f"--android-base-disk={base}",
        f"--android-boot-mode={options.boot_mode}",
        f"--net-forward=tcp:127.0.0.1:{options.adb_port}-{GUEST_ADB_PORT}",
        "--gpu",
        "--gpu-display=native",
        f"--gpu-width={options.width}",
        f"--gpu-height={options.height}",
        f"ncpus={options.cpus}",
        f"mem={options.memory}",
        f"--execution-engine={options.engine}",
    ]


def ensure_loopback_port_available(port: int) -> None:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            probe.bind(("127.0.0.1", port))
        except OSError as error:
            raise LauncherError(
                f"ADB forwarding port 127.0.0.1:{port} is unavailable: {error}"
            ) from error


def disconnect_adb(adb: Path, serial: str) -> None:
    try:
        subprocess.run(
            [str(adb), "disconnect", serial],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=10,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        pass


def wait_for_adb(
    adb: Path,
    serial: str,
    guest: subprocess.Popen[bytes],
    timeout: int,
) -> None:
    deadline = time.monotonic() + timeout
    last_response = ""
    while time.monotonic() < deadline:
        result = guest.poll()
        if result is not None:
            raise LauncherError(
                f"Android guest exited with status {result} before ADB became ready"
            )
        try:
            connection = subprocess.run(
                [str(adb), "connect", serial],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=10,
                check=False,
            )
            last_response = connection.stdout.strip()
            state = subprocess.run(
                [str(adb), "-s", serial, "get-state"],
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                text=True,
                timeout=10,
                check=False,
            )
            if state.returncode == 0 and state.stdout.strip() == "device":
                return
        except subprocess.TimeoutExpired:
            last_response = "ADB command timed out"
        time.sleep(1)
    detail = f": {last_response}" if last_response else ""
    raise LauncherError(f"ADB did not become ready within {timeout} seconds{detail}")


def stop_guest(guest: subprocess.Popen[bytes]) -> None:
    if guest.poll() is not None:
        return
    stages = ((signal.SIGINT, 10), (signal.SIGTERM, 5))
    for signum, timeout in stages:
        try:
            os.killpg(guest.pid, signum)
            guest.wait(timeout=timeout)
            return
        except ProcessLookupError:
            return
        except subprocess.TimeoutExpired:
            continue
    try:
        os.killpg(guest.pid, signal.SIGKILL)
    except ProcessLookupError:
        return
    guest.wait()


def install_shutdown_handlers() -> None:
    def request_shutdown(signum: int, _frame: object) -> NoReturn:
        raise RequestedShutdown(signum)

    signal.signal(signal.SIGTERM, request_shutdown)
    signal.signal(signal.SIGHUP, request_shutdown)


def main(arguments: Sequence[str] | None = None) -> int:
    options = parse_arguments(arguments)
    try:
        release = release_path(options.release)
        instance = instance_path(options.instance, release)
        base = instance_base_path(instance)
        adb = resolve_executable(options.adb, sdk_fallback=True)
        commands: list[list[str]] = []
        if options.build:
            commands = build_commands(resolve_executable("cmake"))
        guest_command = emulator_command(
            options, release, instance, options.emulator, base
        )
        serial = f"127.0.0.1:{options.adb_port}"
        if options.dry_run:
            for command in commands:
                print(shlex.join(command))
            print(shlex.join(guest_command))
            print(shlex.join([str(adb), "connect", serial]))
            print(shlex.join([str(adb), "disconnect", serial]))
            return 0
        if sys.platform != "darwin":
            raise LauncherError("this launcher supports macOS only")
        for command in commands:
            subprocess.run(command, cwd=REPO_ROOT, check=True)
        emulator = resolve_executable(options.emulator)
        prepare_instance_base(release, instance, base)
        resize_instance_base(base, instance, options.disk_size_gib)
        guest_command = emulator_command(options, release, instance, emulator, base)
        instance.parent.mkdir(parents=True, exist_ok=True)
        ensure_loopback_port_available(options.adb_port)
        disconnect_adb(adb, serial)
        print(f"Booting accepted Android release {release.name}.", file=sys.stderr)
        print(f"Persistent instance: {instance}", file=sys.stderr)
        print(
            f"Mutable base: {base} ({options.disk_size_gib} GiB)",
            file=sys.stderr,
        )
        print("Native Metal display; press Ctrl-C to stop the guest.", file=sys.stderr)
        guest = subprocess.Popen(guest_command, start_new_session=True)
        install_shutdown_handlers()
        try:
            wait_for_adb(adb, serial, guest, options.adb_timeout)
            print(f"ADB connected: {adb} -s {serial} shell", file=sys.stderr)
            return guest.wait()
        except KeyboardInterrupt:
            return 130
        except RequestedShutdown as shutdown:
            return 128 + shutdown.signum
        finally:
            stop_guest(guest)
            disconnect_adb(adb, serial)
            print(f"ADB disconnected: {serial}", file=sys.stderr)
    except (LauncherError, OSError, subprocess.CalledProcessError) as error:
        print(f"boot_android.py: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
