# SPDX-License-Identifier: GPL-2.0-or-later
"""Host side of an Android run in Andarium on a connected iOS device.

Andarium carries the Android guests that Kernarium used to run on a device
(Andarium/README.md#automation). A harness launches it through
tests/integration/support/ios_device.py with the settings ``settings_for``
builds: ``ANDARIUM_START`` starts the guest without Home, the runtime's own
``KERNARIUM_*`` settings size it, and a loopback forward publishes the
guest's adbd for ``iproxy``. A system image to boot instead of the bundled
one is staged into the container's Documents directory by exact name
(``ANDARIUM_SYSTEM_IMAGE``), and only a file this module copied is ever
removed again. The module also reads the console lines a run is judged by:
the runtime's result and the thermal state at the start and the end.
"""

from __future__ import annotations
# Imported support must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

from collections.abc import Mapping
from dataclasses import dataclass
import pathlib
import re
import subprocess

import ios_device


BUNDLE_ID = "com.imxieyi.andarium"
# The guest's adbd listens here; the forward publishes it on the device.
GUEST_ADB_PORT = 5555
# The runtime's own lines keep their Kernarium prefix.
RUNTIME_RESULT = re.compile(r"Andarium: runtime returned (-?[0-9]+)")
THERMAL_STATE = re.compile(
    r"Andarium thermal state: phase=(start|change|finish) "
    r"state=(nominal|fair|serious|critical) uptime=([0-9]+(?:\.[0-9]+)?)"
)
ADB_AUTH_OFF_LINE = "Kernarium: ADB authorization is off for this launch"
# The application reports losing and regaining focus, which a system alert
# (iOS asking for local network access) causes until a person answers it.
APPLICATION_ACTIVE_LINE = "Andarium: application active"
APPLICATION_INACTIVE_LINE = "Andarium: application inactive"
# Only the automation runtime reads the authorization setting, so its name in
# the Android runtime's binary identifies that build.
AUTOMATION_SYMBOL = b"KERNARIUM_ANDROID_ADB_AUTH"
ANDROID_RUNTIME_FRAMEWORKS = ("KRuntime_ACK_6_18_4K",)
# The application reads the memory setting in 64 MiB steps within the range
# its ruler spans.
MEMORY_MIB_RANGE = (1024, 4096)
MEMORY_MIB_STEP = 64
START_MODES = ("normal", "recovery")
IMAGE_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*\.img(?:\.zst)?")
DOCUMENTS_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*")
SETTING_NAME = re.compile(r"(?:KERNARIUM|ANDARIUM)_[A-Z0-9_]+")
SLOT_PORT_VARIABLE = "GUEST_SLOT_PORT_BASE"

# A launch clears every inherited setting of either family, because one would
# select a different guest.
APPLICATION = ios_device.Application(
    BUNDLE_ID, ("KERNARIUM_", "ANDARIUM_"), RUNTIME_RESULT
)


class AndariumError(ios_device.DeviceError):
    pass


def require_automation_app(app: pathlib.Path) -> None:
    """Refuse an application whose Android runtime asks for authorization."""
    for name in ANDROID_RUNTIME_FRAMEWORKS:
        binary = app / "Frameworks" / f"{name}.framework" / name
        if not binary.is_file():
            raise AndariumError(f"{app} carries no {name} runtime")
        if AUTOMATION_SYMBOL not in binary.read_bytes():
            raise AndariumError(
                f"{binary} is the release runtime; package the "
                "kernarium-ios-device-android-automation preset with "
                "Kernarium/scripts/package-ios-frameworks.sh "
                "--android-automation"
            )


def application_is_active(transcript: str) -> bool:
    """Whether the console's last word on focus is that it is active (or it
    has said nothing, as a launch that never lost focus does)."""
    return (transcript.rfind(APPLICATION_INACTIVE_LINE)
            <= transcript.rfind(APPLICATION_ACTIVE_LINE))


def require_adb_authorization_off(transcript: str) -> None:
    """Refuse a run whose runtime waits for the USB debugging prompt."""
    if ADB_AUTH_OFF_LINE not in transcript:
        raise AndariumError(
            "the application did not report ADB authorization off; the "
            "installed build is the release runtime, whose first adb "
            "connection waits for a prompt nobody answers"
        )


def host_port_from_slot(environment: Mapping[str, str]) -> int:
    """The guest slot's first port, the one the adb forward uses."""
    value = environment.get(SLOT_PORT_VARIABLE)
    if value is None or not value.isdecimal():
        raise AndariumError(
            f"{SLOT_PORT_VARIABLE} is not set (the guest-slot wrapper "
            "exports it); run under the wrapper or pass --host-port"
        )
    return validate_port(int(value))


def validate_port(port: int) -> int:
    if not 1024 <= port <= 65535 - 100:
        raise AndariumError(f"not a usable port: {port}")
    return port


def host_forward(port: int) -> str:
    """The forward setting: the guest's adbd on the device's loopback."""
    return f"tcp:127.0.0.1:{validate_port(port)}-{GUEST_ADB_PORT}"


def system_image_name(name: str) -> str:
    """A Documents name the application accepts as a seed image."""
    if IMAGE_NAME.fullmatch(name) is None:
        raise AndariumError(
            f"not a seed image name (.img or .img.zst): {name!r}"
        )
    return name


