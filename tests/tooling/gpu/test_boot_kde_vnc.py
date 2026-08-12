#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import importlib.util
from pathlib import Path
import sys
import unittest


SCRIPT = Path(__file__).resolve().parents[3] / "scripts" / "boot_kde_vnc.py"
GPU_INTEGRATION = Path(__file__).resolve().parents[2] / "integration" / "gpu"
SPEC = importlib.util.spec_from_file_location("boot_kde_vnc", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
boot_kde_vnc = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(boot_kde_vnc)


class KdeVncLauncherTests(unittest.TestCase):
    def test_defaults_to_localhost_without_a_password(self) -> None:
        options = boot_kde_vnc.parse_arguments([])
        self.assertEqual(options.bind, "127.0.0.1")
        self.assertEqual(options.display, "native")
        self.assertIsNone(options.password)
        self.assertEqual(options.cpus, 10)
        self.assertEqual(options.engine, "jit")

    def test_execution_settings_are_forwarded(self) -> None:
        options = boot_kde_vnc.parse_arguments(
            ["--cpus", "6", "--engine", "gadgets"]
        )
        command = boot_kde_vnc.build_harness_command(options)
        separator = boot_kde_vnc.guest_argument_separator(command)
        self.assertIn("ncpus=6", command[separator + 1 :])
        self.assertIn("--execution-engine=gadgets", command[separator + 1 :])

    def test_webgl_commands_extend_the_gpu_harness(self) -> None:
        options = boot_kde_vnc.parse_arguments([])
        command = boot_kde_vnc.build_harness_command(options)
        guest_separator = boot_kde_vnc.guest_argument_separator(command)
        self.assertGreater(
            guest_separator, command.index(str(boot_kde_vnc.GUEST_HARNESS))
        )
        for runner_name in ("run_chromium_webgl.py", "run_webgl_samples.py"):
            runner = (GPU_INTEGRATION / runner_name).read_text(encoding="utf-8")
            self.assertIn(
                "boot_kde_vnc.guest_argument_separator(command)", runner
            )

    def test_guest_harness_runs_directly(self) -> None:
        options = boot_kde_vnc.parse_arguments(["--rootfs", "/tmp/rootfs.erofs"])
        command = boot_kde_vnc.build_harness_command(options)
        self.assertEqual(command[0], sys.executable)
        self.assertEqual(
            command[1:6],
            [
                str(boot_kde_vnc.GUEST_HARNESS),
                "--emulator",
                str(options.emulator),
                "--rootfs",
                "/tmp/rootfs.erofs",
            ],
        )

    def test_desktop_idle_lock_is_disabled_in_overlay(self) -> None:
        commands = "\n".join(boot_kde_vnc.KDE_OVERLAY_COMMANDS)
        self.assertIn("kscreenlockerrc", commands)
        self.assertIn("Autolock=false", commands)
        self.assertIn("LockOnResume=false", commands)

    def test_existing_uml_desktop_user_is_reused(self) -> None:
        commands = "\n".join(boot_kde_vnc.KDE_OVERLAY_COMMANDS)
        self.assertIn("getent passwd uml", commands)
        self.assertIn("User=uml", commands)
        self.assertIn("/home/uml", commands)
        self.assertNotIn("useradd", commands)
        self.assertNotIn("User=plasma", commands)
        self.assertNotIn("/home/plasma", commands)
        for runner_name in ("run_chromium_webgl.py", "run_webgl_samples.py"):
            runner = (GPU_INTEGRATION / runner_name).read_text(encoding="utf-8")
            self.assertIn('runtime="/run/user/$(id -u uml)"', runner)
            self.assertIn("runuser -u uml", runner)
            self.assertIn("HOME=/home/uml", runner)
            self.assertNotIn("runuser -u plasma", runner)
            self.assertNotIn("HOME=/home/plasma", runner)

    def test_password_is_forwarded_to_the_host_gpu(self) -> None:
        options = boot_kde_vnc.parse_arguments(
            ["--display", "vnc", "--password", "secret", "--dry-run"]
        )
        command = boot_kde_vnc.build_harness_command(options)
        self.assertIn("--gpu-display=vnc", command)
        self.assertIn("--gpu-vnc-bind=127.0.0.1", command)
        self.assertIn("--gpu-vnc-password=secret", command)

    def test_native_default_has_no_vnc_host_options(self) -> None:
        command = boot_kde_vnc.build_harness_command(
            boot_kde_vnc.parse_arguments([])
        )
        self.assertIn("--gpu-display=native", command)
        self.assertFalse(any(item.startswith("--gpu-vnc-") for item in command))

    def test_vnc_password_limit_is_enforced(self) -> None:
        with self.assertRaises(SystemExit):
            boot_kde_vnc.parse_arguments(["--password", "123456789"])

    def test_native_display_rejects_a_vnc_password(self) -> None:
        with self.assertRaises(SystemExit):
            boot_kde_vnc.parse_arguments(["--password", "secret"])

    def test_native_display_rejects_vnc_network_options(self) -> None:
        with self.assertRaises(SystemExit):
            boot_kde_vnc.parse_arguments(["--port", "5901"])


if __name__ == "__main__":
    unittest.main()
