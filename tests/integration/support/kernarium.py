"""Host side of a Kernarium automation run.

Only the automation build of the Kernarium Linux runtime boots a host payload
(Kernarium/README.md, "Automation runs"). ``KERNARIUM_BOOT_PAYLOAD`` names a
newc segment in the application's Documents directory, which the runtime
appends after its own segment, and ``KERNARIUM_SYSTEMD_UNIT`` names the
systemd target the guest boots. This module writes such segments with
``uml-initramfs``, recognises the automation build, and copies files into the
application's container on a connected device.
"""

from __future__ import annotations
# Imported support must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import hashlib
import json
import os
import pathlib
import re
import signal
import subprocess
import tempfile
import time
from collections.abc import Callable, Mapping, Sequence

import guest


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
BUNDLE_ID = "com.imxieyi.Kernarium"
DEFAULT_INITRAMFS_TOOL = (
    REPOSITORY / ".build" / "presets" / "macos-integration" / "bin"
    / "uml-initramfs"
)
# The runtime refuses a larger payload: the kernel copies the whole initrd
# into boot memory before it unpacks it.
PAYLOAD_LIMIT = 64 << 20
NEWC_MAGIC = b"070701"
# Only the automation runtime reads the payload setting, so its name in a
# framework binary identifies that build.
AUTOMATION_SYMBOL = b"KERNARIUM_BOOT_PAYLOAD"
AUTOMATION_BOOT_LINE = "Kernarium: automation boot with a "
RUNTIME_RESULT = re.compile(r"Kernarium: runtime returned (-?[0-9]+)")
SESSION_EXIT = re.compile(r"UML_SESSION_EXIT:([0-9]+)")
SYSTEMD_TARGET = re.compile(r"[A-Za-z0-9@_.:-]{1,121}\.target")
LINUX_RUNTIME_FRAMEWORKS = ("KRuntime_6_18_16K", "KRuntime_6_18_4K")
WORKLOAD_GUEST_PATH = "/usr/local/libexec/uml-workload"
PROGRESS_INTERVAL_SECONDS = 60


class KernariumError(RuntimeError):
    pass


def root_type(image_name: str) -> str:
    """The root filesystem type the runtime infers from an image's name."""
    suffix = pathlib.PurePosixPath(image_name).suffix.lower()
    if suffix in (".erofs", ".ext4"):
        return suffix[1:]
    raise KernariumError(f"not an .erofs or .ext4 image name: {image_name}")


def boot_parameters(
    image_type: str, *, upper: str = "ext4", entry: str = "/sbin/init"
) -> str:
    """A ``/boot.env`` for the runtime's disks.

    The runtime's own boots the image read-only on ubd0 beneath a fresh ext4
    writable layer on ubd1, the overlay disk, and then the image's init. A
    payload that brings its own replaces that file; ``upper="tmpfs"`` leaves
    the overlay disk to the workload.
    """
    if image_type not in ("erofs", "ext4") or upper not in ("ext4", "tmpfs"):
        raise KernariumError("unsupported root type or writable layer")
    lines = [
        "UML_BOOT_ROOT=/dev/ubda",
        f"UML_BOOT_ROOT_TYPE={image_type}",
        f"UML_BOOT_UPPER={upper}",
        *(["UML_BOOT_UPPER_DEVICE=/dev/ubdb"] if upper == "ext4" else []),
        f"UML_BOOT_ENTRY={entry}",
    ]
    return "".join(f"{line}\n" for line in lines)


