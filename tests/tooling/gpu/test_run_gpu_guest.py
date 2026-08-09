#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

GPU_INTEGRATION_ROOT = Path(__file__).resolve().parents[2] / "integration/gpu"
sys.path.insert(0, str(GPU_INTEGRATION_ROOT))
import run_gpu_guest  # noqa: E402


class GpuGuestSafetyTests(unittest.TestCase):
    def test_current_manifest_schema_is_accepted(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            manifest = Path(directory) / "manifest.json"
            manifest.write_text(
                json.dumps(
                    {
                        "schema_version": 3,
                        "size": 123,
                        "sha256": "00" * 32,
                    }
                ),
                encoding="utf-8",
            )
            self.assertEqual(run_gpu_guest.load_baseline(manifest)["size"], 123)

    def test_non_object_manifest_is_rejected_cleanly(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            manifest = Path(directory) / "manifest.json"
            manifest.write_text("[]\n", encoding="utf-8")
            with self.assertRaisesRegex(
                run_gpu_guest.SafetyError, "must be a JSON object"
            ):
                run_gpu_guest.load_baseline(manifest)

    def test_command_uses_the_raw_erofs_root(self) -> None:
        command = run_gpu_guest.build_command(
            Path("/bin/echo"),
            Path("/tmp/rootfs.erofs"),
            "/dev/ubda",
            "8G",
            "multi-user.target",
            ["quiet"],
        )
        resolved_image = Path("/tmp/rootfs.erofs").resolve()
        self.assertIn(f"ubd0c={resolved_image}", command)
        self.assertIn("root=/dev/ubda", command)
        self.assertIn("rootfstype=erofs", command)
        self.assertNotIn(f"ubd0r={resolved_image}", command)

    def test_conflicting_arguments_are_rejected(self) -> None:
        forbidden = (
            "ubd0=/tmp/other.erofs",
            "ubd0c=/tmp/other.erofs",
            "ubd0r=/tmp/other.erofs",
            "root=/dev/ubdb",
            "rootfstype=ext4",
            "init=/sbin/init",
            "mem=2G",
            "console=ttyS0",
            "systemd.unit=graphical.target",
        )
        for argument in forbidden:
            with self.subTest(argument=argument):
                with self.assertRaises(run_gpu_guest.SafetyError):
                    run_gpu_guest.validate_extra_arguments([argument])

    def test_matching_image_is_accepted_independent_of_host_mode(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / "rootfs.erofs"
            image.write_bytes(b"test image")
            image.chmod(0o644)
            baseline = {
                "size": image.stat().st_size,
                "sha256": hashlib.sha256(image.read_bytes()).hexdigest(),
            }
            snapshot = run_gpu_guest.verify_image_integrity(image, baseline)
            self.assertEqual(snapshot["sha256"], baseline["sha256"])

    def test_bootstrap_installs_overlay_guard_before_systemd(self) -> None:
        bootstrap = run_gpu_guest.make_bootstrap(
            run_gpu_guest.make_test_script(["true"], auto_poweroff=True)
        ).decode()
        docker_tmpfs = (
            "mount -t tmpfs -o mode=0711,nosuid,nodev tmpfs /var/lib/docker"
        )
        self.assertIn("mount -t overlay overlay", bootstrap)
        self.assertIn("pivot_root . .uml-lower", bootstrap)
        self.assertIn("mkdir -p /var/lib/docker", bootstrap)
        self.assertIn(docker_tmpfs, bootstrap)
        self.assertIn(
            'findmnt -n -o FSTYPE -T /var/lib/docker)" tmpfs',
            run_gpu_guest.make_test_script(["true"], auto_poweroff=True),
        )
        self.assertNotIn("getty@tty0.service", bootstrap)
        self.assertNotIn("agetty --autologin", bootstrap)
        self.assertNotIn("useradd", bootstrap)
        self.assertNotIn("groupadd", bootstrap)
        self.assertIn("uml-gpu-overlay-test.service", bootstrap)
        self.assertIn("exec /lib/systemd/systemd", bootstrap)
        self.assertLess(
            bootstrap.index("pivot_root . .uml-lower"),
            bootstrap.index(docker_tmpfs),
        )
        self.assertLess(
            bootstrap.index(docker_tmpfs),
            bootstrap.index("exec /lib/systemd/systemd"),
        )

    def test_bootstrap_chunks_large_test_scripts_for_the_init_tty(self) -> None:
        bootstrap = run_gpu_guest.make_bootstrap("x" * 16_384).decode()
        self.assertLessEqual(max(map(len, bootstrap.splitlines())), 600)

    def test_test_script_stops_at_the_first_failed_command(self) -> None:
        script = run_gpu_guest.make_test_script(
            ["false", "true"], auto_poweroff=True
        )
        self.assertIn("    (\n        set -e\nfalse\ntrue\n    )", script)

    def test_bootstrap_pipe_short_writes_are_retried(self) -> None:
        class ShortWriter(io.BytesIO):
            def write(self, payload: bytes) -> int:
                return super().write(payload[:3])

        stream = ShortWriter()
        run_gpu_guest.write_all(stream, b"0123456789")
        self.assertEqual(stream.getvalue(), b"0123456789")


if __name__ == "__main__":
    unittest.main()