def settings_for(
    *,
    start: str = "normal",
    erase: bool = False,
    system_image: str | None = None,
    cpus: int,
    memory_mib: int,
    engine: str,
    host_port: int,
    disk_size_gib: int | None = None,
    extra: Mapping[str, str] | None = None,
) -> dict[str, str]:
    """The application settings for one run, without the child prefix.

    The runtime's authorization setting is off because nobody is at the
    screen to answer the prompt; ``host_forward`` keeps that on loopback.
    """
    if start not in START_MODES:
        raise AndariumError(f"not a start mode: {start!r}")
    low, high = MEMORY_MIB_RANGE
    if not low <= memory_mib <= high or memory_mib % MEMORY_MIB_STEP:
        raise AndariumError(
            f"guest memory is {low} to {high} MiB in "
            f"{MEMORY_MIB_STEP} MiB steps, not {memory_mib}"
        )
    if cpus < 1:
        raise AndariumError(f"not a vCPU count: {cpus}")
    settings = {
        "ANDARIUM_START": start,
        "KERNARIUM_VCPU_COUNT": str(cpus),
        "KERNARIUM_GUEST_MEMORY_MIB": str(memory_mib),
        "KERNARIUM_EXECUTION_ENGINE": engine,
        "KERNARIUM_HOST_FORWARD": host_forward(host_port),
        "KERNARIUM_ANDROID_ADB_AUTH": "0",
    }
    if erase:
        settings["ANDARIUM_ERASE"] = "1"
    if system_image is not None:
        settings["ANDARIUM_SYSTEM_IMAGE"] = system_image_name(system_image)
    if disk_size_gib is not None:
        settings["KERNARIUM_ANDROID_DISK_SIZE_GIB"] = str(disk_size_gib)
    for name, value in (extra or {}).items():
        if SETTING_NAME.fullmatch(name) is None:
            raise AndariumError(f"not an application setting: {name!r}")
        if name in settings:
            raise AndariumError(f"{name} is set by the runner's own options")
        settings[name] = value
    return settings


def runtime_result(transcript: str) -> int | None:
    return ios_device.runtime_result(APPLICATION, transcript)


@dataclass(frozen=True)
class ThermalSample:
    phase: str
    state: str
    uptime_seconds: float


def thermal_samples(transcript: str) -> list[ThermalSample]:
    return [
        ThermalSample(phase, state, float(uptime))
        for phase, state, uptime in THERMAL_STATE.findall(transcript)
    ]


def thermal_record(transcript: str) -> dict[str, object]:
    """The thermal state at the start and the finish, and every change.

    A run without both is incomplete: its record says so rather than leaving
    a reader to infer a nominal device.
    """
    samples = thermal_samples(transcript)
    first = {}
    for sample in samples:
        first.setdefault(sample.phase, sample)

    def row(sample: ThermalSample | None) -> dict[str, object] | None:
        if sample is None:
            return None
        return {"state": sample.state, "uptime_seconds": sample.uptime_seconds}

    finishes = [sample for sample in samples if sample.phase == "finish"]
    return {
        "start": row(first.get("start")),
        "finish": row(finishes[-1] if finishes else None),
        "changes": [
            row(sample) for sample in samples if sample.phase == "change"
        ],
        "complete": "start" in first and bool(finishes),
    }


def plain_documents_name(name: str) -> str:
    if DOCUMENTS_NAME.fullmatch(name) is None:
        raise AndariumError(f"not a plain Documents name: {name!r}")
    return name


class StagedFiles:
    """The files one run copies into the application's Documents directory.

    ``devicectl`` cannot remove a file from a container, so ``remove`` asks
    afcclient to. It removes exactly the names this run copied: a file the
    device already held under that name, and every other file in Documents,
    stays. A name is recorded before its copy starts, because a copy that
    fails part-way leaves a partial file of that name, which would refuse the
    next copy of the real one.
    """

    def __init__(self, device: str) -> None:
        self.device = device
        self.copied: list[str] = []

    def copy(self, source: pathlib.Path, name: str, timeout: int) -> bool:
        """Copy ``source`` to ``Documents/name``; True when this call did.

        A file of the same name and size already there is reused and left
        for its owner. A different file of that name is refused.
        """
        plain_documents_name(name)
        held = ios_device.device_documents(self.device, APPLICATION).get(name)
        if held is None:
            self.copied.append(name)
        ios_device.copy_to_device(
            self.device, APPLICATION, source, name, timeout
        )
        return held is None

    def remove(self, afcclient: str, timeout: int = 300) -> dict[str, list]:
        """Remove what this run copied; report what went and what stayed.

        The listing after the removals decides, not afcclient's output.
        """
        report: dict[str, list] = {"removed": [], "failed": []}
        if not self.copied:
            return report
        try:
            present = set(ios_device.device_documents(
                self.device, APPLICATION
            ))
        except (OSError, subprocess.SubprocessError):
            present = set(self.copied)
        attempted = [name for name in self.copied if name in present]
        for name in attempted:
            self._afc(afcclient, f"rm /Documents/{name}", timeout)
        try:
            left = set(ios_device.device_documents(self.device, APPLICATION))
        except (OSError, subprocess.SubprocessError):
            left = set(attempted)
        report["removed"] = [name for name in attempted if name not in left]
        report["failed"] = [name for name in attempted if name in left]
        return report

    def _afc(self, afcclient: str, command: str, timeout: int) -> None:
        try:
            subprocess.run(
                [afcclient, "-u", self.device, "--container", BUNDLE_ID],
                input=f"{command}\nquit\n",
                text=True,
                capture_output=True,
                timeout=timeout,
            )
        except (OSError, subprocess.SubprocessError):
            pass
