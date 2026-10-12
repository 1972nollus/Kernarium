#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot the accepted Android guest on macOS and manage its ADB transport."""

from __future__ import annotations

# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import json
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
_ARTIFACTS = str(REPO_ROOT / "kernel/common/artifacts")
if _ARTIFACTS not in sys.path:
    sys.path.insert(0, _ARTIFACTS)
from contract_checks import accepted_directory_name  # noqa: E402
_GUEST_SUPPORT = str(REPO_ROOT / "tests/integration/support")
if _GUEST_SUPPORT not in sys.path:
    sys.path.insert(0, _GUEST_SUPPORT)
from guest import remove_emulator_run_directories  # noqa: E402

ACCEPTED_RELEASE = (
    REPO_ROOT / "android/lineage-24/metadata/accepted-release.env"
)
RELEASES_ROOT = REPO_ROOT / ".build/android/releases"
INSTANCES_ROOT = REPO_ROOT / ".build/android/instances"
DEFAULT_EMULATOR = (
    REPO_ROOT / ".build/presets/macos-android17/bin/uml-darwin-android17"
)
SYSTEM_TRUNCATE = Path("/usr/bin/truncate")
# Where Homebrew installs GPT fdisk, tried after PATH.
SGDISK_FALLBACK_CANDIDATES = (
    Path("/opt/homebrew/bin/sgdisk"),
    Path("/usr/local/bin/sgdisk"),
)
USERDATA_PARTITION_NUMBER = 10
USERDATA_FIRST_LBA = 5_500_928
USERDATA_TYPE_GUID = "0FC63DAF-8483-4772-8E79-3D69D8477DE4"
USERDATA_UNIQUE_GUID = "523ABB36-1F98-5D08-92D0-31C3C8A47304"
GPT_TRAILING_FREE_BYTES = 1024**2
GPT_LOGICAL_BLOCK_BYTES = 512
DEFAULT_ADB_PORT = 5555
GUEST_ADB_PORT = 5555
DEFAULT_DISPLAY_WIDTH = 720
DEFAULT_DISPLAY_HEIGHT = 1280
DEFAULT_VNC_PORT = 5900
DEFAULT_DISK_SIZE_GIB = 8
# The signals that ask the emulator to stop, each with its grace period,
# before it is killed.
GUEST_STOP_STAGES = ((signal.SIGINT, 10), (signal.SIGTERM, 5))


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
        "--disk",
        type=Path,
        help=(
            "persistent writable GPT image (default: android-17.img below "
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
    parser.add_argument(
        "--sgdisk",
        help=(
            "GPT fdisk executable name or path "
            "(default: sgdisk from PATH, then the Homebrew locations)"
        ),
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
        "--display",
        choices=("native", "vnc"),
        default="native",
        help=(
            "native Metal window, or a loopback VNC server that scripted "
            "runs can observe and drive (default: native)"
        ),
    )
    parser.add_argument(
        "--vnc-port",
        type=port_number,
        default=DEFAULT_VNC_PORT,
        help=f"loopback VNC port for --display=vnc (default: {DEFAULT_VNC_PORT})",
    )
    parser.add_argument(
        "--engine",
        choices=("gadgets", "jit"),
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
    return accepted_directory_name(
        descriptor, "LINEAGE24_ACCEPTED_DIRECTORY_NAME", LauncherError
    )


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


def disk_path(requested: Path | None) -> Path:
    candidate = INSTANCES_ROOT / "android-17.img" if requested is None else requested.expanduser()
    if not candidate.is_absolute():
        candidate = Path.cwd() / candidate
    return candidate.resolve(strict=False)


def disk_release_path(disk: Path) -> Path:
    return disk.with_name(disk.name + ".release-id")


def release_id(release: Path) -> str:
    try:
        manifest = json.loads(
            (release / "release-manifest.json").read_text(encoding="utf-8")
        )
        value = manifest["release_id"]
    except (OSError, json.JSONDecodeError, KeyError) as error:
        raise LauncherError(f"cannot read Android release identity: {error}") from error
    if not isinstance(value, str) or not value or "\n" in value:
        raise LauncherError("Android release has an invalid release_id")
    return value


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


def find_sgdisk(
    candidates: Sequence[Path] = SGDISK_FALLBACK_CANDIDATES,
    search_path: str | None = None,
) -> Path:
    """Find GPT fdisk's sgdisk on PATH, then in the Homebrew locations."""
    discovered = shutil.which("sgdisk", path=search_path)
    if discovered is not None:
        return Path(discovered).resolve()
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate.resolve()
    raise LauncherError(
        "GPT fdisk (sgdisk) is unavailable; install gptfdisk, for example "
        "with `brew install gptfdisk`, or name it with --sgdisk"
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


def resize_android_disk(
    disk: Path,
    capacity_gib: int,
    *,
    truncate: Path = SYSTEM_TRUNCATE,
    sgdisk: Path | None = None,
) -> bool:
    requested = capacity_gib * 1024**3
    current = disk.stat().st_size
    selected_sgdisk = find_sgdisk() if sgdisk is None else sgdisk
    if not selected_sgdisk.is_file() or not os.access(selected_sgdisk, os.X_OK):
        raise LauncherError(f"sgdisk is unavailable: {selected_sgdisk}")
    if current == requested and gpt_layout_matches(
        selected_sgdisk, disk, requested
    ):
        return False
    if current > requested:
        raise LauncherError(
            f"Android disk cannot shrink from {current} to {requested} bytes"
        )
    if not truncate.is_file() or not os.access(truncate, os.X_OK):
        raise LauncherError(f"macOS truncate utility is unavailable: {truncate}")
    temporary = disk.with_name(disk.name + ".resize.tmp")
    if temporary.exists() or temporary.is_symlink():
        raise LauncherError(f"stale Android disk resize path: {temporary}")
    try:
        clone_file(disk, temporary)
        subprocess.run(
            [str(truncate), "-s", str(requested), str(temporary)],
            check=True,
        )
        actual = temporary.stat().st_size
        if actual != requested:
            raise LauncherError(
                "Android disk resize produced "
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
            raise LauncherError(f"sgdisk could not resize GPT: {detail}")
        if not gpt_layout_matches(selected_sgdisk, temporary, requested):
            raise LauncherError("sgdisk produced an invalid Android GPT")
        temporary.replace(disk)
    finally:
        if temporary.exists():
            temporary.unlink()
    return True


def prepare_persistent_disk(
    release: Path,
    disk: Path,
    *,
    truncate: Path = SYSTEM_TRUNCATE,
    sgdisk: Path | None = None,
) -> bool:
    source = release / "guest/android-17-uml-arm64.img"
    identity = release_id(release)
    identity_path = disk_release_path(disk)
    if disk.exists():
        if disk.is_symlink() or not disk.is_file() or not os.access(disk, os.W_OK):
            raise LauncherError(f"Android disk is unsafe: {disk}")
        try:
            recorded = identity_path.read_text(encoding="utf-8").strip()
        except FileNotFoundError:
            recorded = ""
        except OSError as error:
            raise LauncherError(
                f"cannot read Android disk release identity {identity_path}: {error}"
            ) from error
        if recorded == identity:
            selected_sgdisk = find_sgdisk() if sgdisk is None else sgdisk
            expected = DEFAULT_DISK_SIZE_GIB * 1024**3
            if disk.stat().st_size != expected or not gpt_layout_matches(
                selected_sgdisk, disk, expected
            ):
                raise LauncherError(
                    "persistent Android disk no longer has the expected 8 GiB GPT"
                )
            return False

    disk.parent.mkdir(parents=True, exist_ok=True)
    temporary = disk.with_name(disk.name + ".prepare.tmp")
    identity_temporary = identity_path.with_name(identity_path.name + ".tmp")
    if temporary.exists() or temporary.is_symlink():
        raise LauncherError(f"stale Android disk staging path: {temporary}")
    if identity_temporary.exists() or identity_temporary.is_symlink():
        raise LauncherError(
            f"stale Android disk identity staging path: {identity_temporary}"
        )
    try:
        clone_file(source, temporary)
        temporary.chmod(temporary.stat().st_mode | 0o200)
        resize_android_disk(
            temporary,
            DEFAULT_DISK_SIZE_GIB,
            truncate=truncate,
            sgdisk=sgdisk,
        )
        identity_temporary.write_text(identity + "\n", encoding="utf-8")
        temporary.replace(disk)
        identity_temporary.replace(identity_path)
    finally:
        temporary.unlink(missing_ok=True)
        identity_temporary.unlink(missing_ok=True)
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
    disk: Path,
    emulator: Path,
) -> list[str]:
    return [
        str(emulator),
        f"--android-release={release}",
        f"--android-disk={disk}",
        f"--android-boot-mode={options.boot_mode}",
        f"--net-forward=tcp:127.0.0.1:{options.adb_port}-{GUEST_ADB_PORT}",
        "--gpu",
        f"--gpu-display={options.display}",
        *([f"--gpu-vnc-port={options.vnc_port}"]
          if options.display == "vnc" else []),
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


def signal_guest(guest: subprocess.Popen[bytes], signum: int) -> None:
    try:
        os.killpg(guest.pid, signum)
    except ProcessLookupError:
        # No group has the emulator's ID, so it leads none. That is no
        # evidence it exited: signal the emulator itself.
        guest.send_signal(signum)


def stop_guest(guest: subprocess.Popen[bytes]) -> None:
    """Return only after the emulator has been reaped, and remove the run
    directories that one ended with SIGKILL could not."""
    try:
        for signum, timeout in GUEST_STOP_STAGES:
            if guest.poll() is not None:
                return
            signal_guest(guest, signum)
            try:
                guest.wait(timeout=timeout)
                return
            except subprocess.TimeoutExpired:
                continue
        if guest.poll() is not None:
            return
        signal_guest(guest, signal.SIGKILL)
        guest.wait()
    finally:
        remove_emulator_run_directories(guest.pid)


def install_shutdown_handlers() -> None:
    def request_shutdown(signum: int, _frame: object) -> NoReturn:
        raise RequestedShutdown(signum)

    signal.signal(signal.SIGTERM, request_shutdown)
    signal.signal(signal.SIGHUP, request_shutdown)


def main(arguments: Sequence[str] | None = None) -> int:
    options = parse_arguments(arguments)
    try:
        release = release_path(options.release)
        disk = disk_path(options.disk)
        adb = resolve_executable(options.adb, sdk_fallback=True)
        commands: list[list[str]] = []
        if options.build:
            commands = build_commands(resolve_executable("cmake"))
        guest_command = emulator_command(options, release, disk, options.emulator)
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
        sgdisk = resolve_executable(options.sgdisk) if options.sgdisk else None
        recreated = prepare_persistent_disk(release, disk, sgdisk=sgdisk)
        guest_command = emulator_command(options, release, disk, emulator)
        ensure_loopback_port_available(options.adb_port)
        disconnect_adb(adb, serial)
        print(f"Booting accepted Android release {release.name}.", file=sys.stderr)
        action = "Re-created" if recreated else "Reusing"
        print(f"{action} persistent 8 GiB disk: {disk}", file=sys.stderr)
        if options.display == "vnc":
            print(f"VNC display on 127.0.0.1:{options.vnc_port}; press Ctrl-C "
                  "to stop the guest.", file=sys.stderr)
        else:
            print("Native Metal display; press Ctrl-C to stop the guest.",
                  file=sys.stderr)
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
