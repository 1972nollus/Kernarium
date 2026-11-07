#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Qualify an Android release's first boot and the harness helper that skips it.

The product starts a fresh guest in LineageOS Setup Wizard. This runner boots
the release twice with the macOS launcher (tools/boot_android.py), each time on
a fresh instance, and checks the contract in android/lineage-24/README.md:

- wizard: the guest is unprovisioned (`ro.setupwizard.mode` OPTIONAL,
  `device_provisioned` and `user_setup_complete` 0), HOME resolves to the
  wizard and stays there when pressed, and the wizard's first page is on
  screen. The runner then completes the wizard through the pages the product
  shows, exactly these and in this order: Welcome, Language, Date and time,
  Theme, Finish. Setup ends in the state the product defines for a fresh guest:
  both settings 1, the wizard's HOME activity and flow manager disabled, HOME
  resolving to, and pressed back to, the launcher, three-button navigation
  (`navigation_mode` 0, the gestural overlay off and both mode overlays
  present), no screen lock, location on as it was before the wizard, Wi-Fi
  connected to VirtWifi, and no status bar lock held by the wizard;
- helper: the guest is started with `--provision`, the harness path of
  tools/android_provision.py, and reaches the same observable state without a
  person: both settings 1 and HOME on the launcher. The shell may not disable
  the wizard's components, so the helper disables the wizard package for user 0
  instead, and stops its process, which holds a status bar lock until it dies.
  The helper then finds nothing left to do.

The flow is the contract of release r17 and later: a release that still shows
LineageOS's other pages fails at the first page that is not in the list.

Every wait has a deadline; the guest's launcher is stopped through its own
process on any exit.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
import pathlib
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ElementTree
from typing import Callable

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
LAUNCHER = REPOSITORY / "tools" / "boot_android.py"
sys.path.insert(0, str(REPOSITORY / "tools"))
import android_provision  # noqa: E402

WIZARD = android_provision.SETUP_WIZARD
LAUNCHER_PACKAGE = "com.android.launcher3"
WELCOME_ACTIVITY = f"{WIZARD}/.WelcomeActivity"
LOCALE_ACTIVITY = f"{WIZARD}/.locale.LocaleActivity"
DATETIME_ACTIVITY = f"{WIZARD}/.time.DateTimeActivity"
THEME_ACTIVITY = f"{WIZARD}/.settings.ThemeSettingsActivity"
FINISH_ACTIVITY = f"{WIZARD}/.FinishActivity"
# The reviewed flow: the pages the wizard shows, in order.
FLOW = (WELCOME_ACTIVITY, LOCALE_ACTIVITY, DATETIME_ACTIVITY, THEME_ACTIVITY, FINISH_ACTIVITY)
NAVBAR_GESTURAL = "com.android.internal.systemui.navbar.gestural"
NAVBAR_3BUTTON = "com.android.internal.systemui.navbar.threebutton"
WIFI_NETWORK = "VirtWifi"
UI_DUMP = "/data/local/tmp/andarium-oobe-ui.xml"
BOUNDS = re.compile(r"\[(\d+),(\d+)\]\[(\d+),(\d+)\]")
RESUMED = re.compile(r"ResumedActivity: ActivityRecord\{\S+ u\d+ (\S+) t\d+\}")
ADB_LOST = re.compile(r"offline|device not found|no devices|closed|unauthorized", re.IGNORECASE)
RECONNECT_ATTEMPTS = 8
# What the wizard disables itself when its last page finishes.
WIZARD_DISABLES = ("SetupWizardActivity", "wizardmanager.WizardManager")
WIZARD_STEPS = 20


class Failure(RuntimeError):
    pass


# --- the wizard's pages ---------------------------------------------------------


def resumed_activity(dumpsys: str) -> str:
    """The activity at the top of the resumed stack, `package/.Class`, or an empty string."""
    match = RESUMED.search(dumpsys)
    return match.group(1) if match else ""


def parse_nodes(hierarchy: str) -> list[dict[str, str]]:
    """The attributes of every node of a uiautomator dump."""
    start = hierarchy.find("<?xml")
    if start < 0:
        start = hierarchy.find("<hierarchy")
    try:
        root = ElementTree.fromstring(hierarchy[start:] if start >= 0 else hierarchy)
    except ElementTree.ParseError as error:
        raise Failure(f"the UI dump is not XML: {error}") from error
    return [dict(node.attrib) for node in root.iter("node")]


