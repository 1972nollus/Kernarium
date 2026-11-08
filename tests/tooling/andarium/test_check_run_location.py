#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Andarium/scripts/check-run-location.py refuses a Mac build in a temporary directory.

The script runs in an Xcode run-script phase, where the destination is
TARGET_DEVICE_PLATFORM_NAME and the products directory TARGET_BUILD_DIR. The
fixtures give it those two variables and nothing else.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import pathlib
import subprocess
import sys
import tempfile
import unittest

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
SCRIPT = REPOSITORY / "Andarium/scripts/check-run-location.py"
BOUND_SECONDS = 30
# A products directory below no temporary directory; it need not exist.
ELSEWHERE = "/opt/xcode-products/Build/Products/Debug-iphoneos"


def run(products: str, destination: str | None = "macosx") -> subprocess.CompletedProcess[str]:
    environment = {"TARGET_BUILD_DIR": products}
    if destination is not None:
        environment["TARGET_DEVICE_PLATFORM_NAME"] = destination
    return subprocess.run(
        [sys.executable, "-B", str(SCRIPT)],
        env=environment, capture_output=True, text=True, timeout=BOUND_SECONDS, check=False, stdin=subprocess.DEVNULL,
    )


class RunLocationTests(unittest.TestCase):
    def test_a_mac_build_below_every_temporary_directory_is_refused(self):
        for root in ("/tmp", "/private/tmp", "/var/tmp", "/private/var/tmp", "/var/folders", "/private/var/folders"):
            with self.subTest(root=root):
                result = run(f"{root}/derived/Build/Products/Debug-iphoneos")
                self.assertEqual(result.returncode, 1)
                self.assertIn("error:", result.stderr)
                self.assertIn(root, result.stderr)

    def test_the_users_temporary_directory_is_refused(self):
        with tempfile.TemporaryDirectory() as scratch:
            result = run(scratch)
        self.assertEqual(result.returncode, 1, result.stderr)

    def test_a_path_that_leads_into_a_temporary_directory_is_refused(self):
        self.assertEqual(run("/opt/xcode-products/../../tmp/derived").returncode, 1)

    def test_a_mac_build_elsewhere_is_accepted(self):
        result = run(ELSEWHERE)
        self.assertEqual((result.returncode, result.stderr), (0, ""))

    def test_a_directory_that_only_starts_like_a_temporary_one_is_accepted(self):
        self.assertEqual(run("/tmpfiles/derived").returncode, 0)

    def test_other_destinations_may_use_a_temporary_directory(self):
        for destination in (None, "iphoneos", "iphonesimulator"):
            with self.subTest(destination=destination):
                self.assertEqual(run("/tmp/derived", destination).returncode, 0)

    def test_a_mac_build_without_a_products_directory_is_refused(self):
        self.assertEqual(run("").returncode, 1)


if __name__ == "__main__":
    unittest.main()
