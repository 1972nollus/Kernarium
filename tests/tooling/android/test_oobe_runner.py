#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The first-boot runner's reading of the guest.

run_android_oobe.py boots a release and completes LineageOS Setup Wizard through
the pages the product shows, and checks the fresh guest's defaults. The screens,
the resumed activity, the wizard package's disabled components, the overlay list
and the Wi-Fi line below are what a release's guest printed, so the runner's
reading of them, its strict flow and its end-state checks run without a guest.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import importlib.util
from pathlib import Path
import unittest
from unittest import mock

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


# `cmd overlay list android` on a fresh three-button guest, and on a release that forced gestures.
OVERLAYS_THREE_BUTTON = """\
android
[ ] com.android.internal.systemui.navbar.threebutton
[ ] com.android.internal.systemui.navbar.transparent
[ ] com.android.internal.systemui.navbar.gestural
"""
OVERLAYS_GESTURAL = OVERLAYS_THREE_BUTTON.replace("[ ] com.android.internal.systemui.navbar.gestural",
                                                  "[x] com.android.internal.systemui.navbar.gestural")
WIFI_LINE = ('mWifiInfo SSID: "VirtWifi", BSSID: c6:73:8b:52:2e:17, MAC: 02:15:b2:00:00:00, IP: /10.0.2.16, '
             'Security type: 0, Supplicant state: COMPLETED, Wi-Fi standard: unknown, RSSI: -50')


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
                        node("Welcome to Andarium", f"{runner.WIZARD}:id/welcome_title", "[32,890][688,1077]"),
                        node("Start", f"{runner.WIZARD}:id/start", "[181,1098][688,1226]")),
            ("tap", 434, 1162))

    def test_the_language_page_takes_its_first_item(self) -> None:
        self.assertEqual(
            self.action(runner.LOCALE_ACTIVITY,
                        node("Language", f"{runner.WIZARD}:id/suc_layout_title", "[32,144][688,212]"),
                        node("English (United States)", f"{runner.WIZARD}:id/sud_items_title", "[53,255][667,286]"),
                        node("Español", f"{runner.WIZARD}:id/sud_items_title", "[53,333][667,364]")),
            ("tap", 360, 270))

    def test_the_finish_page_taps_its_home_icon(self) -> None:
        self.assertEqual(
            self.action(runner.FINISH_ACTIVITY,
                        node("Tap the home button to go home", f"{runner.WIZARD}:id/swipe_hint_text", "[100,1000][600,1040]"),
                        node("", f"{runner.WIZARD}:id/swipe_hint_icon", "[300,900][420,1000]")),
            ("tap", 360, 950))
        with self.assertRaisesRegex(runner.Failure, "swipe_hint_icon"):
            self.action(runner.FINISH_ACTIVITY, node("Tap the home button to go home"))

    def test_buttons_are_found_by_text(self) -> None:
        for activity, label in ((runner.DATETIME_ACTIVITY, "Next"), (runner.THEME_ACTIVITY, "Finish")):
            with self.subTest(activity=activity):
                self.assertEqual(
                    self.action(activity, node("Title", "", "[0,0][10,10]"), node(label, "", "[32,1162][687,1237]")),
                    ("tap", 359, 1199))

    def test_the_theme_page_is_the_last_before_finish_and_says_so(self) -> None:
        self.assertEqual(runner.PAGES[runner.THEME_ACTIVITY], ("text", "Finish"))
        self.assertEqual(runner.FLOW[-2:], (runner.THEME_ACTIVITY, runner.FINISH_ACTIVITY))

    def test_the_flow_is_the_reviewed_pages_in_order(self) -> None:
        self.assertEqual([page.split("/")[-1] for page in runner.FLOW],
                         [".WelcomeActivity", ".locale.LocaleActivity", ".time.DateTimeActivity",
                          ".settings.ThemeSettingsActivity", ".FinishActivity"])

    def test_the_pages_the_product_hides_are_not_known(self) -> None:
        for activity in ("com.android.settings/.network.NetworkSetupActivity",
                         f"{runner.WIZARD}/.backup.RestoreIntroActivity",
                         f"{runner.WIZARD}/.privacy.LocationSettingsActivity",
                         f"{runner.WIZARD}/.settings.LineageSettingsActivity",
                         "com.android.settings/.password.SetupChooseLockGeneric",
                         f"{runner.WIZARD}/.settings.NavigationSettingsActivity"):
            with self.subTest(activity=activity), self.assertRaisesRegex(runner.Failure, "does not know"):
                self.action(activity, node("Next"))

    def test_a_page_without_its_button_is_a_failure(self) -> None:
        with self.assertRaises(runner.Failure):
            self.action(runner.DATETIME_ACTIVITY, node("Title"))

    def test_a_page_the_runner_does_not_know_is_a_failure(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "does not know"):
            self.action(f"{runner.WIZARD}/.NewPageActivity", node("Next"))

    def test_a_dump_that_is_not_xml_is_a_failure(self) -> None:
        with self.assertRaises(runner.Failure):
            runner.parse_nodes("ERROR: null root node returned by UiTestAutomationBridge.")


