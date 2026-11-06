#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Capture every screen and state of Andarium from the Simulator preview.

AndariumPreview opens any screen with mock data from a launch argument
(`-screen <name>`, see Andarium/README.md). This script launches each screen in
the build already installed on a Simulator and saves a labelled PNG for it,
plus a manifest.json that says, per file, the device, orientation, appearance,
screen and what to look at. It installs nothing and rotates nothing: build and
install AndariumPreview first, rotate an iPad (Simulator: Device > Rotate, or a
UI driver) between runs, and the orientation is read from each image.

    capture-screens.py --out DIR --device <udid> [--device <udid> ...]
    capture-screens.py --list

Light mode is captured for every screen; dark mode only for the screens flagged
in the table, where the design adapts to it. Every Simulator command has a hard
time bound. A rerun replaces the files and manifest entries it captures and
keeps the rest.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import json
import re
import struct
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

BUNDLE = "com.imxieyi.AndariumPreview"
COMMAND_SECONDS = 60
SETTLE_SECONDS = 15
SETTLED_FRACTION = 0.003


@dataclass(frozen=True)
class Shot:
    id: str
    group: str
    screen: str
    description: str
    args: tuple[str, ...] = ()
    dark: bool = False
    wait: float = 3.0

    def launch_arguments(self) -> list[str]:
        return ["-screen", self.screen, *self.args]


def s(id: str, group: str, screen: str, description: str, *args: str, dark: bool = False, wait: float = 3.0) -> Shot:
    return Shot(id, group, screen, description, tuple(args), dark, wait)


def boot_stages(prefix: str, screen: str, names: list[str], what: str, dark_step: int | None = None) -> list[Shot]:
    return [
        s(
            f"{prefix}-{index}-{name.lower().replace(' ', '-')}",
            "Boot",
            screen,
            f"{what}, step {index + 1} of {len(names)} ({name}) under way.",
            "-bootStage",
            str(index),
            dark=index == dark_step,
        )
        for index, name in enumerate(names)
    ]