def center(bounds: str) -> tuple[int, int]:
    match = BOUNDS.fullmatch(bounds)
    if match is None:
        raise Failure(f"unreadable bounds {bounds!r}")
    left, top, right, bottom = map(int, match.groups())
    return (left + right) // 2, (top + bottom) // 2


# What each page of the wizard is asked: the id of its button, the text of its
# button, its first list item, or the page's home affordance (in three-button
# navigation the Finish page's icon is the control that goes home).
PAGES: dict[str, tuple[str, str]] = {
    WELCOME_ACTIVITY: ("id", f"{WIZARD}:id/start"),
    LOCALE_ACTIVITY: ("item", "sud_items_title"),
    DATETIME_ACTIVITY: ("text", "Next"),
    # The last page before Finish: its button reads Finish.
    THEME_ACTIVITY: ("text", "Finish"),
    FINISH_ACTIVITY: ("id", f"{WIZARD}:id/swipe_hint_icon"),
}
assert tuple(PAGES) == FLOW


def wizard_action(activity: str, nodes: list[dict[str, str]]) -> tuple[str, int, int]:
    """What to do on this page: ("tap", x, y)."""
    rule = PAGES.get(activity)
    if rule is None:
        raise Failure(f"the wizard shows a page this runner does not know: {activity!r}")
    kind, target = rule
    for node in nodes:
        if kind == "id" and node.get("resource-id") == target:
            return ("tap", *center(node["bounds"]))
        if kind == "text" and node.get("text") == target:
            return ("tap", *center(node["bounds"]))
        if kind == "item" and node.get("resource-id", "").endswith(target):
            return ("tap", *center(node["bounds"]))
    raise Failure(f"{activity} has no {kind} {target!r} to tap")


def next_page(position: int, activity: str) -> int:
    """The index in FLOW after seeing `activity` while the flow stands at `position`.

    The flow is strict: the page on screen is the one expected, or the one after
    it (the tap went through). Any other page, a hidden one or a reviewed one out
    of order, is a failure."""
    if activity == FLOW[position]:
        return position
    if position + 1 < len(FLOW) and activity == FLOW[position + 1]:
        return position + 1
    expected = FLOW[position] if position + 1 >= len(FLOW) else f"{FLOW[position]} or {FLOW[position + 1]}"
    raise Failure(f"the wizard shows {activity!r}, expected {expected}")


OVERLAY_LINE = re.compile(r"^(?:\[(?P<state>[ x])\]|(?P<missing>---)) (?P<package>\S+)$", re.MULTILINE)
WIFI_INFO = re.compile(r'mWifiInfo SSID: "(?P<ssid>[^"]*)".*Supplicant state: (?P<state>[A-Z_]+)')


def overlay_states(listing: str) -> dict[str, str]:
    """`cmd overlay list` as package -> "x" (enabled), " " (disabled) or "-" (not usable)."""
    return {match.group("package"): match.group("state") or "-" for match in OVERLAY_LINE.finditer(listing)}


def wifi_network(dumpsys_wifi: str) -> tuple[str, str] | None:
    """The connected network's name and supplicant state from `dumpsys wifi`, or None."""
    match = WIFI_INFO.search(dumpsys_wifi)
    return (match.group("ssid"), match.group("state")) if match else None


def disabled_components(dumpsys_package: str) -> set[str]:
    """The component class names the wizard package lists as disabled for user 0."""
    lines = dumpsys_package.splitlines()
    names: set[str] = set()
    for index, line in enumerate(lines):
        if line.strip() == "disabledComponents:":
            indent = len(line) - len(line.lstrip())
            for following in lines[index + 1:]:
                if len(following) - len(following.lstrip()) <= indent or not following.strip():
                    break
                names.add(following.strip())
            break
    return names


# --- the guest ------------------------------------------------------------------


