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


# `logcat -b crash -d` after the wizard's process died while the package was disabled under it (r16, trimmed).
WIZARD_CRASH = """\
--------- beginning of crash
11-07 09:40:55.611  1777  1777 E AndroidRuntime: FATAL EXCEPTION: main
11-07 09:40:55.611  1777  1777 E AndroidRuntime: Process: org.lineageos.setupwizard, PID: 1777
11-07 09:40:55.611  1777  1777 E AndroidRuntime: java.lang.RuntimeException: Unable to get provider androidx.startup.InitializationProvider
11-07 09:40:55.611  1777  1777 E AndroidRuntime: Caused by: android.content.pm.PackageManager$NameNotFoundException: ComponentInfo{org.lineageos.setupwizard/androidx.startup.InitializationProvider}
"""
OTHER_CRASH = """\
--------- beginning of crash
11-07 09:50:12.105  2210  2210 E AndroidRuntime: FATAL EXCEPTION: main
11-07 09:50:12.105  2210  2210 E AndroidRuntime: Process: com.android.documentsui, PID: 2210
11-07 09:50:12.105  2210  2210 E AndroidRuntime: Process: org.lineageos.setupwizard.other, PID: 2211
"""


class WizardCrashTests(unittest.TestCase):
    class Guest:
        def __init__(self, crash_log: str) -> None:
            self.crash_log, self.commands = crash_log, []

        def shell(self, command: str, timeout: int = 30) -> str:
            self.commands.append(command)
            return self.crash_log

    def test_the_wizards_process_line_is_a_crash(self) -> None:
        self.assertEqual(
            runner.wizard_fatals(WIZARD_CRASH),
            ["11-07 09:40:55.611  1777  1777 E AndroidRuntime: Process: org.lineageos.setupwizard, PID: 1777"])

    def test_other_processes_and_an_empty_buffer_are_not(self) -> None:
        self.assertEqual(runner.wizard_fatals(OTHER_CRASH), [])
        self.assertEqual(runner.wizard_fatals("--------- beginning of crash\n"), [])
        self.assertEqual(runner.wizard_fatals(""), [])
        # A package that only shares the wizard's prefix is not the wizard.
        self.assertEqual(runner.wizard_fatals("E AndroidRuntime: Process: org.lineageos.setupwizard.remote, PID: 9"), [])

    def test_a_wizard_crash_fails_the_check_and_names_the_process(self) -> None:
        guest = self.Guest(WIZARD_CRASH)
        with self.assertRaisesRegex(runner.Failure, r"the setup wizard crashed: .*Process: org\.lineageos\.setupwizard, PID: 1777"):
            runner.check_wizard_did_not_crash(guest)
        self.assertEqual(guest.commands, ["logcat -b crash -d"])

    def test_a_clean_crash_buffer_passes(self) -> None:
        runner.check_wizard_did_not_crash(self.Guest(OTHER_CRASH))

    def test_both_guests_are_checked_after_the_wizard_is_released(self) -> None:
        calls: list[str] = []
        guest = mock.Mock()
        guest.setting.return_value = "1"
        guest.home_package.return_value = runner.LAUNCHER_PACKAGE
        guest.resumed.return_value = f"{runner.LAUNCHER_PACKAGE}/.Launcher"
        for by_wizard in (True, False):
            calls.clear()
            with self.subTest(by_wizard=by_wizard), \
                    mock.patch.object(runner, "wait_for"), \
                    mock.patch.object(runner, "disabled_components",
                                      return_value={f"{runner.WIZARD}.{name}" for name in runner.WIZARD_DISABLES}), \
                    mock.patch.object(runner, "check_wizard_released", side_effect=lambda *a, **k: calls.append("released")), \
                    mock.patch.object(runner, "check_wizard_did_not_crash", side_effect=lambda *a, **k: calls.append("crash")), \
                    mock.patch.object(runner, "check_defaults", side_effect=lambda *a, **k: calls.append("defaults")), \
                    mock.patch.object(runner, "check_launcher", side_effect=lambda *a, **k: calls.append("launcher")):
                # `pm list packages -d`: the wizard ends enabled, and the helper disables its package.
                guest.shell.return_value = "package:com.other" if by_wizard else f"package:{runner.WIZARD}"
                runner.check_provisioned(guest, by_wizard=by_wizard)
            self.assertEqual(calls, ["released", "crash", "defaults", "launcher"])