SHOTS: list[Shot] = [
    # Home
    s("home", "Home", "home", "Home with a configured guest: Storage with Resize, System (vCPUs, RAM) with Edit, Display, Capabilities, Backup, Recovery and Erase, and the Start bar.", dark=True),
    s("home-welcome", "Home", "welcome", "Home before any guest exists: the welcome and Set Up Guest. First screen of the initial setup."),
    s("home-first-start", "Home", "home-new", "A guest just created and not yet started: Home as it is before the first start."),
    s("home-display", "Home", "home-display", "Home scrolled to Display: the Retina mode toggle and its footnote.", dark=True),
    s("home-capabilities", "Home", "home-capabilities", "Home scrolled to Capabilities: Microphone and Gyroscope toggles, both off.", dark=True),
    s("home-capabilities-mic-on", "Home", "home-mic-on", "Capabilities with Microphone on, iOS having allowed it."),
    s("home-capabilities-mic-denied", "Home", "home-mic-denied", "Capabilities after iOS refused the microphone or its app permission was removed: the row is dimmed and says it is not allowed in Settings.", dark=True),
    s("home-capabilities-mic-alert", "Home", "home-mic-alert", "Turning Microphone on while iOS refuses it: the alert that offers Settings."),
    s("home-capabilities-gyroscope-unavailable", "Home", "home-gyroscope-unavailable", "Capabilities on a device without a gyroscope or accelerometer: Gyroscope is disabled and the footnote says why."),
    s("home-backup", "Home", "home-backup", "Home scrolled to Backup: the guest-data backup toggle and its footnote."),
    s("home-recovery-erase", "Home", "home-recovery-erase", "Home scrolled to Recovery and Erase: Start in Recovery and Erase Guest."),
    s("home-storage-grow", "Home", "home-grow", "Storage with a larger size chosen, taking effect at the next start."),
    s("home-storage-max", "Home", "home-max", "Storage at its largest size."),
    s("home-update", "Home", "home-update", "A system update waiting: the card above the Start bar."),
    s("home-update-unfinished", "Home", "home-update-failed", "An update that did not finish: the card offers retry or restoring the saved data."),
    s("home-update-unfinished-restore-only", "Home", "home-update-failed-restore", "An unfinished update that can only be restored."),
    s("home-blocked", "Home", "home-blocked", "Start blocked because the guest is newer than the system in this app."),
    s("home-notice-closed", "Home", "home-closed", "The notice shown after iOS closed Andarium while it was paused."),
    s("home-notice-low-space", "Home", "home-low-space", "The low free space notice on Home."),
    s("home-erased", "Home", "home-erased", "Home right after Erase Guest: the erased notice and a fresh guest."),
    s("busy-create", "Operations", "busy-create", "The overlay while the guest is created: title, detail and progress."),
    s("busy-erase", "Operations", "busy-erase", "The overlay while the guest is erased."),
    s("alert-update-unfinished", "Alerts", "alert-update-unfinished", "Start pressed while an unfinished update blocks it: the alert."),
    s("alert-blocked", "Alerts", "alert-blocked", "Start pressed on a guest newer than the system in this app: the alert."),
    s("alert-error", "Alerts", "alert-error", "An error alert (an operation that failed with the system's message)."),
    # Sheets and dialogs
    s("setup-new-guest", "Setup", "create", "The initial setup sheet: Storage and Backup only; vCPUs and memory are no longer asked.", dark=True),
    s("setup-new-guest-low-space", "Setup", "create-low-space", "The setup sheet when the device lacks the free space the first start needs: the notice and a disabled Create."),
    s("start-low-space-alert", "Setup", "start-low-space", "Start pressed without enough free space: the alert."),
    s("storage-resize", "Sheets", "resize", "The Resize sheet for the guest's storage."),
    s("system-edit", "Sheets", "system-edit", "The System sheet, opened from Edit: vCPUs and memory with the device's limits.", dark=True),
    s("erase", "Sheets", "erase", "The Erase Guest sheet: what is erased and what is kept.", dark=True),
    s("erase-confirmed", "Sheets", "erase-confirm", "The Erase Guest sheet with the loss acknowledged: the red button is enabled."),
    s("info", "Info", "info", "Info: the app and version, System Info (Android Version, Build, Security Patch, Kernel, Architecture) and Acknowledgements.", dark=True),
    s("info-acknowledgements", "Info", "info-acknowledgements", "Acknowledgements: projects by group; the names and marks line is at the bottom.", dark=True),
    s("info-license", "Info", "info-license", "A project's page: copyright holders, license and the license text (Mesa, MIT).", dark=True),
    s("info-license-long", "Info", "info-license-gpl", "A page with a long license text (Linux: syscall exception and GPL v2)."),
    # Boot
    *boot_stages("boot", "boot", ["Andarium", "Linux Kernel", "Storage", "Startup"], "Normal boot over the dimmed guest display, no percentage"),
    s("boot-first-start", "Boot", "boot-first", "First start: Storage prepares the disk and Startup sets up for the first time."),
    s("boot-update", "Boot", "boot-upgrade", "The first start after an update: Startup updates the apps."),
    s("boot-restart", "Boot", "boot-restart", "The guest restarting."),
    *boot_stages("boot-recovery", "boot-recovery", ["Andarium", "Linux Kernel", "Storage", "Recovery", "Screen"], "Recovery boot (opaque, own stage list)", dark_step=3),
    # Running guest
    s("session", "Guest", "session", "The running guest with the side keys closed."),
    s("session-tip", "Guest", "session-tip", "The side-keys tip over the running guest.", "-resetTips", "YES"),
    s("session-side-keys", "Guest", "session-open", "The side keys open: Power, volume, Back, Home and Settings."),
    s("session-left-edge", "Guest", "session-left", "The side keys on the left edge."),
    s("session-volume", "Guest", "session-volume", "The volume panel after a volume key press."),
    s("session-power-menu", "Guest", "session-power", "Android's power menu."),
    s("session-keyboard", "Guest", "session-keyboard", "The guest's own keyboard up over its search."),
    s("session-stats", "Guest", "session-stats", "The CPU, memory and frame rate readout drawn over the guest."),
    s("session-setup", "Guest", "session-setup", "The guest's own first-run setup screen."),
    s("session-off", "Guest", "session-off", "The guest's screen after it powered off."),
    s("recovery", "Guest", "recovery", "Recovery running."),
    s("recovery-side-keys", "Guest", "recovery-open", "Recovery with the side keys open."),
    # Settings during a session
    s("settings", "Settings", "settings", "Settings at full height: the sections from the top.", dark=True),
    s("settings-stats", "Settings", "settings-stats", "Settings scrolled to Stats, Display and Capabilities."),
    s("settings-display", "Settings", "settings-display", "Settings scrolled to Display: Retina mode."),
    s("settings-capabilities", "Settings", "settings-capabilities", "Settings scrolled to Capabilities: Microphone and Gyroscope.", dark=True),
    s("settings-capabilities-mic-denied", "Settings", "settings-mic-denied", "Settings' Capabilities with the microphone refused: dimmed row, not allowed in Settings."),
    s("settings-install-running", "Settings", "settings-installing", "Apps: an install under way."),
    s("settings-install-done", "Settings", "settings-installed", "Apps: an app installed."),
    s("settings-install-not-apk", "Settings", "settings-not-apk", "Apps: a file that is not an app."),
    s("settings-install-bundle", "Settings", "settings-bundle", "Apps: a bundle that needs more than one file."),
    s("settings-install-failed", "Settings", "settings-install-failed", "Apps: an install the guest refused."),
    s("settings-adb", "Settings", "settings-adb", "ADB over Network on: the address a computer connects to."),
    s("settings-adb-no-address", "Settings", "settings-adb-no-address", "ADB over Network on with no network address."),
    s("settings-adb-off", "Settings", "settings-adb-off", "ADB over Network unavailable because USB debugging is off."),
    # Operations on the guest
    s("overlay-bug-report", "Operations", "bug-report", "The full-screen progress while a bug report is made.", wait=2.0),
    s("overlay-install-apk", "Operations", "install-apk", "The full-screen progress while an app installs from Files.", wait=2.0),
    # End
    s("end-off", "End", "off", "The screen after the guest ended: close and reopen Andarium to start again."),
    s("end-failed", "End", "failed", "The screen after the guest stopped unexpectedly."),
]


