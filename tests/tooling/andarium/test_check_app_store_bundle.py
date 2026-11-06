#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check what the App Store bundle check refuses and what it accepts."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import plistlib
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

TIMEOUT_SECONDS = 120

CLEAN = "int fixture(int value) { return value + 1; }\n"
KNOB = (
    "#include <stdlib.h>\n"
    'const char *fixture(void) { return getenv("KERNARIUM_HOST_FORWARD"); }\n'
)
LAUNCH_KNOB = (
    "#include <stdlib.h>\n"
    'const char *fixture(void) { return getenv("ANDARIUM_START"); }\n'
)
# The harness's console lines (ConsoleReport), which the configuration
# compiles out: what the check reports for each, and a full line.
REPORT_LINES = (
    ("Andarium stats", "Andarium stats: cpu=1% fps=2 memory-used=3 memory=4 held=5 boot=6"),
    ("Andarium thermal state", "Andarium thermal state: phase=start state=nominal uptime=0.000"),
    ("Andarium: runtime returned", "Andarium: runtime returned 0"),
    ("Andarium: application", "Andarium: application active"),
)
HOME_PATH = (
    "const char *fixture(void) { return \"/Users/NAME/work/project/file.c\"; }\n"
)
MANIFEST = {
    "NSPrivacyTracking": False,
    "NSPrivacyTrackingDomains": [],
    "NSPrivacyCollectedDataTypes": [],
    "NSPrivacyAccessedAPITypes": [],
}


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        check=False,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=TIMEOUT_SECONDS,
    )


def build(root: Path, name: str, source: str, extra: list[str] | None = None) -> Path:
    source_path = root / f"{name}.c"
    source_path.write_text(source, encoding="utf-8")
    output = root / f"lib{name}.dylib"
    built = run([
        "xcrun", "--sdk", "iphoneos", "clang", "-target", "arm64-apple-ios27.0",
        "-dynamiclib", str(source_path), "-o", str(output), *(extra or []),
    ])
    if built.returncode != 0:
        raise SystemExit(f"could not build {name}:\n{built.stdout}")
    return output


def build_with_debug_map(root: Path, name: str) -> Path:
    """A library linked from an object compiled with -g: the linker keeps a
    debug map whose N_OSO entry names the object file."""
    source_path = root / f"{name}.c"
    source_path.write_text(CLEAN, encoding="utf-8")
    objects = root / f"{name}.o"
    compiled = run([
        "xcrun", "--sdk", "iphoneos", "clang", "-target", "arm64-apple-ios27.0",
        "-g", "-c", str(source_path), "-o", str(objects),
    ])
    if compiled.returncode != 0:
        raise SystemExit(f"could not compile {name}:\n{compiled.stdout}")
    output = root / f"lib{name}.dylib"
    linked = run([
        "xcrun", "--sdk", "iphoneos", "clang", "-target", "arm64-apple-ios27.0",
        "-dynamiclib", str(objects), "-o", str(output),
    ])
    if linked.returncode != 0:
        raise SystemExit(f"could not link {name}:\n{linked.stdout}")
    return output


def strip_debug_map(library: Path) -> Path:
    """The same library after `strip -S`, as the packaging leaves it."""
    stripped = library.with_name(f"{library.stem}-stripped.dylib")
    shutil.copyfile(library, stripped)
    done = run(["xcrun", "--sdk", "iphoneos", "strip", "-S", str(stripped)])
    if done.returncode != 0:
        raise SystemExit(f"could not strip {library.name}:\n{done.stdout}")
    return stripped


def app_info(**overrides: object) -> dict:
    keys: dict[str, object] = {
        "CFBundleIdentifier": "org.example.Fixture",
        "CFBundleExecutable": "Fixture",
        "CFBundleName": "Fixture",
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": "1.0",
        "CFBundleVersion": "1",
        "MinimumOSVersion": "27.0",
        "ITSAppUsesNonExemptEncryption": False,
        "UIRequiredDeviceCapabilities": ["arm64", "metal"],
        "UIDeviceFamily": [1, 2],
        "NSMicrophoneUsageDescription": "Records only while Microphone is on.",
    }
    keys.update(overrides)
    return {key: value for key, value in keys.items() if value is not None}