class StrictFlowTests(unittest.TestCase):
    def test_the_page_on_screen_or_the_next_one_is_the_flow(self) -> None:
        for position in range(len(runner.FLOW)):
            self.assertEqual(runner.next_page(position, runner.FLOW[position]), position)
            if position + 1 < len(runner.FLOW):
                self.assertEqual(runner.next_page(position, runner.FLOW[position + 1]), position + 1)

    def test_a_page_out_of_order_is_a_failure(self) -> None:
        for position, activity in ((0, runner.DATETIME_ACTIVITY), (1, runner.THEME_ACTIVITY),
                                   (2, runner.LOCALE_ACTIVITY), (3, runner.WELCOME_ACTIVITY),
                                   (4, runner.THEME_ACTIVITY)):
            with self.subTest(position=position), self.assertRaisesRegex(runner.Failure, "expected"):
                runner.next_page(position, activity)

    def test_a_hidden_page_is_a_failure_wherever_it_shows(self) -> None:
        for position in range(len(runner.FLOW)):
            with self.subTest(position=position), self.assertRaises(runner.Failure):
                runner.next_page(position, f"{runner.WIZARD}/.privacy.LocationSettingsActivity")

    def test_the_failure_names_the_page_and_what_was_expected(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "LocationSettingsActivity.*DateTimeActivity.*ThemeSettingsActivity"):
            runner.next_page(2, f"{runner.WIZARD}/.privacy.LocationSettingsActivity")


class FakeWizard:
    """A guest that shows the given activities in turn, one per look, and answers taps."""

    def __init__(self, activities: list[str]) -> None:
        self.activities = list(activities)
        self.taps: list[str] = []

    def resumed(self) -> str:
        return self.activities.pop(0) if len(self.activities) > 1 else self.activities[0]

    def ui_nodes(self) -> list[dict[str, str]]:
        return runner.parse_nodes(hierarchy(
            node("Start", f"{runner.WIZARD}:id/start", "[0,0][100,100]"),
            node("English", f"{runner.WIZARD}:id/sud_items_title", "[0,0][100,100]"),
            node("Next", "", "[0,0][100,100]"),
            node("Finish", "", "[0,0][100,100]"),
            node("", f"{runner.WIZARD}:id/swipe_hint_icon", "[0,0][100,100]")))

    def shell(self, command: str) -> str:
        self.taps.append(command)
        return ""


