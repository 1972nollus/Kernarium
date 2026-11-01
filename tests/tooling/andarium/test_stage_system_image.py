#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Andarium/scripts/stage-system-image.py stages only a checked, accepted release.

A fake release carries the files and manifest fields the script reads: the
zstd disk archive (a frame header declaring its expanded size is enough), the
kernel command line and the partition layout.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
SCRIPT = REPOSITORY / "Andarium/scripts/stage-system-image.py"
PACKAGER = REPOSITORY / "Kernarium/scripts/package-ios-frameworks.sh"

DISK_BYTES = 3_274_702_848
RELEASE_ID = "20261031-los24-r4"
FAMILY = "lineage-24-mainline-6.18-4k"
DIRECTORY = f"lineage24-mainline6.18-4k-{RELEASE_ID}"
ARCHIVE = "guest/android-17-uml-arm64.img.zst"


def load_module():
    spec = importlib.util.spec_from_file_location("stage_system_image", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def zstd_archive(content_size: int | None = DISK_BYTES, body: bytes = b"compressed") -> bytes:
    """A zstd frame header with a 4-byte content size (single segment), then a body."""
    if content_size is None:
        # No content size, a window descriptor instead.
        return bytes.fromhex("28b52ffd") + bytes([0x00, 0x48]) + body
    return bytes.fromhex("28b52ffd") + bytes([0xA0]) + content_size.to_bytes(4, "little") + body


def make_release(root: pathlib.Path, **overrides) -> pathlib.Path:
    release = root / DIRECTORY
    (release / "guest").mkdir(parents=True)
    (release / "boot").mkdir()
    archive = overrides.get("archive", zstd_archive())
    (release / ARCHIVE).write_bytes(archive)
    (release / "boot/kernel-command-line.txt").write_text("init=/init console=ttyS0\n")
    layout = json.dumps({"schema_version": 1, "disk_size_bytes": overrides.get("layout_bytes", DISK_BYTES)})
    (release / "guest/partition-layout.json").write_text(layout)
    manifest = {
        "acceptance": overrides.get("acceptance", "accepted"),
        "android": {"major": 17},
        "kernel": {"page_size": overrides.get("page_size", 4096), "release": "6.18.54-gabc"},
        "product_family": overrides.get("family", FAMILY),
        "release_id": overrides.get("release_id", RELEASE_ID),
        "inventory": [
            {"path": ARCHIVE, "role": "guest-disk-archive", "size": overrides.get("archive_size", len(archive))},
            {"path": "boot/kernel-command-line.txt", "role": "kernel-command-line",
             "size": (release / "boot/kernel-command-line.txt").stat().st_size},
            {"path": "guest/partition-layout.json", "role": "partition-layout",
             "size": len(layout)},
        ],
    }
    (release / "release-manifest.json").write_text(json.dumps(manifest))
    return release


def make_acceptance(path: pathlib.Path, **overrides) -> pathlib.Path:
    values = {
        "LINEAGE24_ACCEPTED_RELEASE_ID": RELEASE_ID,
        "LINEAGE24_ACCEPTED_DIRECTORY_NAME": DIRECTORY,
        "LINEAGE24_ACCEPTED_PRODUCT_FAMILY": FAMILY,
        "LINEAGE24_ACCEPTED_ANDROID_MAJOR": "17",
    }
    values.update(overrides)
    lines = [f"{key}='{value}'" if not value.isdigit() else f"{key}={value}" for key, value in values.items()]
    path.write_text("\n".join(lines) + "\n")
    return path


def run(*arguments: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, "-B", str(SCRIPT), *arguments],
        capture_output=True, text=True, check=False, stdin=subprocess.DEVNULL,
    )