def make_app(
    root: Path, name: str, executable: Path, info: dict,
    framework: Path | None = None, manifest: bool = True,
) -> Path:
    app = root / name / "Fixture.app"
    app.mkdir(parents=True)
    shutil.copyfile(executable, app / "Fixture")
    (app / "Info.plist").write_bytes(plistlib.dumps(info))
    if manifest:
        (app / "PrivacyInfo.xcprivacy").write_bytes(plistlib.dumps(MANIFEST))
    if framework is not None:
        bundle = app / "Frameworks" / "Runtime.framework"
        bundle.mkdir(parents=True)
        shutil.copyfile(framework, bundle / "Runtime")
        (bundle / "Info.plist").write_bytes(plistlib.dumps({
            "CFBundleIdentifier": "org.example.Runtime",
            "CFBundleExecutable": "Runtime",
            "CFBundleName": "Runtime",
            "CFBundlePackageType": "FMWK",
            "CFBundleShortVersionString": "1.0",
            "CFBundleVersion": "1",
            "MinimumOSVersion": "27.0",
        }))
        (bundle / "PrivacyInfo.xcprivacy").write_bytes(plistlib.dumps(MANIFEST))
    return app


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--checker", type=Path, required=True)
    options = parser.parse_args()

    failures: list[str] = []

    def check(app: Path, *, passes: bool, expect: str = "") -> None:
        result = run([sys.executable, "-B", str(options.checker), str(app)])
        label = app.parent.name
        if (result.returncode == 0) != passes:
            failures.append(f"{label}: expected {'a pass' if passes else 'a failure'}:\n{result.stdout}")
        elif expect and expect not in result.stdout:
            failures.append(f"{label}: output lacks {expect!r}:\n{result.stdout}")

    with tempfile.TemporaryDirectory(prefix="andarium-app-store-") as directory:
        root = Path(directory)
        clean = build(root, "clean", CLEAN)
        knob = build(root, "knob", KNOB)
        launch = build(root, "launch", LAUNCH_KNOB)
        reports = {
            index: build(root, f"report{index}", f'const char *fixture(void) {{ return "{line}"; }}\n')
            for index, (_name, line) in enumerate(REPORT_LINES)
        }
        # The guest kernel image rides in a section of its own; a knob name in
        # it belongs to the guest.
        guest = root / "guest.bin"
        guest.write_bytes(
            b"\0UML_ROOTFS\0KERNARIUM_GUEST_NOTE\0/Users/guest/built/here.c\0")
        carrier = build(
            root, "carrier", CLEAN,
            ["-Wl,-sectcreate,__TEXT,__uml_elf," + str(guest)])

        check(make_app(root, "ready", clean, app_info(), framework=clean),
              passes=True, expect="ready for the App Store")
        check(make_app(root, "guest-section", clean, app_info(), framework=carrier), passes=True)

        # Info.plist.
        check(make_app(root, "no-encryption-key", clean, app_info(ITSAppUsesNonExemptEncryption=None)),
              passes=False, expect="ITSAppUsesNonExemptEncryption")
        check(make_app(root, "uses-encryption", clean, app_info(ITSAppUsesNonExemptEncryption=True)),
              passes=False, expect="ITSAppUsesNonExemptEncryption")
        check(make_app(root, "no-metal", clean, app_info(UIRequiredDeviceCapabilities=["arm64"])),
              passes=False, expect="lacks metal")
        check(make_app(root, "no-arm64", clean, app_info(UIRequiredDeviceCapabilities=["metal"])),
              passes=False, expect="lacks arm64")
        check(make_app(root, "ipad-only", clean, app_info(UIDeviceFamily=[2])),
              passes=False, expect="UIDeviceFamily")
        check(make_app(root, "background", clean, app_info(UIBackgroundModes=["audio"])),
              passes=False, expect="background mode")

        # Automation knobs in the host code of the application or a framework.
        check(make_app(root, "app-knob", knob, app_info()),
              passes=False, expect="KERNARIUM_HOST_FORWARD")
        check(make_app(root, "launch-knob", launch, app_info()),
              passes=False, expect="ANDARIUM_START")
        knob_app = make_app(root, "framework-knob", clean, app_info(), framework=knob)
        check(knob_app, passes=False, expect="Runtime.framework/Runtime")

        # The harness's console lines, in the application's own code, whether
        # or not anything calls them.
        for index, (name, _line) in enumerate(REPORT_LINES):
            check(make_app(root, f"report-line-{index}", reports[index], app_info()),
                  passes=False, expect=f"'{name}'")

        # What the build host leaves in a binary: the linker's debug map, whose
        # N_OSO entries name the objects of the build, and a home-directory
        # path. The same library stripped with `strip -S` passes.
        mapped = build_with_debug_map(root, "mapped")
        stripped = strip_debug_map(mapped)
        homed = build(root, "homed", HOME_PATH)
        # Xcode strips the application's own executable when it archives, so
        # only an embedded framework's debug map is the check's concern.
        check(make_app(root, "app-debug-map", mapped, app_info()), passes=True)
        check(make_app(root, "framework-debug-map", clean, app_info(), framework=mapped),
              passes=False, expect="Runtime.framework/Runtime: carries the linker's debug map")
        check(make_app(root, "stripped-debug-map", stripped, app_info(), framework=stripped),
              passes=True, expect="ready for the App Store")
        check(make_app(root, "app-home-path", homed, app_info()),
              passes=False, expect="/Users/NAME/work/project/file.c")
        check(make_app(root, "framework-home-path", clean, app_info(), framework=homed),
              passes=False, expect="names a path under a home directory")

        # A runtime framework alone (what package-ios-frameworks.sh --app-store
        # checks): its host code, and not the guest kernel image.
        for name, passes, expect in (
            ("framework-knob", False, "KERNARIUM_HOST_FORWARD"),
            ("framework-debug-map", False, "debug map"),
            ("framework-home-path", False, "home directory"),
            ("stripped-debug-map", True, ""),
            ("guest-section", True, ""),
            ("ready", True, ""),
        ):
            framework = root / name / "Fixture.app" / "Frameworks" / "Runtime.framework"
            result = run([sys.executable, "-B", str(options.checker), "--runtime", str(framework)])
            if (result.returncode == 0) != passes or expect not in result.stdout:
                failures.append(f"--runtime {name}: unexpected result ({result.returncode}):\n{result.stdout}")

        # The GPU frameworks are checked for the build host's traces alone: a
        # knob-like name is no concern of theirs.
        for name, passes, expect in (
            ("framework-knob", True, ""),
            ("framework-debug-map", False, "debug map"),
            ("framework-home-path", False, "home directory"),
            ("stripped-debug-map", True, ""),
        ):
            framework = root / name / "Fixture.app" / "Frameworks" / "Runtime.framework"
            result = run([sys.executable, "-B", str(options.checker), "--host-traces", str(framework)])
            if (result.returncode == 0) != passes or expect not in result.stdout:
                failures.append(f"--host-traces {name}: unexpected result ({result.returncode}):\n{result.stdout}")

        # The privacy manifests are checked too.
        check(make_app(root, "no-manifest", clean, app_info(), manifest=False),
              passes=False, expect="PrivacyInfo.xcprivacy")

        missing = run([sys.executable, "-B", str(options.checker), str(root / "absent.app")])
        if missing.returncode == 0:
            failures.append("a missing application passed")

    for failure in failures:
        print(failure, file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
