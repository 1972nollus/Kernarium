#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The harness helper that provisions an erased Android guest.

A fake guest answers the shell commands the helper issues and models the two
settings and the wizard's HOME activity, so the test states what the helper
does to a fresh guest, leaves alone in a provisioned one, and refuses when the
guest does not end up provisioned. The real guest's reference state (what
LineageOS Setup Wizard leaves behind when a person completes it) is checked by
tests/integration/android/run_android_oobe.py.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import importlib.util
from pathlib import Path
import stat
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location("android_provision", ROOT / "tools/android_provision.py")
assert SPEC is not None and SPEC.loader is not None
android_provision = importlib.util.module_from_spec(SPEC)
sys.modules["android_provision"] = android_provision
SPEC.loader.exec_module(android_provision)

# `cmd package resolve-activity --brief` as the release prints it.
WIZARD_HOME = (
    "priority=9 preferredOrder=0 match=0x108000 specificIndex=-1 isDefault=true\n"
    "org.lineageos.setupwizard/.SetupWizardActivity\n"
)
LAUNCHER_HOME = (
    "priority=0 preferredOrder=0 match=0x108000 specificIndex=-1 isDefault=true\n"
    "com.android.launcher3/.uioverrides.QuickstepLauncher\n"
)


class FakeGuest:
    """The parts of a guest the helper reads and writes."""

    def __init__(self, provisioned: str = "0", setup_complete: str = "0",
                 disabled: tuple[str, ...] = (), ignores_disable: bool = False) -> None:
        self.settings = {("global", "device_provisioned"): provisioned,
                         ("secure", "user_setup_complete"): setup_complete}
        self.disabled = set(disabled)
        self.ignores_disable = ignores_disable
        self.commands: list[str] = []

    def writes(self) -> list[str]:
        return [command for command in self.commands
                if command.startswith(("settings put", "pm "))]

    def __call__(self, command: str) -> str:
        self.commands.append(command)
        words = command.split()
        if words[:2] == ["settings", "get"]:
            return self.settings.get((words[2], words[3]), "null") + "\n"
        if words[:2] == ["settings", "put"]:
            self.settings[(words[2], words[3])] = words[4]
            return ""
        if words[:2] == ["pm", "disable-user"]:
            assert words[2:4] == ["--user", "0"], command
            if not self.ignores_disable:
                self.disabled.add(words[4])
            return f"Package {words[4]} new state: disabled-user\n"
        if words[:2] == ["pm", "disable"]:
            # The shell may not change another package's component state.
            return "java.lang.SecurityException: Shell cannot change component state\n"
        if command.startswith("cmd package resolve-activity --brief"):
            wizard = android_provision.SETUP_WIZARD
            return LAUNCHER_HOME if wizard in self.disabled else WIZARD_HOME
        raise AssertionError(f"unexpected command: {command}")


class ProvisionTests(unittest.TestCase):
    def test_a_fresh_guest_is_provisioned_and_its_home_is_the_launcher(self) -> None:
        guest = FakeGuest()
        self.assertTrue(android_provision.provision(guest))
        self.assertEqual(guest.settings[("global", "device_provisioned")], "1")
        self.assertEqual(guest.settings[("secure", "user_setup_complete")], "1")
        self.assertEqual(guest.disabled, {android_provision.SETUP_WIZARD})
        self.assertEqual(android_provision.home_package(guest), "com.android.launcher3")
        # Only the settings and the wizard package's state for user 0 change.
        self.assertEqual(
            guest.writes(),
            ["settings put global device_provisioned 1",
             "settings put secure user_setup_complete 1",
             f"pm disable-user --user 0 {android_provision.SETUP_WIZARD}"],
        )

    def test_a_provisioned_guest_is_left_alone(self) -> None:
        guest = FakeGuest("1", "1", (android_provision.SETUP_WIZARD,))
        self.assertFalse(android_provision.provision(guest))
        self.assertEqual(guest.writes(), [])
        self.assertTrue(android_provision.is_provisioned(guest))

    def test_a_guest_whose_wizard_still_owns_home_is_completed(self) -> None:
        guest = FakeGuest("1", "1")
        self.assertTrue(android_provision.provision(guest))
        self.assertEqual(guest.disabled, {android_provision.SETUP_WIZARD})

    def test_a_guest_missing_one_setting_gets_it(self) -> None:
        guest = FakeGuest("1", "0", (android_provision.SETUP_WIZARD,))
        self.assertTrue(android_provision.provision(guest))
        self.assertEqual(guest.settings[("secure", "user_setup_complete")], "1")

    def test_a_guest_that_stays_unprovisioned_is_an_error(self) -> None:
        guest = FakeGuest(ignores_disable=True)
        with self.assertRaisesRegex(android_provision.ProvisionError, "HOME='org.lineageos.setupwizard'"):
            android_provision.provision(guest)

    def test_unset_settings_read_as_not_provisioned(self) -> None:
        guest = FakeGuest("null", "null")
        self.assertFalse(android_provision.is_provisioned(guest))

    def test_home_package_reads_the_brief_form(self) -> None:
        self.assertEqual(android_provision.home_package(lambda _: WIZARD_HOME), android_provision.SETUP_WIZARD)
        self.assertEqual(android_provision.home_package(lambda _: LAUNCHER_HOME), "com.android.launcher3")
        self.assertEqual(android_provision.home_package(lambda _: "No activity found\n"), "")
        self.assertEqual(android_provision.home_package(lambda _: ""), "")

    def test_the_adb_shell_passes_the_command_to_the_guest_and_returns_its_output(self) -> None:
        with tempfile.TemporaryDirectory(prefix="android-provision-") as scratch:
            fake = Path(scratch) / "adb"
            fake.write_text(
                "#!/bin/sh\n"
                'printf "%s|" "$@"\n'
                'printf "\\r\\n"\n'
            )
            fake.chmod(fake.stat().st_mode | stat.S_IXUSR)
            shell = android_provision.adb_shell(str(fake), "127.0.0.1:5555")
            self.assertEqual(shell("settings get global device_provisioned"),
                             "-s|127.0.0.1:5555|shell|settings get global device_provisioned|\n")


if __name__ == "__main__":
    unittest.main()
