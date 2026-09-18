#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run one Kernarium Vulkan CTS campaign on a connected iOS device.

The application already carries every campaign and generates its guest-side
launcher from the same ``run_vk_cts.py`` driver the macOS lane uses, so this
lane only installs the bundle, names a campaign, and reads what the guest
prints. It is the device counterpart of ``run_ios_mac_vk_gl_cts.py``, which
covers the same application running on this Mac in Designed for iPad mode.

``devicectl`` forwards each ``DEVICECTL_CHILD_``-prefixed variable to the
launched process with the prefix removed, and the guest console reaches the
console transcript, so the campaign's artifact frames arrive without a session
log. The image itself is staged once with ``devicectl device copy to``; an iOS
application reads nothing outside its own container.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import os
import re
import signal
import subprocess
import sys
import time
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import run_vk_cts as vulkan  # noqa: E402
import run_vk_gl_cts as cts  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_ARTIFACT_ROOT = REPO_ROOT / ".build" / "ios-device-vk-cts"
BUNDLE_ID = "com.imxieyi.Kernarium"
# The receipt names the authenticated derivative; the application selects it by
# file name inside its own Documents directory.
DEFAULT_ROOTFS_FILENAME = "rootfs-vulkan-venus.ext4"
PROGRESS_INTERVAL = 60


def fail(message: str) -> None:
    raise cts.CampaignError(message)


def device_command(*arguments: str) -> list[str]:
    return ["xcrun", "devicectl", *arguments]


def install(device: str, app: Path, timeout: int) -> None:
    subprocess.run(
        ["codesign", "--verify", "--deep", "--strict", str(app)], check=True
    )
    subprocess.run(
        device_command(
            "device", "install", "app", "--device", device, str(app),
            "--quiet", "--timeout", str(timeout),
        ),
        check=True,
    )


def stage_image(device: str, image: Path, filename: str, timeout: int) -> None:
    subprocess.run(
        device_command(
            "device", "copy", "to", "--device", device,
            "--domain-type", "appDataContainer",
            "--domain-identifier", BUNDLE_ID,
            "--user", "mobile",
            "--source", str(image),
            "--destination", f"Documents/{filename}",
            "--quiet", "--timeout", str(timeout),
        ),
        check=True,
    )


def launch_environment(options: argparse.Namespace) -> dict[str, str]:
    """The switches the campaign's stack needs, named by the drivers.

    A Vulkan campaign reaches Venus, so it takes the same pair the macOS lane
    sets. Every other mode the application understands is cleared, because an
    inherited one would select a different guest.
    """
    environment = dict(os.environ)
    for name in list(environment):
        if name.startswith("DEVICECTL_CHILD_KERNARIUM_"):
            del environment[name]
    environment.update(
        {
            "DEVICECTL_CHILD_KERNARIUM_CTS_CAMPAIGN": options.campaign,
            "DEVICECTL_CHILD_KERNARIUM_ROOTFS": options.rootfs_filename,
            "DEVICECTL_CHILD_KERNARIUM_EXECUTION_ENGINE": options.engine,
            "DEVICECTL_CHILD_KERNARIUM_GUEST_MEMORY_MIB": str(
                options.memory_mib
            ),
            "DEVICECTL_CHILD_KERNARIUM_VCPU_COUNT": str(options.cpus),
            "DEVICECTL_CHILD_KERNARIUM_KERNEL_PAGE_SIZE": "16384",
            "DEVICECTL_CHILD_UML_GPU_EXPERIMENTAL_VULKAN": "1",
            "DEVICECTL_CHILD_UML_GPU_CAPABILITY_REPORT": "1",
            "DEVICECTL_CHILD_UML_GPU_FRAME_STATS": "1",
        }
    )
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


def run_campaign(options: argparse.Namespace, log: Path) -> str:
    """Hold the console until the campaign's last frame or the deadline.

    The wait is bounded twice: an overall deadline, and a stall deadline that
    only the console growing resets. A device that wedges with the screen on
    would otherwise hold this lane for as long as the battery lasts.
    """
    command = device_command(
        "device", "process", "launch", "--device", options.device,
        "--console", "--terminate-existing", BUNDLE_ID,
    )
    deadline = time.monotonic() + options.timeout
    stall_deadline = time.monotonic() + options.stall_timeout
    next_update = time.monotonic() + PROGRESS_INTERVAL
    observed = 0
    with log.open("wb") as output:
        process = subprocess.Popen(
            command,
            stdout=output,
            stderr=subprocess.STDOUT,
            env=launch_environment(options),
        )
        try:
            while True:
                transcript = log.read_text(
                    encoding="utf-8", errors="replace"
                )
                if cts.FINAL_ARTIFACT_FRAME in transcript:
                    return transcript
                exited = process.poll() is not None
                now = time.monotonic()
                size = log.stat().st_size
                if size > observed:
                    observed = size
                    stall_deadline = now + options.stall_timeout
                if exited:
                    return transcript
                if now >= deadline:
                    fail(
                        f"the campaign exceeded {options.timeout} seconds; "
                        f"see {log}"
                    )
                if now >= stall_deadline:
                    fail(
                        "the console stopped growing for "
                        f"{options.stall_timeout} seconds; see {log}"
                    )
                if now >= next_update:
                    print(
                        f"{options.campaign} is running; "
                        f"console_bytes={size}",
                        flush=True,
                    )
                    next_update = now + PROGRESS_INTERVAL
                time.sleep(1.0)
        finally:
            stop_console(process)