class Payload:
    """The files one automation run adds to the guest's new root."""

    def __init__(self) -> None:
        self._files: dict[str, guest.GuestFile] = {}

    def add(
        self, guest_path: str, contents: bytes | str, mode: int = 0o644
    ) -> None:
        guest.validate_payload_path(guest_path)
        data = contents.encode() if isinstance(contents, str) else contents
        archive_path = f"/payload{guest_path}"
        if archive_path in self._files:
            raise KernariumError(f"the payload already holds {guest_path}")
        self._files[archive_path] = guest.GuestFile(data, mode)

    def set_boot_parameters(self, parameters: str) -> None:
        """Replace the runtime's ``/boot.env``."""
        self._files["/boot.env"] = guest.GuestFile(parameters.encode())

    @property
    def files(self) -> Mapping[str, guest.GuestFile]:
        """Archive paths and contents, ``/payload/...`` and ``/boot.env``."""
        return dict(self._files)

    def write(
        self,
        output: pathlib.Path,
        tool: pathlib.Path = DEFAULT_INITRAMFS_TOOL,
    ) -> pathlib.Path:
        """Write the segment to the new file ``output`` and return it."""
        if not self._files:
            raise KernariumError("an empty payload changes nothing")
        if not tool.is_file() or not os.access(tool, os.X_OK):
            raise KernariumError(
                f"uml-initramfs is unavailable: {tool}; build the "
                "uml-initramfs target or pass its path"
            )
        with tempfile.TemporaryDirectory(prefix="kernarium-payload-") as name:
            directory = pathlib.Path(name)
            arguments = [str(tool), f"--output={output}"]
            for index, (archive_path, file) in enumerate(
                sorted(self._files.items())
            ):
                host = directory / f"{index:04d}"
                host.write_bytes(file.contents)
                host.chmod(file.mode)
                arguments.append(f"--file={archive_path}={host}")
            subprocess.run(arguments, check=True, stdin=subprocess.DEVNULL)
        size = output.stat().st_size
        with output.open("rb") as handle:
            magic = handle.read(len(NEWC_MAGIC))
        if magic != NEWC_MAGIC or size % 4 or size > PAYLOAD_LIMIT:
            raise KernariumError(
                f"{output} is not a padded newc segment of at most 64 MiB"
            )
        return output


def payload_name(path: pathlib.Path) -> str:
    """A Documents name that changes whenever the payload does.

    The device lanes never replace a file on a device, so equal names mean
    equal contents and a payload already there is reused.
    """
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    return f"kernarium-payload-{digest[:24]}.cpio"


def workload_payload(
    image_type: str,
    workload: bytes,
    *,
    files: Mapping[str, guest.GuestFile] | None = None,
    upper: str = "ext4",
) -> Payload:
    """A payload that runs one guest script and powers the guest off.

    The harness session (tools/rootfs/boot-ramdisk/session) runs the script
    as a child of PID 1 with the runtime filesystems a shell needs, prints
    ``UML_SESSION_EXIT:<status>`` and powers off; nothing is typed, so every
    line the script prints is its own.
    """
    payload = Payload()
    payload.set_boot_parameters(
        boot_parameters(
            image_type, upper=upper, entry=guest.SESSION_GUEST_PATH
        )
    )
    payload.add(
        guest.SESSION_GUEST_PATH, guest.SESSION_SCRIPT.read_bytes(), 0o755
    )
    payload.add(
        guest.SESSION_PARAMETERS_GUEST_PATH,
        f"UML_SESSION_ENTRY={WORKLOAD_GUEST_PATH}\n"
        "UML_SESSION_RUNTIME_MOUNTS=1\n",
    )
    payload.add(WORKLOAD_GUEST_PATH, workload, 0o755)
    for guest_path, file in sorted((files or {}).items()):
        payload.add(guest_path, file.contents, file.mode)
    return payload


def validate_systemd_target(unit: str) -> str:
    if SYSTEMD_TARGET.fullmatch(unit) is None:
        raise KernariumError(f"not a systemd target the runtime boots: {unit}")
    return unit


def is_automation_binary(binary: pathlib.Path) -> bool:
    return AUTOMATION_SYMBOL in binary.read_bytes()


def require_automation_app(app: pathlib.Path) -> None:
    """Refuse an application whose Linux runtimes ignore payloads."""
    for name in LINUX_RUNTIME_FRAMEWORKS:
        binary = app / "Frameworks" / f"{name}.framework" / name
        if not binary.is_file():
            raise KernariumError(f"{app} carries no {name} runtime")
        if not is_automation_binary(binary):
            raise KernariumError(
                f"{binary} is the release runtime; package the "
                "kernarium-ios-device-automation presets with "
                "Kernarium/scripts/package-ios-frameworks.sh --automation"
            )