class Device:
    """The guest as the host's adb sees it, over one server and one key."""

    def __init__(self, adb: str, serial: str, environment: dict[str, str]) -> None:
        self.adb = adb
        self.serial = serial
        self.environment = environment

    def run(self, *arguments: str, timeout: int = 30) -> subprocess.CompletedProcess:
        return subprocess.run(
            [self.adb, *arguments], env=self.environment, text=True,
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=timeout, check=False)

    def connect(self) -> bool:
        try:
            self.run("connect", self.serial, timeout=15)
            state = self.run("-s", self.serial, "get-state", timeout=15)
        except subprocess.TimeoutExpired:
            return False
        return state.returncode == 0 and state.stdout.strip() == "device"

    def shell(self, command: str, timeout: int = 30) -> str:
        # A user build restarts adbd when its first-boot work ends, which drops
        # the session for a moment: reconnect and ask again before failing.
        for attempt in range(RECONNECT_ATTEMPTS):
            result = self.run("-s", self.serial, "shell", command, timeout=timeout)
            if result.returncode == 0:
                return result.stdout.replace("\r", "").strip()
            if attempt == RECONNECT_ATTEMPTS - 1 or ADB_LOST.search(result.stdout) is None:
                break
            time.sleep(3)
            self.connect()
        raise Failure(f"'{command}' exited {result.returncode}: {result.stdout.strip()}")

    def setting(self, namespace: str, name: str) -> str:
        return self.shell(f"settings get {namespace} {name}")

    def resumed(self) -> str:
        return resumed_activity(self.shell("dumpsys activity activities"))

    def home_package(self) -> str:
        return android_provision.home_package(lambda command: self.shell(command))

    def ui_nodes(self) -> list[dict[str, str]]:
        return parse_nodes(self.shell(
            f"rm -f {UI_DUMP}; uiautomator dump {UI_DUMP} >/dev/null 2>&1; cat {UI_DUMP}; rm -f {UI_DUMP}",
            timeout=60))

    def wizard_status_bar_record(self) -> bool | None:
        return android_provision.wizard_status_bar_record(self.shell)

    def wizard_processes(self) -> list[str] | None:
        return android_provision.wizard_processes(self.shell)


def wait_for(description: str, probe: Callable[[], bool], seconds: int,
             guest: subprocess.Popen | None = None) -> None:
    """Poll the probe until it is true; a probe that raises is not true yet."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if guest is not None and guest.poll() is not None:
            raise Failure(f"the guest's launcher exited ({guest.returncode}) while waiting for {description}")
        try:
            if probe():
                return
        except (Failure, subprocess.TimeoutExpired):
            pass
        time.sleep(1)
    raise Failure(f"{description}: not within {seconds} s")


def wait_booted(device: Device, seconds: int, guest: subprocess.Popen | None) -> None:
    wait_for("the guest to be booted",
             lambda: device.connect() and device.shell("getprop sys.boot_completed") == "1",
             seconds, guest)


def free_loopback_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def stop_launcher(guest: subprocess.Popen) -> None:
    if guest.poll() is None:
        guest.send_signal(signal.SIGTERM)
        try:
            guest.wait(timeout=120)
        except subprocess.TimeoutExpired:
            guest.kill()
            guest.wait(timeout=30)


# --- the checks -----------------------------------------------------------------


# What the wizard's Location page used to write and the product's flow no longer shows:
# the platform's values stay as they were before the wizard ran.
LOCATION_SETTINGS = (("secure", "location_mode"), ("global", "assisted_gps_enabled"))


def location_state(device: Device) -> dict[str, str]:
    return {f"{namespace} {name}": device.setting(namespace, name) for namespace, name in LOCATION_SETTINGS}


def check_unprovisioned(device: Device) -> dict[str, str]:
    """A fresh guest is in Setup Wizard, which HOME does not leave; returns its location settings."""
    mode = device.shell("getprop ro.setupwizard.mode")
    if mode != "OPTIONAL":
        raise Failure(f"ro.setupwizard.mode is {mode!r}, expected OPTIONAL")
    for namespace, name in (("global", "device_provisioned"), ("secure", "user_setup_complete")):
        value = device.setting(namespace, name)
        if value != "0":
            raise Failure(f"{name} is {value!r} on a fresh data partition, expected 0")
    wait_for("the wizard's welcome page", lambda: device.resumed() == WELCOME_ACTIVITY, 90)
    if device.home_package() != WIZARD:
        raise Failure(f"HOME resolves to {device.home_package()!r} on a fresh guest, expected the wizard")
    device.shell("input keyevent KEYCODE_HOME")
    time.sleep(3)
    if device.resumed() != WELCOME_ACTIVITY:
        raise Failure(f"HOME left the wizard for {device.resumed()!r} on an unprovisioned guest")
    print("fresh guest: unprovisioned, HOME stays in the wizard")
    return location_state(device)


def ui_nodes(device: Device) -> list[dict[str, str]]:
    """The page's nodes; a dump taken while the page changes is not XML, so ask again."""
    for attempt in range(3):
        try:
            return device.ui_nodes()
        except Failure:
            if attempt == 2:
                raise
            time.sleep(2)
    return []