class StageSystemImageTests(unittest.TestCase):
    def setUp(self):
        self._scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self._scratch.cleanup)
        self.root = pathlib.Path(self._scratch.name)
        self.output = self.root / "out/andarium-system"

    def test_stages_the_four_files_of_an_explicit_release(self):
        release = make_release(self.root / "releases")
        result = run("--release", str(release), "--output", str(self.output))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            sorted(entry.name for entry in self.output.iterdir()),
            ["android-17-uml-arm64.img.zst", "kernel-command-line.txt",
             "partition-layout.json", "release-manifest.json"],
        )
        self.assertEqual(
            (self.output / "android-17-uml-arm64.img.zst").read_bytes(), (release / ARCHIVE).read_bytes()
        )
        self.assertEqual(
            (self.output / "kernel-command-line.txt").read_text(),
            (release / "boot/kernel-command-line.txt").read_text(),
        )
        self.assertIn(RELEASE_ID, result.stdout)

    def test_the_accepted_release_is_found_through_the_acceptance_file(self):
        make_release(self.root / "releases")
        acceptance = make_acceptance(self.root / "accepted-release.env")
        result = run("--acceptance", str(acceptance), "--releases", str(self.root / "releases"),
                     "--output", str(self.output))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.output / "release-manifest.json").is_file())

    def test_a_release_that_is_not_the_accepted_one_is_refused(self):
        make_release(self.root / "releases", release_id="20260101-other")
        acceptance = make_acceptance(self.root / "accepted-release.env")
        result = run("--acceptance", str(acceptance), "--releases", str(self.root / "releases"),
                     "--output", str(self.output))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("release_id", result.stderr)
        self.assertFalse(self.output.exists())

    def test_a_missing_release_is_named(self):
        acceptance = make_acceptance(self.root / "accepted-release.env")
        result = run("--acceptance", str(acceptance), "--releases", str(self.root / "none"),
                     "--output", str(self.output))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not installed", result.stderr)

    def test_each_defect_of_a_release_is_refused_and_stages_nothing(self):
        cases = {
            "acceptance": ({"acceptance": "candidate"}, "not accepted"),
            "page size": ({"page_size": 16384}, "4096"),
            "family": ({"family": "aosp-17"}, "LineageOS"),
            "archive size": ({"archive_size": 7}, "size in the manifest"),
            "not zstd": ({"archive": b"not zstd at all"}, "not a zstd frame"),
            "layout": ({"layout_bytes": DISK_BYTES + 1}, "partition layout"),
        }
        for name, (overrides, message) in cases.items():
            with self.subTest(name):
                release = make_release(self.root / name, **overrides)
                output = self.root / "out" / name
                result = run("--release", str(release), "--output", str(output))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)
                self.assertFalse(output.exists())

    def test_a_missing_file_is_refused(self):
        release = make_release(self.root / "releases")
        (release / "boot/kernel-command-line.txt").unlink()
        result = run("--release", str(release), "--output", str(self.output))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("missing or unsafe", result.stderr)

    def test_staging_again_replaces_the_output_and_a_failure_keeps_it(self):
        first = make_release(self.root / "first")
        self.assertEqual(run("--release", str(first), "--output", str(self.output)).returncode, 0)
        before = (self.output / "release-manifest.json").read_text()

        broken = make_release(self.root / "broken", page_size=16384)
        self.assertNotEqual(run("--release", str(broken), "--output", str(self.output)).returncode, 0)
        self.assertEqual((self.output / "release-manifest.json").read_text(), before)

        second = make_release(self.root / "second", release_id="20261101-los24-r5",
                              archive=zstd_archive(body=b"newer body"))
        self.assertEqual(run("--release", str(second), "--output", str(self.output)).returncode, 0)
        self.assertIn("20261101-los24-r5", (self.output / "release-manifest.json").read_text())
        self.assertTrue((self.output / "android-17-uml-arm64.img.zst").read_bytes().endswith(b"newer body"))
        # No scratch directory is left beside the output.
        self.assertEqual([entry.name for entry in self.output.parent.iterdir()], ["andarium-system"])


class ZstdHeaderTests(unittest.TestCase):
    def setUp(self):
        self.module = load_module()

    def test_reads_every_content_size_width(self):
        magic = bytes.fromhex("28b52ffd")
        # Single segment: one byte, flag 0.
        self.assertEqual(self.module.zstd_content_size(magic + bytes([0x20, 200]) + b"\0" * 12), 200)
        # Two bytes carry size - 256.
        self.assertEqual(self.module.zstd_content_size(magic + bytes([0x60]) + (44).to_bytes(2, "little") + b"\0" * 12), 300)
        # Eight bytes.
        self.assertEqual(
            self.module.zstd_content_size(magic + bytes([0xE0]) + (3_274_702_848).to_bytes(8, "little")),
            3_274_702_848,
        )
        # A window descriptor and a one-byte dictionary id come first when not single-segment.
        self.assertEqual(
            self.module.zstd_content_size(magic + bytes([0x81, 0x48, 7]) + (1000).to_bytes(4, "little") + b"\0"),
            1000,
        )

    def test_no_declared_size_is_not_an_error(self):
        magic = bytes.fromhex("28b52ffd")
        self.assertIsNone(self.module.zstd_content_size(magic + bytes([0x00, 0x48]) + b"\0" * 12))

    def test_a_truncated_or_foreign_header_is_refused(self):
        with self.assertRaises(self.module.StageError):
            self.module.zstd_content_size(b"PK\x03\x04" + b"\0" * 14)
        with self.assertRaises(self.module.StageError):
            self.module.zstd_content_size(bytes.fromhex("28b52ffd") + bytes([0xE0, 1, 2]))


class PackageOptionTests(unittest.TestCase):
    """The packaging script checks its options before it touches any tool."""

    def package(self, *arguments: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            ["/bin/sh", str(PACKAGER), *arguments],
            capture_output=True, text=True, check=False, stdin=subprocess.DEVNULL,
        )

    def test_an_unknown_runtime_set_is_refused(self):
        result = self.package("--runtimes", "linux")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--runtimes takes all or android", result.stderr)

    def test_runtimes_needs_a_value(self):
        result = self.package("--runtimes")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("usage", result.stderr)

    def test_the_linux_automation_runtime_is_not_an_android_only_package(self):
        result = self.package("--runtimes", "android", "--automation")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--automation", result.stderr)

    def test_an_unknown_option_shows_the_usage_with_the_new_option(self):
        result = self.package("--bogus")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--runtimes", result.stderr)


if __name__ == "__main__":
    unittest.main()