def require_automation_boot(transcript: str) -> None:
    """Refuse a run whose application booted without the payload."""
    if AUTOMATION_BOOT_LINE not in transcript:
        raise KernariumError(
            "the application did not report an automation boot; the "
            "installed build is the release runtime, which ignores payloads"
        )


def device_command(*arguments: str) -> list[str]:
    return ["xcrun", "devicectl", *arguments]


def device_documents(device: str, timeout: int = 120) -> dict[str, int]:
    """The files in the application's Documents directory and their sizes."""
    with tempfile.TemporaryDirectory(prefix="kernarium-documents-") as name:
        report = pathlib.Path(name) / "files.json"
        subprocess.run(
            device_command(
                "device", "info", "files", "--device", device,
                "--domain-type", "appDataContainer",
                "--domain-identifier", BUNDLE_ID,
                "--subdirectory", "Documents",
                "--json-output", str(report),
                "--quiet", "--timeout", str(timeout),
            ),
            check=True,
            stdin=subprocess.DEVNULL,
            timeout=timeout + 30,
        )
        listing = json.loads(report.read_text(encoding="utf-8"))
    return {
        entry["relativePath"]: entry["metadata"]["size"]
        for entry in listing["result"]["files"]
        if not entry["resources"]["isDirectory"]
    }


def copy_to_device(
    device: str, source: pathlib.Path, name: str, timeout: int
) -> None:
    """Copy ``source`` to ``Documents/name``, never replacing a file there.

    A file of the same name and size is taken to be this one: payload names
    carry their digest, and an image name is its release.
    """
    if not name or "/" in name or name in (".", ".."):
        raise KernariumError(f"not a plain Documents name: {name!r}")
    size = source.stat().st_size
    existing = device_documents(device).get(name)
    if existing == size:
        print(f"the device already holds Documents/{name}", flush=True)
        return
    if existing is not None:
        raise KernariumError(
            f"the device holds a different Documents/{name}; this lane "
            "never replaces a file on a device"
        )
    subprocess.run(
        device_command(
            "device", "copy", "to", "--device", device,
            "--domain-type", "appDataContainer",
            "--domain-identifier", BUNDLE_ID,
            "--user", "mobile",
            "--source", str(source),
            "--destination", f"Documents/{name}",
            "--quiet", "--timeout", str(timeout),
        ),
        check=True,
        stdin=subprocess.DEVNULL,
        timeout=timeout + 30,
    )


def install_app(device: str, app: pathlib.Path, timeout: int) -> None:
    subprocess.run(
        ["codesign", "--verify", "--deep", "--strict", str(app)],
        check=True,
        stdin=subprocess.DEVNULL,
    )
    subprocess.run(
        device_command(
            "device", "install", "app", "--device", device, str(app),
            "--quiet", "--timeout", str(timeout),
        ),
        check=True,
        stdin=subprocess.DEVNULL,
        timeout=timeout + 30,
    )


def device_launch_environment(settings: Mapping[str, str]) -> dict[str, str]:
    """devicectl forwards each ``DEVICECTL_CHILD_`` variable to the app.

    Inherited Kernarium settings are cleared, because one would select a
    different guest.
    """
    environment = {
        name: value
        for name, value in os.environ.items()
        if not name.startswith("DEVICECTL_CHILD_KERNARIUM_")
    }
    for name, value in settings.items():
        environment[f"DEVICECTL_CHILD_{name}"] = value
    return environment


