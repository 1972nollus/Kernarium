#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The first-boot runner's reading of the guest.

run_android_oobe.py boots a release and completes LineageOS Setup Wizard through
its own pages. The screens, the resumed activity and the wizard package's
disabled components below are what release r15's guest printed, so the runner's
reading of them is checked without a guest.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import importlib.util
from pathlib import Path
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / "integration/android/run_android_oobe.py"
SPEC = importlib.util.spec_from_file_location("run_android_oobe", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)

DUMPSYS_ACTIVITIES = """\
  topResumedActivity=ActivityRecord{248339971 u0 org.lineageos.setupwizard/.WelcomeActivity t8}
  ResumedActivity: ActivityRecord{248339971 u0 org.lineageos.setupwizard/.WelcomeActivity t8}
"""

DISABLED_AFTER_SETUP = """\
      overlay paths:
        /product/overlay/NavigationBarModeGestural/NavigationBarModeGesturalOverlay.apk
      disabledComponents:
        org.lineageos.setupwizard.wizardmanager.WizardManager
        org.lineageos.setupwizard.system.UpdateRecoveryActivity
        org.lineageos.setupwizard.SetupWizardActivity
        org.lineageos.setupwizard.system.DeviceSpecificActivity
      enabledComponents:
        org.lineageos.setupwizard.user.ScreenLockActivity

Queries:
"""


def node(text: str = "", resource_id: str = "", bounds: str = "[0,0][100,100]") -> str:
    return f'<node text="{text}" resource-id="{resource_id}" class="x" bounds="{bounds}" />'


def hierarchy(*nodes: str) -> str:
    return '<?xml version=\'1.0\' encoding=\'UTF-8\' standalone=\'yes\' ?><hierarchy rotation="0">' + "".join(nodes) + "</hierarchy>"


class ReadingTests(unittest.TestCase):
    def test_the_resumed_activity_is_the_one_the_stack_resumes(self) -> None:
        self.assertEqual(runner.resumed_activity(DUMPSYS_ACTIVITIES), runner.WELCOME_ACTIVITY)
        self.assertEqual(runner.resumed_activity("nothing resumed"), "")

    def test_disabled_components_stop_at_the_next_list(self) -> None:
        self.assertEqual(
            runner.disabled_components(DISABLED_AFTER_SETUP),
            {
                "org.lineageos.setupwizard.wizardmanager.WizardManager",
                "org.lineageos.setupwizard.system.UpdateRecoveryActivity",
                "org.lineageos.setupwizard.SetupWizardActivity",
                "org.lineageos.setupwizard.system.DeviceSpecificActivity",
            },
        )
        self.assertEqual(runner.disabled_components("      enabledComponents:\n        a.B\n"), set())

    def test_the_wizard_disables_the_components_the_runner_expects(self) -> None:
        disabled = runner.disabled_components(DISABLED_AFTER_SETUP)
        for name in runner.WIZARD_DISABLES:
            self.assertIn(f"{runner.WIZARD}.{name}", disabled)

    def test_bounds_have_a_center(self) -> None:
        self.assertEqual(runner.center("[181,1098][688,1226]"), (434, 1162))
        with self.assertRaises(runner.Failure):
            runner.center("[181,1098]")


class WizardPageTests(unittest.TestCase):
    def action(self, activity: str, *nodes: str) -> tuple:
        return runner.wizard_action(activity, runner.parse_nodes(hierarchy(*nodes)))

    def test_welcome_taps_start(self) -> None:
        self.assertEqual(
            self.action(runner.WELCOME_ACTIVITY,
                        node("Welcome to LineageOS", f"{runner.WIZARD}:id/welcome_title", "[32,890][688,1077]"),
                        node("Start", f"{runner.WIZARD}:id/start", "[181,1098][688,1226]")),
            ("tap", 434, 1162))

    def test_the_language_page_takes_its_first_item(self) -> None:
        self.assertEqual(
            self.action(f"{runner.WIZARD}/.locale.LocaleActivity",
                        node("Language", f"{runner.WIZARD}:id/suc_layout_title", "[32,144][688,212]"),
                        node("English (United States)", f"{runner.WIZARD}:id/sud_items_title", "[53,255][667,286]"),
                        node("Español", f"{runner.WIZARD}:id/sud_items_title", "[53,333][667,364]")),
            ("tap", 360, 270))

    def test_the_lock_page_declines_and_then_confirms(self) -> None:
        lock = "com.android.settings/.password.SetupChooseLockGeneric"
        page = (node("PIN", "android:id/title", "[122,448][157,477]"),
                node("Not now", "android:id/title", "[53,596][134,625]"))
        self.assertEqual(self.action(lock, *page), ("tap", 93, 610))
        # The confirmation sits over the same activity and is answered first.
        self.assertEqual(
            self.action(lock, *page,
                        node("Go back", "android:id/button2", "[362,692][466,756]"),
                        node("Skip anyway", "android:id/button1", "[477,692][615,756]")),
            ("tap", 546, 724))

    def test_the_finish_page_is_a_swipe(self) -> None:
        self.assertEqual(self.action(f"{runner.WIZARD}/.FinishActivity"), ("swipe",))

    def test_buttons_are_found_by_text(self) -> None:
        for activity, label in (
            ("com.android.settings/.network.NetworkSetupActivity", "Next"),
            (f"{runner.WIZARD}/.time.DateTimeActivity", "Next"),
            (f"{runner.WIZARD}/.backup.RestoreIntroActivity", "Skip"),
            (f"{runner.WIZARD}/.settings.NavigationSettingsActivity", "Finish"),
        ):
            with self.subTest(activity=activity):
                self.assertEqual(
                    self.action(activity, node("Title", "", "[0,0][10,10]"), node(label, "", "[32,1162][687,1237]")),
                    ("tap", 359, 1199))

    def test_a_page_without_its_button_is_a_failure(self) -> None:
        with self.assertRaises(runner.Failure):
            self.action(f"{runner.WIZARD}/.time.DateTimeActivity", node("Title"))

    def test_a_page_the_runner_does_not_know_is_a_failure(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "does not know"):
            self.action(f"{runner.WIZARD}/.NewPageActivity", node("Next"))

    def test_a_dump_that_is_not_xml_is_a_failure(self) -> None:
        with self.assertRaises(runner.Failure):
            runner.parse_nodes("ERROR: null root node returned by UiTestAutomationBridge.")


if __name__ == "__main__":
    unittest.main()