class CompleteWizardTests(unittest.TestCase):
    LAUNCHER = f"{runner.LAUNCHER_PACKAGE}/.uioverrides.QuickstepLauncher"

    def run_flow(self, activities: list[str]) -> FakeWizard:
        device = FakeWizard(activities)
        with mock.patch.object(runner.time, "sleep"):
            runner.complete_wizard(device)
        return device

    def test_the_reviewed_flow_completes(self) -> None:
        device = self.run_flow([page for page in runner.FLOW for _ in (0, 1)] + [self.LAUNCHER])
        # One tap per look at a page, two looks per page: the page was still up after the first tap.
        self.assertEqual(len(device.taps), 2 * len(runner.FLOW))

    def test_the_flow_may_be_slow_to_change_page(self) -> None:
        device = self.run_flow([runner.WELCOME_ACTIVITY] * 3 + [""] + list(runner.FLOW[1:]) + [self.LAUNCHER])
        self.assertGreaterEqual(len(device.taps), len(runner.FLOW))

    def test_a_hidden_page_is_a_failure(self) -> None:
        for hidden in ("com.android.settings/.network.NetworkSetupActivity",
                       f"{runner.WIZARD}/.privacy.LocationSettingsActivity",
                       "com.android.settings/.password.SetupChooseLockGeneric"):
            with self.subTest(hidden=hidden), self.assertRaisesRegex(runner.Failure, "the product does not"):
                self.run_flow([runner.WELCOME_ACTIVITY, runner.LOCALE_ACTIVITY, hidden])

    def test_a_reviewed_page_out_of_order_is_a_failure(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "expected"):
            self.run_flow([runner.WELCOME_ACTIVITY, runner.LOCALE_ACTIVITY, runner.THEME_ACTIVITY])

    def test_a_reviewed_page_that_never_shows_is_a_failure(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "before the whole flow"):
            self.run_flow([runner.WELCOME_ACTIVITY, runner.LOCALE_ACTIVITY, self.LAUNCHER])

    def test_the_launcher_before_any_page_is_a_failure(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "before the whole flow"):
            self.run_flow([self.LAUNCHER])

    def test_a_wizard_that_does_not_finish_is_a_failure(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "did not finish"):
            self.run_flow([runner.WELCOME_ACTIVITY])


class DefaultsTests(unittest.TestCase):
    def test_overlay_states_are_read_from_the_listing(self) -> None:
        self.assertEqual(
            runner.overlay_states(OVERLAYS_THREE_BUTTON),
            {runner.NAVBAR_3BUTTON: " ", "com.android.internal.systemui.navbar.transparent": " ",
             runner.NAVBAR_GESTURAL: " "})
        self.assertEqual(runner.overlay_states(OVERLAYS_GESTURAL)[runner.NAVBAR_GESTURAL], "x")
        self.assertEqual(runner.overlay_states("--- some.package\n")["some.package"], "-")
        self.assertEqual(runner.overlay_states(""), {})

    def test_the_wifi_network_is_read_from_the_dump(self) -> None:
        self.assertEqual(runner.wifi_network(WIFI_LINE), ("VirtWifi", "COMPLETED"))
        self.assertEqual(runner.wifi_network(WIFI_LINE.replace("COMPLETED", "SCANNING")), ("VirtWifi", "SCANNING"))
        self.assertIsNone(runner.wifi_network("Wifi is disabled"))


class FakeDefaults:
    """The guest's answers to the end-state checks, each of which a case can change."""

    def __init__(self, **changes: str) -> None:
        self.answers = {
            "secure navigation_mode": "0",
            "secure location_mode": "3",
            "global assisted_gps_enabled": "0",
            "overlays": OVERLAYS_THREE_BUTTON,
            "lock": "true",
            "wifi": WIFI_LINE,
        }
        self.answers.update(changes)

    def setting(self, namespace: str, name: str) -> str:
        return self.answers[f"{namespace} {name}"]

    def shell(self, command: str) -> str:
        if command == "cmd overlay list android":
            return self.answers["overlays"]
        if command == "locksettings get-disabled":
            return self.answers["lock"]
        if command.startswith("dumpsys wifi"):
            return self.answers["wifi"]
        raise AssertionError(command)