def run(command: list[str], *, timeout: float = COMMAND_SECONDS) -> subprocess.CompletedProcess[str]:
    """Run a command with a hard time bound; a command that outlives it fails."""
    try:
        return subprocess.run(
            command, check=False, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=timeout
        )
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(command, 124, "", f"timed out after {timeout:.0f} s")


def simctl(*arguments: str, timeout: float = COMMAND_SECONDS) -> subprocess.CompletedProcess[str]:
    return run(["xcrun", "simctl", *arguments], timeout=timeout)


def device_names() -> dict[str, str]:
    result = simctl("list", "devices", "--json")
    if result.returncode != 0:
        sys.exit(f"capture-screens.py: simctl list failed: {result.stderr.strip()}")
    names: dict[str, str] = {}
    for devices in json.loads(result.stdout)["devices"].values():
        for device in devices:
            names[device["udid"]] = device["name"]
    return names


def png_size(path: Path) -> tuple[int, int]:
    header = path.read_bytes()[:24]
    return struct.unpack(">II", header[16:24])


def slug(value: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", value.lower()).strip("-")


def differing_pixels(first: Path, second: Path) -> float:
    """The fraction of pixels that differ between two screenshots (1.0 if unknown)."""
    try:
        result = run(["magick", "compare", "-metric", "AE", "-fuzz", "5%", str(first), str(second), "null:"], timeout=30)
        width, height = png_size(first)
        return float(result.stderr.split()[0]) / (width * height)
    except (OSError, ValueError, IndexError, subprocess.TimeoutExpired):
        return 1.0


def blank(path: Path) -> bool:
    """A plain white frame: an app that has not drawn yet."""
    try:
        result = run(["magick", str(path), "-format", "%[fx:mean] %[fx:standard_deviation]", "info:"], timeout=30)
        mean, deviation = (float(value) for value in result.stdout.split())
        return mean > 0.97 and deviation < 0.02
    except (OSError, ValueError, subprocess.TimeoutExpired):
        return False


def settled_screenshot(udid: str, shot_id: str, out: Path) -> tuple[Path | None, bool]:
    """Screenshot once the screen has stopped changing (a launch animation or a
    scroll is over): two looks a second apart that differ in under 0.3% of
    their pixels. After SETTLE_SECONDS the last look is taken as it is."""
    temporary = out / f".{shot_id}.png"
    previous = out / f".{shot_id}.previous.png"
    deadline = time.monotonic() + SETTLE_SECONDS
    settled = False
    while True:
        taken = simctl("io", udid, "screenshot", str(temporary))
        if taken.returncode != 0 or not temporary.is_file():
            print(f"  {shot_id}: screenshot failed: {taken.stderr.strip()}", file=sys.stderr)
            return None, False
        if previous.is_file() and differing_pixels(previous, temporary) < SETTLED_FRACTION and not blank(temporary):
            settled = True
            break
        if time.monotonic() >= deadline:
            break
        temporary.replace(previous)
        time.sleep(1.0)
    previous.unlink(missing_ok=True)
    return temporary, settled


def capture(udid: str, name: str, shot: Shot, appearance: str, out: Path, wait: float) -> dict | None:
    simctl("terminate", udid, BUNDLE)
    launched = simctl("launch", udid, BUNDLE, *shot.launch_arguments())
    if launched.returncode != 0:
        print(f"  {shot.id}: launch failed: {launched.stderr.strip()}", file=sys.stderr)
        return None
    time.sleep(max(wait, shot.wait))
    temporary, settled = settled_screenshot(udid, shot.id, out)
    if temporary is None:
        return None
    if not settled:
        print(f"  {shot.id}: the screen kept changing; kept the last look", file=sys.stderr)
    width, height = png_size(temporary)
    orientation = "landscape" if width > height else "portrait"
    idiom = "iPad" if "iPad" in name else "iPhone"
    file = f"{idiom.lower()}-{orientation}-{appearance}-{shot.id}.png"
    temporary.replace(out / file)
    return {
        "file": file,
        "device": name,
        "idiom": idiom,
        "orientation": orientation,
        "appearance": appearance,
        "group": shot.group,
        "id": shot.id,
        "screen": shot.screen,
        "launch_arguments": " ".join(shot.launch_arguments()),
        "pixels": [width, height],
        "settled": settled,
        "description": shot.description,
    }


def write_manifest(path: Path, entries: dict[str, dict]) -> None:
    manifest = {"schema": 1, "shots": sorted(entries.values(), key=lambda entry: entry["file"])}
    path.write_text(json.dumps(manifest, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, help="directory for the PNG files and manifest.json")
    parser.add_argument("--device", action="append", default=[], metavar="UDID", help="a booted Simulator (repeatable)")
    parser.add_argument("--only", metavar="REGEX", help="capture only shots whose id matches")
    parser.add_argument("--wait", type=float, default=0.0, help="minimum seconds after each launch")
    parser.add_argument("--list", action="store_true", help="list the shots and exit")
    args = parser.parse_args(argv)
    if args.list:
        for shot in SHOTS:
            print(f"{shot.id}\t{shot.group}\t{' '.join(shot.launch_arguments())}\t{'light,dark' if shot.dark else 'light'}")
        return 0
    if args.out is None or not args.device:
        parser.error("--out and at least one --device are required")
    ids = [shot.id for shot in SHOTS]
    if len(ids) != len(set(ids)):
        sys.exit("capture-screens.py: duplicate shot ids")
    args.out.mkdir(parents=True, exist_ok=True)
    manifest_path = args.out / "manifest.json"
    entries: dict[str, dict] = {}
    if manifest_path.is_file():
        entries = {entry["file"]: entry for entry in json.loads(manifest_path.read_text())["shots"]}
    names = device_names()
    selected = [shot for shot in SHOTS if not args.only or re.search(args.only, shot.id)]
    status = 0
    for udid in args.device:
        name = names.get(udid)
        if name is None:
            sys.exit(f"capture-screens.py: no Simulator {udid}")
        simctl(
            "status_bar", udid, "override", "--time", "9:41", "--batteryState", "charged", "--batteryLevel", "100",
            "--cellularMode", "active", "--cellularBars", "4", "--wifiBars", "3", "--dataNetwork", "wifi",
        )
        for appearance in ("light", "dark"):
            simctl("ui", udid, "appearance", appearance)
            for shot in selected:
                if appearance == "dark" and not shot.dark:
                    continue
                entry = capture(udid, name, shot, appearance, args.out, args.wait)
                if entry is None:
                    entry = capture(udid, name, shot, appearance, args.out, args.wait)
                if entry is None:
                    status = 1
                    continue
                entries[entry["file"]] = entry
                write_manifest(manifest_path, entries)
                print(f"{name} {entry['orientation']} {appearance}: {shot.id}", flush=True)
        simctl("ui", udid, "appearance", "light")
    write_manifest(manifest_path, entries)
    return status


if __name__ == "__main__":
    raise SystemExit(main())