# The launcher's `Data Model` as `dumpsys activity com.android.launcher3/.uioverrides.QuickstepLauncher`
# prints it: protobuf text for the container, which leaves a zero field out.
DUMP_HEAD = """\
    ContextTracker:
    \tmCurrentContext=com.android.launcher3.uioverrides.QuickstepLauncher@c03d938
    Data Model:
     ---- items id map
    \tItemInfo(id=-103 type=APP container=# com.android.launcher3.logger.LauncherAtom$ContainerInfo@19f79dd
predicted_hotseat_container {
  index: -1
} targetComponent=null ResolvedTargetInfo=ResolvedTargetInfo(targetActivityComponentName=null, componentName=null, user=UserHandle{0}) screen=-1 cell(-1,-1) span(1,1) title=null, items: [])
    \tItemInfo(id=-102 type=APP container=# com.android.launcher3.logger.LauncherAtom$ContainerInfo@1a1bf6a
prediction_container {
} targetComponent=null ResolvedTargetInfo=ResolvedTargetInfo(targetActivityComponentName=null, componentName=null, user=UserHandle{0}) screen=-1 cell(-1,-1) span(1,1) title=null, items: [])
"""
DUMP_TAIL = """\
     ---- extra items
    Default MSDL player implementation.
"""


def dump_item(item_id: int, kind: str, package: str, class_name: str, *, page: int = 0, x: int = 0, y: int = 0,
              type_name: str = "APP", index: int | None = None) -> str:
    fields = []
    if kind == "hotseat":
        fields = [f"index: {item_id}"] if index is None else [f"index: {index}"]
    else:
        fields += [f"grid_x: {x}"] if x else []
        fields += [f"grid_y: {y}"] if y else []
        fields += [f"page_index: {page}"] if page else []
    body = "".join(f"  {field}\n" for field in fields)
    return (f"    \tItemInfo(id={item_id} type={type_name} container=# com.android.launcher3.logger.LauncherAtom"
            f"$ContainerInfo@21c970c\n{kind} {{\n{body}}} targetComponent=ComponentInfo{{{package}/{class_name}}} "
            f"ResolvedTargetInfo=ResolvedTargetInfo(componentName=ComponentInfo{{{package}/{class_name}}}) "
            f"screen={page} cell({x},{y}) span(1,1) title=null, items: [])\n")