class CheckDefaultsTests(unittest.TestCase):
    BEFORE = {"secure location_mode": "3", "global assisted_gps_enabled": "0"}

    def check(self, before: dict[str, str] | None = BEFORE, **changes: str) -> None:
        def immediately(description: str, probe, seconds: int, guest=None) -> None:
            if not probe():
                raise runner.Failure(f"{description}: not within {seconds} s")

        with mock.patch.object(runner, "wait_for", immediately):
            runner.check_defaults(FakeDefaults(**changes), before)

    def test_a_fresh_guest_has_the_platform_defaults(self) -> None:
        self.check()
        self.check(before=None)

    def test_navigation_is_three_buttons(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "navigation_mode is '2', expected 0"):
            self.check(**{"secure navigation_mode": "2"})
        with self.assertRaisesRegex(runner.Failure, "navigation_mode is 'null'"):
            self.check(**{"secure navigation_mode": "null"})

    def test_the_gestural_overlay_is_off_and_both_overlays_are_installed(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "gestural navigation overlay is enabled"):
            self.check(overlays=OVERLAYS_GESTURAL)
        with self.assertRaisesRegex(runner.Failure, f"{runner.NAVBAR_3BUTTON} is not installed"):
            self.check(overlays=OVERLAYS_THREE_BUTTON.replace(runner.NAVBAR_3BUTTON, "x.other"))
        with self.assertRaisesRegex(runner.Failure, f"{runner.NAVBAR_GESTURAL} is not installed"):
            self.check(overlays=OVERLAYS_THREE_BUTTON.replace(runner.NAVBAR_GESTURAL, "x.other"))

    def test_there_is_no_screen_lock(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "expected true"):
            self.check(lock="false")

    def test_location_stays_on_as_it_was(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "location is not on"):
            self.check(**{"secure location_mode": "0"})
        with self.assertRaisesRegex(runner.Failure, "changed the location settings"):
            self.check(**{"global assisted_gps_enabled": "1"})
        # A guest with no before-state (the helper's) is held to location on only.
        self.check(before=None, **{"global assisted_gps_enabled": "1"})

    def test_wi_fi_is_connected_to_the_virtual_network(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "Wi-Fi connected to VirtWifi"):
            self.check(wifi="Wifi is disabled")
        with self.assertRaisesRegex(runner.Failure, "Wi-Fi connected to VirtWifi"):
            self.check(wifi=WIFI_LINE.replace("VirtWifi", "Other"))
        with self.assertRaisesRegex(runner.Failure, "Wi-Fi connected to VirtWifi"):
            self.check(wifi=WIFI_LINE.replace("COMPLETED", "ASSOCIATING"))


class WizardReleasedTests(unittest.TestCase):
    class Guest:
        def __init__(self, record, processes) -> None:
            self.record, self.processes = record, processes

        def wizard_status_bar_record(self):
            return self.record

        def wizard_processes(self):
            return self.processes

    def check(self, guest, *, by_wizard: bool) -> None:
        def immediately(description: str, probe, seconds: int, guest=None) -> None:
            if not probe():
                raise runner.Failure(f"{description}: not within {seconds} s")

        with mock.patch.object(runner, "wait_for", immediately):
            runner.check_wizard_released(guest, by_wizard=by_wizard)

    def test_a_wizard_that_holds_the_status_bar_fails_both_guests(self) -> None:
        for by_wizard in (True, False):
            with self.subTest(by_wizard=by_wizard), self.assertRaisesRegex(runner.Failure, "no status bar disable record"):
                self.check(self.Guest(True, []), by_wizard=by_wizard)
            with self.subTest(by_wizard=by_wizard, unreadable=True), self.assertRaises(runner.Failure):
                self.check(self.Guest(None, []), by_wizard=by_wizard)

    def test_the_helper_leaves_no_wizard_process_and_the_wizard_may_linger(self) -> None:
        self.check(self.Guest(False, ["1445"]), by_wizard=True)
        self.check(self.Guest(False, []), by_wizard=False)
        with self.assertRaisesRegex(runner.Failure, "still runs after the helper"):
            self.check(self.Guest(False, ["1445"]), by_wizard=False)


if __name__ == "__main__":
    unittest.main()