def report(artifacts: dict[str, bytes], transcript: str) -> int:
    """Judge the attempt by its summary, not by what the console says.

    The application echoes the generated guest launcher before running it, and
    that script carries its own ``UML_VK_CTS_ERROR`` strings inside ``||``
    clauses, so searching the transcript for the marker reports a failure on
    every successful run. ``summary.txt`` is the guest's own verdict.
    """

    print("---- campaign summary ----")
    print("artifacts: " + (", ".join(sorted(artifacts)) or "none"))
    payload = artifacts.get("summary.txt")
    if payload is None:
        print("the campaign delivered no summary")
        print("--------------------------")
        return 1
    summary: dict[str, str] = {}
    for line in payload.decode("utf-8", errors="replace").splitlines():
        key, separator, value = line.partition("=")
        if separator:
            summary[key] = value
    for key in ("mode", "expected_case_count", "cts_rc", "formal_conformance"):
        if key in summary:
            print(f"{key}={summary[key]}")
    qpa = artifacts.get("results.qpa")
    if qpa is not None:
        statuses = Counter(
            match.decode("ascii")
            for match in re.findall(
                rb'StatusCode="([A-Za-z]+)"', qpa
            )
        )
        for status, count in sorted(statuses.items()):
            print(f"{status}={count}")
    failed = summary.get("cts_rc") != "0"
    print("vulkan_campaign_failed=" + ("true" if failed else "false"))
    print("--------------------------")
    return 1 if failed else 0


def parse_arguments(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run a Kernarium Vulkan CTS campaign on an iOS device"
    )
    parser.add_argument(
        "--device", required=True,
        help="devicectl device identifier, UDID, or name",
    )
    parser.add_argument(
        "--campaign", required=True,
        help="a Vulkan campaign the installed application carries",
    )
    parser.add_argument("--app", type=Path, help="the .app bundle to install")
    parser.add_argument(
        "--rootfs", type=Path,
        help="stage this image before launching; omit when it is already there",
    )
    parser.add_argument("--rootfs-filename", default=DEFAULT_ROOTFS_FILENAME)
    parser.add_argument("--cpus", type=int, default=4)
    parser.add_argument("--memory-mib", type=int, default=1536)
    parser.add_argument("--engine", choices=("gadgets", "jit"),
                        default="gadgets")
    parser.add_argument("--timeout", type=int, default=86400)
    parser.add_argument("--stall-timeout", type=int, default=1800)
    parser.add_argument("--install-timeout", type=int, default=600)
    parser.add_argument("--artifact-root", type=Path,
                        default=DEFAULT_ARTIFACT_ROOT)
    options = parser.parse_args(argv)
    if options.cpus < 1 or options.memory_mib < 512:
        parser.error("guest CPU and memory settings are invalid")
    if options.timeout < 1 or options.stall_timeout < 1:
        parser.error("timeouts must be positive")
    return options


def main(argv: list[str] | None = None) -> int:
    options = parse_arguments(argv)
    if sys.platform != "darwin":
        fail("this lane drives an iOS device from macOS")
    carried = vulkan.apple_campaigns()
    if options.campaign not in carried:
        fail(
            f"no Vulkan campaign {options.campaign}; the application carries "
            + ", ".join(carried)
        )
    if options.artifact_root.exists():
        fail(f"artifact directory already exists: {options.artifact_root}")
    options.artifact_root.mkdir(parents=True)

    if options.app is not None:
        install(options.device, options.app.resolve(), options.install_timeout)
    if options.rootfs is not None:
        stage_image(
            options.device, options.rootfs.resolve(),
            options.rootfs_filename, options.install_timeout,
        )

    log = options.artifact_root / "console.log"
    started = time.monotonic()
    transcript = run_campaign(options, log)
    print(f"console closed after {time.monotonic() - started:.0f}s: {log}")
    artifacts = cts.decode_artifacts(transcript, partial=True)
    cts.write_artifacts(artifacts, options.artifact_root / "artifacts")
    print(f"artifacts: {options.artifact_root / 'artifacts'}")
    return report(artifacts, transcript)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except cts.CampaignError as error:
        print(f"run_ios_device_vk_cts.py: {error}", file=sys.stderr)
        raise SystemExit(1) from error
