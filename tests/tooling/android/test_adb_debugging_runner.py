#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The adb debugging runner's reading of persist.sys.usb.config and of `adb root`.

A fresh data partition of a user build holds `none`, which init's property
service writes into an empty value at boot, and a debugging toggle leaves the
empty string that AdbService writes. Both mean off, by adbd's rule: `adb` is
not one of the comma-separated functions.

`adb root` on a user build is answered as AOSP answers it. The runner requires
that answer, and the adb route's series (p0011) is checked to test the build
before it asks LineageOS's adb_root service.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import importlib.util
from pathlib import Path
import unittest


SCRIPT = Path(__file__).resolve().parents[2] / "integration/android/run_android_adb_debugging.py"
ADB_SERIES = Path(__file__).resolve().parents[3] / "android/lineage-24/patches/projects/p0011/series"
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
    def test_the_production_build_refusal_that_keeps_the_connection_passes(self) -> None:
        runner.check_root_refused(RootDevice("adbd cannot run as root in production builds"))

    def test_another_answer_fails_and_the_failure_names_it(self) -> None:
        # LineageOS's message for a switch that is off points at a Settings page a user build lacks.
        for answer in ("ADB Root access is disabled by system setting - "
                       "enable in Settings -> System -> Developer options", ""):
            with self.subTest(answer=answer), self.assertRaises(runner.Failure) as raised:
                runner.check_root_refused(RootDevice(answer))
            self.assertIn(runner.ROOT_REFUSAL_LINE, str(raised.exception))
            self.assertIn(answer, str(raised.exception))

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


class RootAnswerTopicTests(unittest.TestCase):
    """The adb route's topic that answers `adb root` on a build that is not debuggable.

    The answer is a string AOSP's adbd already holds, so the built binary cannot show the topic:
    the series does. After the topic, restart_root_service tests the build before it looks up
    adbroot_service, and only once.
    """

    ANSWER = "adbd cannot run as root in production builds"
    SERVICE_LOOKUP = 'AServiceManager_getService("adbroot_service")'

    def topic_hunks(self) -> list[list[str]]:
        listed = [line.split("\t")[1] for line in (ADB_SERIES / "series.tsv").read_text().splitlines()
                  if line.startswith("patch\t")]
        patches = [name for name in listed if "diff --git a/daemon/restart_service.cpp " in (ADB_SERIES / name).read_text()]
        self.assertEqual(len(patches), 1, f"restart_service.cpp is changed by {patches}")
        hunks: list[list[str]] = []
        for line in (ADB_SERIES / patches[0]).read_text().splitlines():
            if line.startswith("@@"):
                hunks.append([])
            elif hunks and line[:1] in (" ", "+", "-"):
                hunks[-1].append(line)
        return hunks

    def test_the_build_is_tested_before_adbroot_service_is_looked_up(self) -> None:
        after = [line[1:] for hunk in self.topic_hunks() for line in hunk if not line.startswith("-")]
        answers = [index for index, line in enumerate(after) if self.ANSWER in line]
        lookups = [index for index, line in enumerate(after) if self.SERVICE_LOOKUP in line]
        self.assertEqual(len(answers), 1, "the production build answer is given once")
        self.assertEqual(len(lookups), 1, "the hunks show the service lookup")
        self.assertLess(answers[0], lookups[0])

    def test_the_old_test_of_the_build_is_gone(self) -> None:
        removed = [line for hunk in self.topic_hunks() for line in hunk if line.startswith("-") and self.ANSWER in line]
        self.assertEqual(len(removed), 1, "the answer moves, it is not duplicated")


if __name__ == "__main__":
    unittest.main()
