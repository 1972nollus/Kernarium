#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import tempfile
import unittest

import run_gpu_guest


class GpuGuestSafetyTests(unittest.TestCase):
    def test_command_is_always_read_only(self) -> None:
        command = run_gpu_guest.build_command(
            Path("/bin/echo"),
            Path("/tmp/rootfs.img"),
            "/dev/ubda1",
            "8G",
            "multi-user.target",
            ["quiet"],
        )
        resolved_image = Path("/tmp/rootfs.img").resolve()
        self.assertIn(f"ubd0r={resolved_image}", command)
        self.assertIn("ro", command)
        self.assertNotIn("rw", command)
        self.assertNotIn(f"ubd0={resolved_image}", command)

    def test_conflicting_arguments_are_rejected(self) -> None:
        forbidden = (
            "rw",
            "ro",
            "ubd0=/tmp/other.img",
            "ubd0r=/tmp/other.img",
            "root=/dev/ubda",
            "init=/sbin/init",
            "mem=2G",
            "console=ttyS0",
            "systemd.unit=graphical.target",
        )
        for argument in forbidden:
            with self.subTest(argument=argument):
                with self.assertRaises(run_gpu_guest.SafetyError):
                    run_gpu_guest.validate_extra_arguments([argument])

    def test_writable_image_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / "rootfs.img"
            image.write_bytes(b"test image")
            image.chmod(0o644)
            baseline = {
                "size": image.stat().st_size,
                "sha256": hashlib.sha256(image.read_bytes()).hexdigest(),
            }
            with self.assertRaises(run_gpu_guest.SafetyError):
                run_gpu_guest.enforce_image_immutable(image, baseline)

    def test_frozen_matching_image_is_accepted(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / "rootfs.img"
            image.write_bytes(b"test image")
            image.chmod(0o444)
            baseline = {
                "size": image.stat().st_size,
                "sha256": hashlib.sha256(image.read_bytes()).hexdigest(),
            }
            descriptor, snapshot = run_gpu_guest.enforce_image_immutable(
                image, baseline
            )
            try:
                self.assertEqual(snapshot["mode"], 0o444)
                self.assertEqual(snapshot["sha256"], baseline["sha256"])
            finally:
                os.close(descriptor)

    def test_bootstrap_installs_overlay_guard_before_systemd(self) -> None:
        bootstrap = run_gpu_guest.make_bootstrap(
            run_gpu_guest.make_test_script(["true"], auto_poweroff=True)
        ).decode()
        self.assertIn("mount -t overlay overlay", bootstrap)
        self.assertIn("pivot_root . .uml-lower", bootstrap)
        self.assertIn("uml-gpu-overlay-test.service", bootstrap)
        self.assertIn("exec /lib/systemd/systemd", bootstrap)

    def test_bootstrap_chunks_large_test_scripts_for_the_init_tty(self) -> None:
        bootstrap = run_gpu_guest.make_bootstrap("x" * 16_384).decode()
        self.assertLessEqual(max(map(len, bootstrap.splitlines())), 600)


if __name__ == "__main__":
    unittest.main()
