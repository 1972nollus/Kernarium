#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The adb debugging runner's reading of persist.sys.usb.config.

A fresh data partition of a user build holds `none`, which init's property
service writes into an empty value at boot, and a debugging toggle leaves the
empty string that AdbService writes. Both mean off, by adbd's rule: `adb` is
not one of the comma-separated functions.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import importlib.util
from pathlib import Path
import unittest


SCRIPT = Path(__file__).resolve().parents[2] / "integration/android/run_android_adb_debugging.py"
SPEC = importlib.util.spec_from_file_location("run_android_adb_debugging", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)


class FakeDevice:
    def __init__(self, usb_config: str, adb_enabled: str = "0", developer: str = "null",
                 adbd: str = "running") -> None:
        self.properties = {runner.USB_CONFIG: usb_config, "init.svc.adbd": adbd}
        self.settings = {"adb_enabled": adb_enabled, "development_settings_enabled": developer}

    def getprop(self, name: str) -> str:
        return self.properties[name]

    def setting(self, name: str) -> str:
        return self.settings[name]


class UsbDebuggingRecordedTests(unittest.TestCase):
    def test_adb_is_a_comma_separated_function(self) -> None:
        recorded = {
            "": False,
            "none": False,
            "mtp": False,
            "adbx": False,
            "xadb": False,
            "adb": True,
            "adb,mtp": True,
            "mtp,adb": True,
            "none,adb": True,
        }
        for value, expected in recorded.items():
            with self.subTest(value=value):
                self.assertEqual(runner.usb_debugging_recorded(value), expected)


class FreshPartitionTests(unittest.TestCase):
    def test_empty_and_none_are_a_fresh_user_build(self) -> None:
        for value in ("", "none"):
            with self.subTest(value=value):
                runner.check_fresh(FakeDevice(value))

    def test_adb_on_a_fresh_partition_fails(self) -> None:
        for value in ("adb", "mtp,adb"):
            with self.subTest(value=value), self.assertRaises(runner.Failure) as raised:
                runner.check_fresh(FakeDevice(value))
            self.assertIn(value, str(raised.exception))

    def test_the_other_fresh_state_checks_still_fail(self) -> None:
        for device in (FakeDevice("none", adb_enabled="1"),
                       FakeDevice("none", developer="1"),
                       FakeDevice("none", adbd="stopped")):
            with self.assertRaises(runner.Failure):
                runner.check_fresh(device)


class RootDevice:
    """A guest as `check_root_refused` sees it: `adb root`'s answer and what follows it."""

    serial = "127.0.0.1:1"

    def __init__(self, answer: str = "adbd cannot run as root in production builds", *,
                 debuggable: str = "0", hang: bool = False, state: str = "device",
                 uid: str = "2000", pids: tuple[str, str] = ("500", "500")) -> None:
        self.answer, self.debuggable, self.hang, self.state, self.uid = answer, debuggable, hang, state, uid
        self.pids = list(pids)

    def getprop(self, name: str) -> str:
        return {"ro.debuggable": self.debuggable, "ro.build.type": "user"}[name]

    def run(self, *arguments: str, timeout: int = 30):
        assert arguments[-1] == "root"
        if self.hang:
            raise runner.subprocess.TimeoutExpired(arguments, timeout)
        return runner.subprocess.CompletedProcess(arguments, 1, stdout=self.answer + "\n")

    def devices_line(self) -> str:
        return self.state

    def shell(self, command: str) -> str:
        assert command == "id -u"
        return self.uid

    def adbd_pid(self) -> str:
        return self.pids.pop(0)


class RootRefusedTests(unittest.TestCase):
    def test_a_refusal_that_keeps_the_connection_passes(self) -> None:
        for answer in ("adbd cannot run as root in production builds",
                       "ADB Root access is disabled by system setting - enable in Settings"):
            with self.subTest(answer=answer):
                runner.check_root_refused(RootDevice(answer))

    def test_an_announced_restart_that_never_comes_fails(self) -> None:
        for device in (RootDevice("restarting adbd as root"), RootDevice(hang=True)):
            with self.assertRaises(runner.Failure) as raised:
                runner.check_root_refused(device)
            self.assertIn("root", str(raised.exception))

    def test_a_lost_connection_or_a_changed_adbd_fails(self) -> None:
        for device in (RootDevice(state="offline"), RootDevice(uid="0"), RootDevice(pids=("500", "501"))):
            with self.assertRaises(runner.Failure):
                runner.check_root_refused(device)

    def test_a_debuggable_build_is_not_asked(self) -> None:
        runner.check_root_refused(RootDevice("restarting adbd as root", debuggable="1"))


if __name__ == "__main__":
    unittest.main()