def stop_console(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    process.send_signal(signal.SIGINT)
    try:
        process.wait(timeout=30)
    except subprocess.TimeoutExpired:
        process.terminate()
        process.wait(timeout=10)


def run_device_console(
    device: str,
    settings: Mapping[str, str],
    log: pathlib.Path,
    *,
    timeout: int,
    stall_timeout: int,
    label: str,
    finished: Callable[[str], bool] | None = None,
) -> str:
    """Launch the application and hold its console until the run ends.

    The run ends at the application's own ``Kernarium: runtime returned`` line
    or when ``finished`` accepts the transcript. The wait is bounded by an
    overall deadline and by a stall deadline that only a growing console
    resets.
    """
    command = device_command(
        "device", "process", "launch", "--device", device,
        "--console", "--terminate-existing", BUNDLE_ID,
    )
    started = time.monotonic()
    deadline = started + timeout
    stall_deadline = started + stall_timeout
    next_update = started + PROGRESS_INTERVAL_SECONDS
    observed = 0
    with log.open("wb") as output:
        process = subprocess.Popen(
            command,
            stdout=output,
            stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL,
            env=device_launch_environment(settings),
        )
        try:
            while True:
                transcript = log.read_text(encoding="utf-8", errors="replace")
                if RUNTIME_RESULT.search(transcript) is not None or (
                    finished is not None and finished(transcript)
                ):
                    return transcript
                if process.poll() is not None:
                    return transcript
                now = time.monotonic()
                size = log.stat().st_size
                if size > observed:
                    observed = size
                    stall_deadline = now + stall_timeout
                if now >= deadline:
                    raise KernariumError(
                        f"{label} exceeded {timeout} seconds; see {log}"
                    )
                if now >= stall_deadline:
                    raise KernariumError(
                        f"the console of {label} stopped growing for "
                        f"{stall_timeout} seconds; see {log}"
                    )
                if now >= next_update:
                    print(
                        f"{label} is running; console_bytes={size}",
                        flush=True,
                    )
                    next_update = now + PROGRESS_INTERVAL_SECONDS
                time.sleep(1.0)
        finally:
            stop_console(process)


def runtime_result(transcript: str) -> int | None:
    results = RUNTIME_RESULT.findall(transcript)
    return int(results[-1]) if results else None


def session_exit(transcript: str) -> int | None:
    results = SESSION_EXIT.findall(transcript)
    return int(results[-1]) if results else None


def settings_for(
    *,
    rootfs: str,
    payload: str | None,
    unit: str | None = None,
    cpus: int,
    memory_mib: int,
    engine: str,
    page_size: int,
    extra: Mapping[str, str] | None = None,
) -> dict[str, str]:
    """The application settings for one run, without the child prefix."""
    if page_size not in (4096, 16384):
        raise KernariumError("the kernel page size is 4096 or 16384")
    settings = {
        "KERNARIUM_ROOTFS": rootfs,
        "KERNARIUM_VCPU_COUNT": str(cpus),
        "KERNARIUM_GUEST_MEMORY_MIB": str(memory_mib),
        "KERNARIUM_EXECUTION_ENGINE": engine,
        "KERNARIUM_KERNEL_PAGE_SIZE": str(page_size),
    }
    if payload is not None:
        settings["KERNARIUM_BOOT_PAYLOAD"] = payload
    if unit is not None:
        settings["KERNARIUM_SYSTEMD_UNIT"] = validate_systemd_target(unit)
    settings.update(extra or {})
    return settings


def parse_guest_file(value: str) -> tuple[str, guest.GuestFile]:
    """``GUEST=HOST``: a host file installed at a guest path, mode kept."""
    guest_path, separator, host = value.partition("=")
    if not separator or not host:
        raise KernariumError(f"expected GUEST=HOST, not {value!r}")
    source = pathlib.Path(host)
    mode = source.stat().st_mode & 0o777
    return guest_path, guest.GuestFile(source.read_bytes(), mode)


def guest_files(values: Sequence[str]) -> dict[str, guest.GuestFile]:
    files: dict[str, guest.GuestFile] = {}
    for value in values:
        guest_path, file = parse_guest_file(value)
        if guest_path in files:
            raise KernariumError(f"{guest_path} is named twice")
        files[guest_path] = file
    return files
