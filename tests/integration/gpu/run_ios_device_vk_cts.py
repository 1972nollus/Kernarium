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
from typing import Sequence

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
# The guest kernel's reaper names what it killed. A fraction whose CTS process
# is gone will never reach its final frame, and the guest has been observed to
# stop printing entirely at that moment, so waiting out the stall guard costs
# half an hour and yields nothing the console did not already hold.
GUEST_OOM_KILL = re.compile(
    r"Out of memory: Killed process \d+ \(([^)]+)\)"
)
# What the guest's own exit trap needs to transport its last QPA segment and
# its summary, when it still can.
OOM_GRACE_SECONDS = 180
# A backend that abandons its connection says so and never serves that device
# again. The guest driver discovers it one warning per interrupt instead, which
# has produced a 139 MB console and a run that had to be ended by hand, so stop
# reading shortly after the line appears rather than at the stall guard.
LOST_VHOST_USER = re.compile(r"^.*vhost-user connection failed: .*$", re.M)
LOST_TRANSPORT_GRACE_SECONDS = 60


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


def run_campaign(
    options: argparse.Namespace, log: Path
) -> tuple[str, str | None, str | None]:
    """Hold the console until the campaign's last frame or the deadline.

    The wait is bounded four times: an overall deadline, a stall deadline that
    only the console growing resets, and a short grace after either of the two
    events that end a fraction's useful output — the guest kernel reporting an
    out-of-memory kill, or a vhost-user backend abandoning its connection. A
    device that wedges with the screen on would otherwise hold this lane for as
    long as the battery lasts; a guest whose CTS process the reaper took would
    hold it for the whole stall guard; and a guest whose GPU transport died
    keeps the console growing with one driver warning per interrupt, so the
    stall guard never fires at all.

    Returns the transcript, the reaper's own line, and the backend's own line.
    """
    command = device_command(
        "device", "process", "launch", "--device", options.device,
        "--console", "--terminate-existing", BUNDLE_ID,
    )
    deadline = time.monotonic() + options.timeout
    stall_deadline = time.monotonic() + options.stall_timeout
    next_update = time.monotonic() + PROGRESS_INTERVAL
    observed = 0
    grace_deadline: float | None = None
    oom_report: str | None = None
    transport_report: str | None = None
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
                    return transcript, oom_report, transport_report
                exited = process.poll() is not None
                now = time.monotonic()
                size = log.stat().st_size
                if size > observed:
                    observed = size
                    stall_deadline = now + options.stall_timeout
                if exited:
                    return transcript, oom_report, transport_report
                if grace_deadline is None:
                    found = GUEST_OOM_KILL.search(transcript)
                    if found is not None:
                        oom_report = found.group(0)
                        grace_deadline = now + OOM_GRACE_SECONDS
                        print(
                            f"the guest kernel killed {found.group(1)} out of "
                            f"memory; giving it {OOM_GRACE_SECONDS}s to "
                            "transport what it has",
                            flush=True,
                        )
                if grace_deadline is None:
                    found = LOST_VHOST_USER.search(transcript)
                    if found is not None:
                        transport_report = found.group(0).strip()
                        grace_deadline = now + LOST_TRANSPORT_GRACE_SECONDS
                        print(
                            "a vhost-user backend abandoned its connection; "
                            f"giving the guest {LOST_TRANSPORT_GRACE_SECONDS}s "
                            f"to transport what it has: {transport_report}",
                            flush=True,
                        )
                if now >= deadline:
                    fail(
                        f"the campaign exceeded {options.timeout} seconds; "
                        f"see {log}"
                    )
                if grace_deadline is not None and now >= grace_deadline:
                    # Not a failure of this lane: the executed prefix is in the
                    # transcript and the caller decodes and reports it.
                    return transcript, oom_report, transport_report
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


def report(
    artifacts: dict[str, bytes],
    transcript: str,
    oom_report: str | None = None,
    rejected: Sequence[str] = (),
    transport_report: str | None = None,
) -> int:
    """Judge the attempt by its summary, not by what the console says.

    The application echoes the generated guest launcher before running it, and
    that script carries its own ``UML_VK_CTS_ERROR`` strings inside ``||``
    clauses, so searching the transcript for the marker reports a failure on
    every successful run. ``summary.txt`` is the guest's own verdict.

    A run the guest's reaper ended has no summary, so the executed prefix's own
    status counts are printed either way: they are the only verdicts it has.
    """

    print("---- campaign summary ----")
    print("artifacts: " + (", ".join(sorted(artifacts)) or "none"))
    summary: dict[str, str] = {}
    payload = artifacts.get("summary.txt")
    if payload is None:
        print("the campaign delivered no summary")
    else:
        for line in payload.decode("utf-8", errors="replace").splitlines():
            key, separator, value = line.partition("=")
            if separator:
                summary[key] = value
        for key in (
            "mode",
            "fraction",
            "expected_case_count",
            *sorted(key for key in summary if key.startswith("device_")),
            "cts_rc",
            # A fraction that could not transport every QPA byte it produced
            # reports that here rather than in the size of its results.
            "qpa_transfer",
            "qpa_missing_bytes",
            "formal_conformance",
        ):
            if key in summary:
                print(f"{key}={summary[key]}")
    skipped = artifacts.get("skipped.txt")
    if skipped is not None:
        # Each row is "<reason>\t<case>": the reason the device left it out.
        reasons = Counter(
            line.partition("\t")[0]
            for line in skipped.decode("utf-8", errors="replace").splitlines()
            if line
        )
        print(f"skipped_for_a_device_limit={sum(reasons.values())}")
        for reason, count in sorted(reasons.items()):
            print(f"skipped_{reason}={count}")
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
    if oom_report is not None:
        print(f"guest_out_of_memory={oom_report}")
    if transport_report is not None:
        # The guest lost a device from this line onwards, so every case after
        # it is unjudged rather than passing.
        print(f"lost_vhost_user_transport={transport_report}")
    if rejected:
        # A frame group the host could not accept: results the guest framed and
        # this run does not hold. Named so the loss is not read as a short run.
        print("undecodable_artifacts=" + ",".join(sorted(rejected)))
    failed = (
        payload is None
        or summary.get("cts_rc") != "0"
        or transport_report is not None
    )
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
    transcript, oom_report, transport_report = run_campaign(options, log)
    print(f"console closed after {time.monotonic() - started:.0f}s: {log}")
    rejected: list[str] = []
    artifacts = cts.decode_artifacts(
        transcript, partial=True, rejected=rejected
    )
    cts.write_artifacts(artifacts, options.artifact_root / "artifacts")
    print(f"artifacts: {options.artifact_root / 'artifacts'}")
    return report(artifacts, transcript, oom_report, rejected, transport_report)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except cts.CampaignError as error:
        print(f"run_ios_device_vk_cts.py: {error}", file=sys.stderr)
        raise SystemExit(1) from error