def home_dump(*, columns: int = 5, **changes) -> str:
    apps = sorted(runner.HOME_APPS)
    order = [("com.android.settings", "com.android.settings.Settings")] + [app for app in apps if app[0] != "com.android.settings"]
    items = []
    for index, (package, class_name) in enumerate(order):
        items.append(dump_item(index + 2, "workspace", package, class_name, x=index % columns, y=index // columns))
    return DUMP_HEAD + "".join(items) + DUMP_TAIL


class LauncherWorkspaceTests(unittest.TestCase):
    def test_the_six_applications_on_the_first_page_are_the_default_layout(self) -> None:
        items = runner.launcher_items(home_dump())
        self.assertEqual(len(items), 6)
        self.assertEqual({item["component"] for item in items}, runner.HOME_APPS)
        self.assertEqual({item["page"] for item in items}, {0})
        self.assertEqual({item["container"] for item in items}, {"workspace"})
        self.assertEqual(runner.launcher_problems(items), [])

    def test_a_grid_with_other_columns_is_the_same_layout(self) -> None:
        for columns in (4, 5, 6):
            with self.subTest(columns=columns):
                self.assertEqual(runner.launcher_problems(runner.launcher_items(home_dump(columns=columns))), [])

    def test_the_prediction_items_are_not_items(self) -> None:
        items = runner.launcher_items(DUMP_HEAD + DUMP_TAIL)
        self.assertEqual(items, [])

    def test_a_dump_without_a_data_model_has_no_items(self) -> None:
        self.assertEqual(runner.launcher_items("nothing here"), [])
        self.assertEqual(len(runner.launcher_problems([])), 7)

    def test_the_old_layout_is_a_failure_on_every_count(self) -> None:
        # What r16 showed: a widget and three applications on the second page.
        old = (DUMP_HEAD
               + dump_item(2, "workspace", "org.lineageos.twelve", "org.lineageos.twelve.ui.widgets.NowPlayingAppWidgetProvider",
                           page=1, type_name="WIDGET")
               + dump_item(3, "workspace", "org.lineageos.glimpse", "org.lineageos.glimpse.MainActivity", page=1, x=1, y=4)
               + dump_item(4, "workspace", "org.lineageos.twelve", "org.lineageos.twelve.MainActivity", page=1, x=2, y=4)
               + dump_item(5, "workspace", "com.android.settings", "com.android.settings.Settings", page=1, x=3, y=4)
               + DUMP_TAIL)
        problems = runner.launcher_problems(runner.launcher_items(old))
        text = "; ".join(problems)
        self.assertIn("4 items, expected 6", text)
        self.assertIn("(WIDGET) is not one of the six applications", text)
        self.assertIn("org.lineageos.glimpse/org.lineageos.glimpse.MainActivity is on page 1, not the first", text)
        self.assertIn("com.android.calculator2/com.android.calculator2.Calculator is missing", text)

    def test_an_item_in_the_dock_is_a_failure(self) -> None:
        dump = home_dump().replace(
            "    \tItemInfo(id=2 type=APP container=# com.android.launcher3.logger.LauncherAtom$ContainerInfo@21c970c\nworkspace {",
            "    \tItemInfo(id=2 type=APP container=# com.android.launcher3.logger.LauncherAtom$ContainerInfo@21c970c\nhotseat {\n  index: 0\n",
            1)
        problems = runner.launcher_problems(runner.launcher_items(dump))
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("is in the hotseat, not on the workspace", problems[0])

    def test_a_seventh_item_is_a_failure(self) -> None:
        dump = home_dump().replace(DUMP_TAIL, dump_item(9, "workspace", "com.android.deskclock", "com.android.deskclock.DeskClock",
                                                      x=0, y=4) + DUMP_TAIL)
        problems = runner.launcher_problems(runner.launcher_items(dump))
        self.assertTrue(any("7 items, expected 6" in problem for problem in problems), problems)
        self.assertTrue(any("com.android.deskclock" in problem for problem in problems), problems)

    def test_a_missing_application_is_named(self) -> None:
        dump = home_dump()
        start = dump.index("    \tItemInfo(id=7 ")
        end = dump.index(DUMP_TAIL)
        problems = runner.launcher_problems(runner.launcher_items(dump[:start] + dump[end:]))
        self.assertEqual(len(problems), 2, problems)
        self.assertIn("5 items, expected 6", problems[0])
        self.assertIn("is missing", problems[1])


DROPS = """\
--------- beginning of main
11-07 03:59:00.310 D/ModelDbController( 1080): loading default workspace
11-07 03:59:00.496 W/DefaultLayoutParser( 1080): No preference or single system activity found for Intent { act=android.intent.action.DIAL }
11-07 03:59:00.878 D/DefaultLayoutParser( 1080): Can't find widget provider: com.google.android.googlequicksearchbox.SearchWidgetProvider
11-07 03:59:01.110 E/AutoInstalls( 1080): Favorite not found: com.google.android.gm/com.google.android.gm.ConversationListActivityGmail
"""
CLEAN = """\
--------- beginning of main
11-07 03:40:00.100 W/DefaultLayoutParser( 900): left over from an older load
11-07 03:59:00.310 D/ModelDbController( 1080): loading default workspace
11-07 03:59:00.500 D/DefaultLayoutParser( 1080): parsed favorites
"""


class LauncherLayoutLogTests(unittest.TestCase):
    def test_the_parsers_complaints_after_the_layout_load_are_the_drops(self) -> None:
        drops = runner.launcher_layout_drops(DROPS)
        self.assertEqual(len(drops), 3)
        self.assertTrue(drops[0].startswith("DefaultLayoutParser: No preference"))
        self.assertIn("Can't find widget provider", drops[1])
        self.assertIn("AutoInstalls: Favorite not found", drops[2])

    def test_a_clean_load_has_no_drops_and_older_lines_do_not_count(self) -> None:
        self.assertEqual(runner.launcher_layout_drops(CLEAN), [])

    def test_a_log_without_the_load_has_no_answer(self) -> None:
        # None is not "no drops": the caller refuses it.
        self.assertIsNone(runner.launcher_layout_drops("11-07 03:59:01.110 E/AutoInstalls( 1080): Favorite not found: x/y\n"))

    def test_the_last_load_is_the_one_that_counts(self) -> None:
        self.assertEqual(runner.launcher_layout_drops(DROPS + CLEAN), [])


class FakeLauncher:
    def __init__(self, dump: str, log: str) -> None:
        self.dump = dump
        self.log = log
        self.dumps = 0

    def shell(self, command: str, timeout: int = 30) -> str:
        if command.startswith("dumpsys activity"):
            self.dumps += 1
            return self.dump
        if command.startswith("logcat"):
            return self.log
        raise AssertionError(command)


class CheckLauncherTests(unittest.TestCase):
    def check(self, guest: FakeLauncher) -> None:
        with mock.patch.object(runner, "LAUNCHER_LOAD_SECONDS", 0), mock.patch.object(runner.time, "sleep"):
            runner.check_launcher(guest)

    def test_the_default_layout_passes(self) -> None:
        guest = FakeLauncher(home_dump(), CLEAN)
        self.check(guest)
        self.assertEqual(guest.dumps, 1)

    def test_a_log_without_the_layout_load_is_a_failure(self) -> None:
        # The check must not skip itself: it fails when the line is missing, rotated out or never logged.
        for log in ("--------- beginning of main\n", "",
                    "11-07 03:59:01.110 E/AutoInstalls( 1080): Favorite not found: x/y\n"):
            with self.subTest(log=log), self.assertRaisesRegex(
                    runner.Failure, "no 'loading default workspace' line, though the log buffers were raised to 64 MiB"):
                self.check(FakeLauncher(home_dump(), log))

    def test_the_log_is_read_from_the_main_buffer(self) -> None:
        commands: list[str] = []

        class Recording(FakeLauncher):
            def shell(self, command: str, timeout: int = 30) -> str:
                commands.append(command)
                return super().shell(command, timeout)

        self.check(Recording(home_dump(), CLEAN))
        log_commands = [command for command in commands if command.startswith("logcat")]
        self.assertEqual(len(log_commands), 1)
        self.assertIn("-b main", log_commands[0])

    def test_the_wrong_workspace_is_a_failure_naming_the_problem(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "not the default layout: 0 items, expected 6"):
            self.check(FakeLauncher(DUMP_HEAD + DUMP_TAIL, CLEAN))

    def test_a_dropped_item_is_a_failure_even_when_the_workspace_looks_right(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "dropped items of its default layout: DefaultLayoutParser"):
            self.check(FakeLauncher(home_dump(), DROPS))

    def test_the_launcher_may_load_after_the_check_starts(self) -> None:
        class Slow(FakeLauncher):
            def shell(self, command: str, timeout: int = 30) -> str:
                if command.startswith("dumpsys activity") and self.dumps < 2:
                    self.dumps += 1
                    return DUMP_HEAD + DUMP_TAIL
                return super().shell(command, timeout)

        guest = Slow(home_dump(), CLEAN)
        with mock.patch.object(runner, "LAUNCHER_LOAD_SECONDS", 60), mock.patch.object(runner.time, "sleep"):
            runner.check_launcher(guest)
        self.assertEqual(guest.dumps, 3)


# `logcat -b main -b system -b crash -g` on a guest whose buffers were raised.
LOG_SIZES_RAISED = """\
main: ring buffer is 64 MiB (3 MiB consumed), max entry is 5120 B, max payload is 4068 B
system: ring buffer is 64 MiB (1 MiB consumed), max entry is 5120 B, max payload is 4068 B
crash: ring buffer is 64 MiB (0 B consumed), max entry is 5120 B, max payload is 4068 B
"""
LOG_SIZES_DEFAULT = LOG_SIZES_RAISED.replace("64 MiB", "256 KiB")


class FakeLogDevice:
    """A guest's shell for the log buffer commands; `-G` takes effect when `accepts` is true."""

    def __init__(self, accepts: bool = True, failure: str | None = None) -> None:
        self.sizes = LOG_SIZES_DEFAULT
        self.accepts = accepts
        self.failure = failure
        self.commands: list[str] = []

    def shell(self, command: str, timeout: int = 30) -> str:
        self.commands.append(command)
        if " -G " in command:
            if self.failure:
                raise runner.Failure(f"'{command}' exited 1: {self.failure}")
            if self.accepts:
                self.sizes = LOG_SIZES_RAISED
            return ""
        if command.endswith(" -g"):
            return self.sizes
        raise AssertionError(command)


class LogBufferTests(unittest.TestCase):
    def test_sizes_are_read_in_bytes_whatever_the_unit(self) -> None:
        self.assertEqual(
            runner.log_buffer_sizes(LOG_SIZES_RAISED), {"main": 64 << 20, "system": 64 << 20, "crash": 64 << 20})
        self.assertEqual(runner.log_buffer_sizes(LOG_SIZES_DEFAULT)["main"], 256 << 10)
        self.assertEqual(runner.log_buffer_sizes("main: ring buffer is 256Kb (4Kb consumed)")["main"], 256 << 10)
        self.assertEqual(runner.log_buffer_sizes("events: ring buffer is 1.5 MiB (0 B consumed)")["events"], 3 << 19)
        self.assertEqual(runner.log_buffer_sizes("crash: ring buffer is 512 B (0 B consumed)")["crash"], 512)
        self.assertEqual(runner.log_buffer_sizes("logcat: unable to open the log"), {})

    def test_the_buffers_a_plain_logcat_reads_are_raised_and_read_back(self) -> None:
        guest = FakeLogDevice()
        runner.size_log_buffers(guest)
        self.assertEqual(guest.commands, [
            "logcat -b main -b system -b crash -G 64M", "logcat -b main -b system -b crash -g"])
        self.assertIn("main", runner.LOG_BUFFERS)
        self.assertGreaterEqual(runner.LOG_BUFFER_MIB, 64)

    def test_a_buffer_that_kept_its_size_is_a_failure_naming_it(self) -> None:
        guest = FakeLogDevice(accepts=False)
        with self.assertRaisesRegex(runner.Failure, r"not 64 MiB after logcat -G: \{'main': 262144, 'system': 262144, "):
            runner.size_log_buffers(guest)

    def test_a_buffer_the_guest_does_not_report_is_a_failure(self) -> None:
        guest = FakeLogDevice()
        guest.shell = lambda command, timeout=30: (  # type: ignore[method-assign]
            "" if " -G " in command else LOG_SIZES_RAISED.replace("crash:", "kernel:"))
        with self.assertRaisesRegex(runner.Failure, r"'crash': None"):
            runner.size_log_buffers(guest)

    def test_a_refused_resize_is_a_failure_with_the_guests_answer(self) -> None:
        with self.assertRaisesRegex(runner.Failure, "failed to set the buffer size"):
            runner.size_log_buffers(FakeLogDevice(failure="failed to set the buffer size to 64M"))


class FakeBootingGuest:
    """A guest whose adb answers after `silent` connects and whose boot completes after `polls` reads."""

    def __init__(self, silent: int = 0, polls: int = 0, sizing: FakeLogDevice | None = None) -> None:
        self.silent = silent
        self.polls = polls
        self.log = sizing or FakeLogDevice()
        self.events: list[str] = []
        self.reads = 0

    def connect(self) -> bool:
        self.events.append("connect")
        if self.silent > 0:
            self.silent -= 1
            return False
        return True

    def shell(self, command: str, timeout: int = 30) -> str:
        if command == "getprop sys.boot_completed":
            self.reads += 1
            # The property is unset, so empty, until the boot ends.
            value = "1" if self.reads > self.polls else ""
            self.events.append(f"boot={value}")
            return value
        self.events.append("resize" if " -G " in command else "read-sizes")
        return self.log.shell(command, timeout)


class WaitBootedTests(unittest.TestCase):
    def wait(self, guest: FakeBootingGuest, seconds: int = 100) -> None:
        with mock.patch.object(runner.time, "sleep"):
            runner.wait_booted(guest, seconds, None)

    def test_the_buffers_are_raised_when_adb_first_answers_and_before_the_boot_completes(self) -> None:
        guest = FakeBootingGuest(silent=2, polls=3)
        self.wait(guest)
        self.assertEqual(guest.events[:3], ["connect", "connect", "connect"])
        resize = guest.events.index("resize")
        self.assertEqual(guest.events[3], "boot=")  # the empty answer that told adb was up
        self.assertEqual(guest.events[resize:resize + 2], ["resize", "read-sizes"])
        self.assertLess(resize, guest.events.index("boot=1"))
        self.assertEqual(guest.events.count("resize"), 1)

    def test_a_guest_that_booted_before_adb_answered_is_still_raised_once(self) -> None:
        guest = FakeBootingGuest(silent=1, polls=0)
        self.wait(guest)
        self.assertEqual(guest.events.count("resize"), 1)
        self.assertLess(guest.events.index("resize"), guest.events.index("boot=1", guest.events.index("resize")))

    def test_a_resize_that_did_not_take_ends_the_wait_with_its_own_failure(self) -> None:
        guest = FakeBootingGuest(polls=3, sizing=FakeLogDevice(accepts=False))
        with self.assertRaisesRegex(runner.Failure, "not 64 MiB after logcat -G"):
            self.wait(guest)
        self.assertNotIn("boot=1", guest.events)

    def test_adb_that_never_answers_is_a_failure_at_the_deadline(self) -> None:
        guest = FakeBootingGuest(silent=10**6)
        with self.assertRaisesRegex(runner.Failure, "the guest's adb to answer: not within 0 s"):
            self.wait(guest, seconds=0)
        self.assertNotIn("resize", guest.events)

    def test_the_guest_is_sized_before_the_exercise_starts(self) -> None:
        order: list[str] = []
        guest = mock.Mock()
        guest.poll.return_value = 0
        options = mock.Mock(adb_port=5576, boot_timeout=100)
        with mock.patch.object(runner, "launch", return_value=(guest, Path("launcher.log"))), \
                mock.patch.object(runner, "stop_launcher"), \
                mock.patch.object(runner, "wait_booted", side_effect=lambda *args: order.append("sized and booted")):
            runner.run_guest(options, Path("."), "wizard", "adb", {}, [], lambda *args: order.append("exercise"))
        self.assertEqual(order, ["sized and booted", "exercise"])


if __name__ == "__main__":
    unittest.main()