def complete_wizard(device: Device) -> None:
    """Answer the wizard's pages until the launcher is on screen, and only these pages, in order."""
    position = 0
    unknown = 0
    answered: set[int] = set()
    for _ in range(WIZARD_STEPS):
        activity = device.resumed()
        if activity.startswith(f"{LAUNCHER_PACKAGE}/"):
            if position != len(FLOW) - 1 or position not in answered:
                raise Failure(f"the wizard ended at the launcher after {FLOW[position]}, before the whole flow "
                              f"{' > '.join(page.split('/')[-1] for page in FLOW)}")
            print(f"wizard completed through {len(FLOW)} pages")
            return
        # A page that is not in the flow may be a transition: look again before
        # calling it unexpected.
        if activity not in FLOW:
            unknown += 1
            if unknown > 3:
                raise Failure(f"the wizard shows a page the product does not: {activity!r}")
            time.sleep(3)
            continue
        unknown = 0
        position = next_page(position, activity)
        action = wizard_action(activity, ui_nodes(device))
        device.shell(f"input tap {action[1]} {action[2]}")
        answered.add(position)
        # Give the page time to change before the next look.
        time.sleep(4)
    raise Failure(f"the wizard did not finish in {WIZARD_STEPS} steps; at {device.resumed()!r}")


def check_defaults(device: Device, before: dict[str, str] | None) -> None:
    """The platform's defaults of a fresh guest, which the shortened flow leaves alone."""
    mode = device.setting("secure", "navigation_mode")
    if mode != "0":
        raise Failure(f"navigation_mode is {mode!r}, expected 0 (three buttons): the wizard may not choose the mode")
    states = overlay_states(device.shell("cmd overlay list android"))
    for package in (NAVBAR_GESTURAL, NAVBAR_3BUTTON):
        if package not in states:
            raise Failure(f"the navigation mode overlay {package} is not installed: {states}")
    if states[NAVBAR_GESTURAL] == "x":
        raise Failure(f"the gestural navigation overlay is enabled: {states}")
    disabled = device.shell("locksettings get-disabled")
    if disabled != "true":
        raise Failure(f"locksettings get-disabled is {disabled!r}, expected true (no screen lock)")
    location = location_state(device)
    if location["secure location_mode"] != "3":
        raise Failure(f"location is not on: {location}")
    if before is not None and location != before:
        raise Failure(f"the wizard changed the location settings: before {before}, after {location}")
    wait_for(f"Wi-Fi connected to {WIFI_NETWORK}",
             lambda: wifi_network(device.shell("dumpsys wifi | grep 'mWifiInfo SSID'")) == (WIFI_NETWORK, "COMPLETED"), 60)
    print(f"defaults: three buttons, no screen lock, location {location}, Wi-Fi on {WIFI_NETWORK}")


def check_wizard_released(device: Device, *, by_wizard: bool) -> None:
    """The wizard holds no status bar lock: no disable record, and for the helper no process either.

    A wizard that ran to its last page may take a moment to let go; a lock that stays is
    the empty status bar and the single Back key of a guest nobody can use."""
    wait_for("the wizard to hold no status bar disable record", lambda: device.wizard_status_bar_record() is False, 30)
    if not by_wizard and device.wizard_processes() != []:
        raise Failure(f"the setup wizard still runs after the helper: {device.wizard_processes()}")
    print("the wizard holds no status bar lock")


def check_provisioned(device: Device, *, by_wizard: bool, before: dict[str, str] | None = None) -> None:
    """The end state: both settings 1, and HOME the launcher, pressed or not.

    The wizard ends by disabling its HOME activity and its flow manager, and
    leaves the package enabled. The shell may not change another package's
    component state, so the helper disables the wizard package for user 0.
    `before` is the wizard guest's location settings from before the wizard.
    """
    for namespace, name in (("global", "device_provisioned"), ("secure", "user_setup_complete")):
        value = device.setting(namespace, name)
        if value != "1":
            raise Failure(f"{name} is {value!r} after setup, expected 1")
    if device.home_package() != LAUNCHER_PACKAGE:
        raise Failure(f"HOME resolves to {device.home_package()!r} after setup, expected the launcher")
    device.shell("input keyevent KEYCODE_HOME")
    wait_for("HOME to reach the launcher", lambda: device.resumed().startswith(f"{LAUNCHER_PACKAGE}/"), 30)
    package_disabled = f"package:{WIZARD}" in device.shell("pm list packages -d").splitlines()
    if by_wizard:
        if package_disabled:
            raise Failure("the wizard package is disabled after the wizard finished, not only its activities")
        disabled = disabled_components(device.shell(f"dumpsys package {WIZARD}"))
        missing = {name for name in WIZARD_DISABLES if f"{WIZARD}.{name}" not in disabled}
        if missing:
            raise Failure(f"the wizard did not disable {sorted(missing)}; disabled: {sorted(disabled)}")
    elif not package_disabled:
        raise Failure("the helper left the wizard package enabled")
    print("provisioned: settings 1, HOME is the launcher, the wizard "
          + ("disabled its HOME activity and manager" if by_wizard else "package is disabled"))
    check_wizard_released(device, by_wizard=by_wizard)
    check_defaults(device, before)


