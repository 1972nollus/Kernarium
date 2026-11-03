#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Qualify an Android release's adb debugging setting and adbd's key rule.

The guest is the release on a fresh data disk, started with the macOS launcher
(tools/boot_android.py), or any running guest named by --serial. The contract
is android/lineage-24/README.md (the p0007 and p0011 adb topics):

- Developer options and USB debugging are off on a fresh data partition:
  `adb_enabled` is 0 (or unset) and `persist.sys.usb.config` is empty;
- turning USB debugging on records `adb` in `persist.sys.usb.config` and keeps
  the host's adbd running, the same process;
- turning it off clears the property, ends the sessions by restarting adbd
  once, and leaves adbd running for the host, which reconnects;
- both settings survive a guest reboot;
- when the guest checks keys (`ro.boot.kernarium.adb_auth` is 1, which the
  macOS launcher never sets, so only a guest the Kernarium runtime started),
  a second throwaway key stays unauthorized with no prompt while debugging is
  off, and raises the prompt while it is on. The host's own key is the one this
  runner's adb uses throughout and is never refused.

Every wait has a deadline; the guest's launcher is stopped through its own
process on any exit.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
import pathlib
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from typing import Callable

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
LAUNCHER = REPOSITORY / "tools" / "boot_android.py"
SETTLE_SECONDS = 30
RECONNECT_SECONDS = 90
REFUSAL_LINE = "kernarium: key refused, debugging is off"
PROMPT_ACTIVITY = "UsbDebuggingActivity"


class Failure(RuntimeError):
    pass


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
        result = self.run("-s", self.serial, "shell", command, timeout=timeout)
        if result.returncode != 0:
            raise Failure(f"'{command}' exited {result.returncode}: {result.stdout.strip()}")
        return result.stdout.strip()

    def getprop(self, name: str) -> str:
        return self.shell(f"getprop {name}")

    def setting(self, name: str) -> str:
        return self.shell(f"settings get global {name}")

    def adbd_pid(self) -> str:
        return self.shell("pidof adbd")

    def turn_debugging_off(self) -> None:
        """Set adb_enabled to 0; the restart that follows may end this very session."""
        try:
            self.run("-s", self.serial, "shell", "settings put global adb_enabled 0", timeout=30)
        except subprocess.TimeoutExpired:
            pass

    def devices_line(self) -> str:
        listing = self.run("devices", timeout=15).stdout
        for line in listing.splitlines():
            if line.startswith(self.serial):
                return line.split()[-1]
        return "absent"


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


def boot_id(device: Device) -> str:
    return device.shell("cat /proc/sys/kernel/random/boot_id")


def wait_booted(device: Device, seconds: int, guest: subprocess.Popen | None,
                previous_boot: str | None = None) -> None:
    def booted() -> bool:
        if not device.connect() or device.getprop("sys.boot_completed") != "1":
            return False
        return previous_boot is None or boot_id(device) != previous_boot
    wait_for("the guest to be booted", booted, seconds, guest)


def reboot(device: Device, seconds: int, guest: subprocess.Popen | None) -> None:
    previous = boot_id(device)
    device.run("-s", device.serial, "reboot", timeout=30)
    wait_booted(device, seconds, guest, previous)


def free_loopback_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def check_fresh(device: Device) -> None:
    enabled = device.setting("adb_enabled")
    if enabled not in ("0", "null"):
        raise Failure(f"adb_enabled is {enabled!r} on a fresh data partition")
    if device.getprop("persist.sys.usb.config"):
        raise Failure("persist.sys.usb.config is set on a fresh data partition")
    developer = device.setting("development_settings_enabled")
    if developer not in ("0", "null"):
        raise Failure(f"Developer options are on ({developer!r}) on a fresh data partition")
    if device.getprop("init.svc.adbd") != "running":
        raise Failure("adbd is not running for the host")


def check_toggle(device: Device) -> None:
    pid = device.adbd_pid()
    device.shell("settings put global adb_enabled 1")
    wait_for("persist.sys.usb.config to record the debugging setting",
             lambda: device.getprop("persist.sys.usb.config") == "adb", SETTLE_SECONDS)
    if device.setting("adb_enabled") != "1":
        raise Failure("adb_enabled is not 1 after turning USB debugging on")
    if device.adbd_pid() != pid:
        raise Failure("turning debugging on restarted adbd")
    print("toggle on: recorded, adbd untouched")

    device.turn_debugging_off()
    # The restart ends every session, the host's included.
    wait_for("adbd to restart and the host to reconnect",
             lambda: device.connect() and device.adbd_pid() != pid, RECONNECT_SECONDS)
    wait_for("persist.sys.usb.config to be cleared",
             lambda: device.getprop("persist.sys.usb.config") == "", SETTLE_SECONDS)
    if device.getprop("init.svc.adbd") != "running":
        raise Failure("adbd is not running after debugging was turned off")
    if device.setting("adb_enabled") != "0":
        raise Failure("adb_enabled is not 0 after turning USB debugging off")
    print("toggle off: cleared, adbd restarted once and serving the host")


def check_reboot(device: Device, seconds: int, guest: subprocess.Popen | None) -> None:
    device.shell("settings put global adb_enabled 1")
    wait_for("persist.sys.usb.config to record the setting",
             lambda: device.getprop("persist.sys.usb.config") == "adb", SETTLE_SECONDS)
    reboot(device, seconds, guest)
    wait_for("adb_enabled to be 1 after the reboot", lambda: device.setting("adb_enabled") == "1",
             SETTLE_SECONDS)
    if device.getprop("persist.sys.usb.config") != "adb":
        raise Failure("persist.sys.usb.config lost 'adb' across a reboot")
    device.turn_debugging_off()
    wait_for("the host to reconnect after turning debugging off",
             lambda: device.connect() and device.getprop("persist.sys.usb.config") == "", RECONNECT_SECONDS)
    reboot(device, seconds, guest)
    wait_for("adb_enabled to be 0 after the reboot", lambda: device.setting("adb_enabled") == "0",
             SETTLE_SECONDS)
    if device.getprop("persist.sys.usb.config"):
        raise Failure("persist.sys.usb.config came back after a reboot")
    print("reboot: both settings persist")


