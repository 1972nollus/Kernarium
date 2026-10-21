#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build the Apple playback sink for the iOS Simulator and run its
interruption test there.

ios_interruption_tests.mm plays through the sink and posts the audio-session
notifications iOS posts when another app interrupts the session; it prints one
PASS or FAIL line per check. This runner compiles it with the sink, runs it in
a booted iOS Simulator device (or boots one and shuts it down again), and
passes when the test does. It exits 77, which CTest reports as skipped, when
the host has no iOS Simulator runtime at the deployment target.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import json
import pathlib
import re
import subprocess
import sys
import tempfile

SKIPPED = 77
RUNTIME = re.compile(r"com\.apple\.CoreSimulator\.SimRuntime\.iOS-(\d+)-(\d+)")
BOOT_TIMEOUT_SECONDS = 180
COMPILE_TIMEOUT_SECONDS = 300
RUN_TIMEOUT_SECONDS = 120


def simctl(*arguments: str, timeout: int = 60) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["xcrun", "simctl", *arguments],
        check=False,
        capture_output=True,
        text=True,
        stdin=subprocess.DEVNULL,
        timeout=timeout,
    )


def pick_device(floor: tuple[int, int]) -> tuple[str, bool] | None:
    """The UDID of a device at or above ``floor`` and whether it is booted."""
    listing = json.loads(
        simctl("list", "devices", "available", "--json").stdout
    )
    booted = []
    shutdown = []
    for runtime, devices in listing["devices"].items():
        match = RUNTIME.fullmatch(runtime)
        if match is None or (int(match[1]), int(match[2])) < floor:
            continue
        for device in devices:
            if not device.get("isAvailable"):
                continue
            bucket = booted if device["state"] == "Booted" else shutdown
            bucket.append(device["udid"])
    if booted:
        return booted[0], True
    if shutdown:
        return shutdown[0], False
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sink-directory", required=True, type=pathlib.Path)
    parser.add_argument("--include-directory", required=True, type=pathlib.Path)
    parser.add_argument("--deployment-target", required=True)
    arguments = parser.parse_args()
    floor = tuple(int(part) for part in arguments.deployment_target.split("."))[:2]

    chosen = pick_device(floor)
    if chosen is None:
        print(f"SKIP: no iOS Simulator device at iOS {arguments.deployment_target} or later")
        return SKIPPED
    udid, was_booted = chosen

    here = pathlib.Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix="uml-audio-interruption-") as scratch:
        binary = pathlib.Path(scratch) / "interruption_tests"
        compile_command = [
            "xcrun", "--sdk", "iphonesimulator", "clang++",
            "-target", f"arm64-apple-ios{arguments.deployment_target}-simulator",
            "-x", "objective-c++", "-std=c++20", "-fobjc-arc", "-O1",
            "-Wall", "-Wextra", "-Werror",
            f"-I{arguments.sink_directory}", f"-I{arguments.include_directory}",
            str(here / "ios_interruption_tests.mm"),
            str(arguments.sink_directory / "apple_playback_sink.mm"),
            "-framework", "AVFAudio", "-framework", "Foundation",
            "-o", str(binary),
        ]
        built = subprocess.run(
            compile_command,
            check=False,
            stdin=subprocess.DEVNULL,
            timeout=COMPILE_TIMEOUT_SECONDS,
        )
        if built.returncode != 0:
            print("FAIL: building the interruption test for the iOS Simulator")
            return 1

        if not was_booted:
            booted = simctl("boot", udid, timeout=BOOT_TIMEOUT_SECONDS)
            if booted.returncode != 0:
                print(f"FAIL: booting simulator {udid}: {booted.stderr.strip()}")
                return 1
            simctl("bootstatus", udid, timeout=BOOT_TIMEOUT_SECONDS)
        try:
            result = simctl(
                "spawn", udid, str(binary), timeout=RUN_TIMEOUT_SECONDS
            )
        finally:
            if not was_booted:
                simctl("shutdown", udid, timeout=BOOT_TIMEOUT_SECONDS)
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        return 0 if result.returncode == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