# --- the runner -----------------------------------------------------------------


def launch(options: argparse.Namespace, scratch: pathlib.Path, name: str, adb: str,
           extra: list[str]) -> tuple[subprocess.Popen, pathlib.Path]:
    serial_port = options.adb_port
    log_path = scratch / f"{name}-launcher.log"
    command = [sys.executable, str(LAUNCHER), "--instance", str(scratch / f"{name}-instance"),
               "--adb", adb, "--adb-port", str(serial_port), "--display", "offscreen",
               "--cpus", str(options.cpus), "--boot-timeout", str(options.boot_timeout), *extra]
    if options.release:
        command += ["--release", str(options.release)]
    if options.emulator:
        command += ["--emulator", str(options.emulator)]
    with open(log_path, "wb") as log:
        guest = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                 env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"})
    return guest, log_path


def run_guest(options: argparse.Namespace, scratch: pathlib.Path, name: str, adb: str,
              environment: dict[str, str], extra: list[str],
              exercise: Callable[[Device, pathlib.Path], None]) -> None:
    guest, log_path = launch(options, scratch, name, adb, extra)
    try:
        device = Device(adb, f"127.0.0.1:{options.adb_port}", environment)
        wait_booted(device, options.boot_timeout, guest)
        exercise(device, log_path)
    except (Failure, subprocess.TimeoutExpired, OSError):
        if log_path.is_file():
            print(f"{name} launcher log tail:\n" + "\n".join(log_path.read_text(errors="replace").splitlines()[-30:]),
                  file=sys.stderr)
        raise
    finally:
        stop_launcher(guest)


def wizard_guest(device: Device, _log: pathlib.Path) -> None:
    before = check_unprovisioned(device)
    complete_wizard(device)
    check_provisioned(device, by_wizard=True, before=before)


def helper_guest(device: Device, log: pathlib.Path) -> None:
    # The launcher provisions right after sys.boot_completed; the home screen
    # follows within seconds.
    wait_for("the launcher's provisioning", lambda: "Guest provisioned." in log.read_text(errors="replace"), 120)
    check_provisioned(device, by_wizard=False)
    if android_provision.provision(lambda command: device.shell(command)):
        raise Failure("the helper changed a guest that was already provisioned")
    print("helper: nothing left to do on a provisioned guest")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--release", type=pathlib.Path, help="release directory (default: the accepted release)")
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--emulator", type=pathlib.Path, help="the Android macOS executable (the launcher's default)")
    parser.add_argument("--adb-port", type=int, default=5576, help="loopback port the launcher forwards adbd to")
    parser.add_argument("--cpus", type=int, default=4)
    parser.add_argument("--boot-timeout", type=int, default=900)
    parser.add_argument("--only", choices=("wizard", "helper"), help="run one of the two guests")
    options = parser.parse_args()

    adb = shutil.which(options.adb) or options.adb
    with tempfile.TemporaryDirectory(prefix="android-oobe-") as scratch_name:
        scratch = pathlib.Path(scratch_name)
        environment = dict(os.environ)
        environment["ANDROID_ADB_SERVER_PORT"] = str(free_loopback_port())
        try:
            if options.only != "helper":
                run_guest(options, scratch, "wizard", adb, environment, [], wizard_guest)
            if options.only != "wizard":
                run_guest(options, scratch, "helper", adb, environment, ["--provision"], helper_guest)
            print("PASS")
            return 0
        except (Failure, subprocess.TimeoutExpired, OSError) as problem:
            print(f"FAIL: {problem}", file=sys.stderr)
            return 1
        finally:
            subprocess.run([adb, "kill-server"], env=environment, stdin=subprocess.DEVNULL,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30, check=False)


if __name__ == "__main__":
    raise SystemExit(main())