def check_key_rule(device: Device, scratch: pathlib.Path) -> None:
    """A second key: refused with no prompt while debugging is off, prompted while on."""
    key = scratch / "throwaway_adbkey"
    other_environment = dict(device.environment)
    other_environment["ADB_VENDOR_KEYS"] = str(key)
    other_environment["ANDROID_ADB_SERVER_PORT"] = str(free_loopback_port())
    stranger = Device(device.adb, device.serial, other_environment)
    created = stranger.run("keygen", str(key), timeout=30)
    if created.returncode != 0 or not key.is_file():
        raise Failure(f"adb keygen failed: {created.stdout.strip()}")

    def stranger_state() -> str:
        stranger.run("connect", stranger.serial, timeout=15)
        return stranger.devices_line()

    try:
        device.shell("logcat -c")
        wait_for("the second key to reach adbd", lambda: stranger_state() == "unauthorized", SETTLE_SECONDS)
        time.sleep(10)
        if stranger_state() != "unauthorized":
            raise Failure("the second key authenticated while debugging is off")
        if REFUSAL_LINE not in device.shell("logcat -d"):
            raise Failure(f"adbd did not log {REFUSAL_LINE!r} for the second key")
        if PROMPT_ACTIVITY in device.shell("dumpsys activity activities"):
            raise Failure("the second key raised the debugging prompt while debugging is off")
        print("key rule, debugging off: unauthorized, refused, no prompt")

        device.shell("settings put global adb_enabled 1")
        wait_for("persist.sys.usb.config to record the setting",
                 lambda: device.getprop("persist.sys.usb.config") == "adb", SETTLE_SECONDS)
        stranger.run("disconnect", stranger.serial, timeout=15)
        wait_for("the second key to be offered the prompt",
                 lambda: stranger_state() == "unauthorized"
                 and PROMPT_ACTIVITY in device.shell("dumpsys activity activities"),
                 SETTLE_SECONDS)
        print("key rule, debugging on: unauthorized until accepted, prompt raised")
    finally:
        stranger.run("kill-server", timeout=15)
        device.turn_debugging_off()


def stop_launcher(guest: subprocess.Popen) -> None:
    if guest.poll() is None:
        guest.send_signal(signal.SIGTERM)
        try:
            guest.wait(timeout=120)
        except subprocess.TimeoutExpired:
            guest.kill()
            guest.wait(timeout=30)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--release", type=pathlib.Path, help="release directory (default: the accepted release)")
    parser.add_argument("--serial", help="a running guest on a fresh data partition (host:port) that "
                        "accepts this adb's key; no guest is launched")
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--emulator", type=pathlib.Path, help="the Android macOS executable (the launcher's default)")
    parser.add_argument("--adb-port", type=int, default=5575, help="loopback port the launcher forwards adbd to")
    parser.add_argument("--cpus", type=int, default=4)
    parser.add_argument("--boot-timeout", type=int, default=900)
    options = parser.parse_args()

    adb = shutil.which(options.adb) or options.adb
    guest: subprocess.Popen | None = None
    with tempfile.TemporaryDirectory(prefix="android-adb-debugging-") as scratch_name:
        scratch = pathlib.Path(scratch_name)
        environment = dict(os.environ)
        environment["ANDROID_ADB_SERVER_PORT"] = str(free_loopback_port())
        log_path = scratch / "launcher.log"
        try:
            if options.serial:
                serial = options.serial
            else:
                serial = f"127.0.0.1:{options.adb_port}"
                command = [sys.executable, str(LAUNCHER), "--instance", str(scratch / "instance"),
                           "--adb", adb, "--adb-port", str(options.adb_port), "--display", "offscreen",
                           "--cpus", str(options.cpus), "--boot-timeout", str(options.boot_timeout)]
                if options.release:
                    command += ["--release", str(options.release)]
                if options.emulator:
                    command += ["--emulator", str(options.emulator)]
                launcher_environment = {**os.environ, "PYTHONDONTWRITEBYTECODE": "1"}
                with open(log_path, "wb") as log:
                    guest = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log,
                                             stderr=subprocess.STDOUT, env=launcher_environment)
            device = Device(adb, serial, environment)
            wait_booted(device, options.boot_timeout, guest)
            check_fresh(device)
            print("fresh data: debugging off, adbd serving the host")
            check_toggle(device)
            check_reboot(device, options.boot_timeout, guest)
            if device.getprop("ro.boot.kernarium.adb_auth") == "1":
                check_key_rule(device, scratch)
            else:
                print("key rule: skipped, this guest does not check keys (ro.boot.kernarium.adb_auth is not 1)")
            print("PASS")
            return 0
        except (Failure, subprocess.TimeoutExpired, OSError) as problem:
            print(f"FAIL: {problem}", file=sys.stderr)
            if log_path.is_file():
                print("launcher log tail:\n" + "\n".join(log_path.read_text(errors="replace").splitlines()[-30:]),
                      file=sys.stderr)
            return 1
        finally:
            if guest is not None:
                stop_launcher(guest)
            subprocess.run([adb, "kill-server"], env=environment, stdin=subprocess.DEVNULL,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30, check=False)


if __name__ == "__main__":
    raise SystemExit(main())
