#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import importlib.util
from pathlib import Path
import unittest


SCRIPT = Path(__file__).resolve().parents[3] / "scripts" / "boot_kde_vnc.py"
SPEC = importlib.util.spec_from_file_location("boot_kde_vnc", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
boot_kde_vnc = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(boot_kde_vnc)


class KdeVncLauncherTests(unittest.TestCase):
    def test_defaults_to_localhost_without_a_password(self) -> None:
        options = boot_kde_vnc.parse_arguments([])
        self.assertEqual(options.bind, "127.0.0.1")
        self.assertIsNone(options.password)
        self.assertEqual(options.cpus, 10)
        self.assertEqual(options.engine, "jit")

    def test_execution_settings_are_forwarded(self) -> None:
        options = boot_kde_vnc.parse_arguments(
            ["--cpus", "6", "--engine", "gadgets"]
        )
        command = boot_kde_vnc.build_harness_command(options)
        separator = command.index("--")
        self.assertIn("ncpus=6", command[separator + 1 :])
        self.assertIn("--execution-engine=gadgets", command[separator + 1 :])

    def test_desktop_idle_lock_is_disabled_in_overlay(self) -> None:
        commands = "\n".join(boot_kde_vnc.KDE_OVERLAY_COMMANDS)
        self.assertIn("kscreenlockerrc", commands)
        self.assertIn("Autolock=false", commands)
        self.assertIn("LockOnResume=false", commands)

    def test_password_is_forwarded_to_the_host_gpu(self) -> None:
        options = boot_kde_vnc.parse_arguments(
            ["--password", "secret", "--dry-run"]
        )
        command = boot_kde_vnc.build_harness_command(options)
        self.assertIn("--gpu-vnc-bind=127.0.0.1", command)
        self.assertIn("--gpu-vnc-password=secret", command)

    def test_vnc_password_limit_is_enforced(self) -> None:
        with self.assertRaises(SystemExit):
            boot_kde_vnc.parse_arguments(["--password", "123456789"])


if __name__ == "__main__":
    unittest.main()
