#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[3] / "scripts/boot_android.py"
SPEC = importlib.util.spec_from_file_location("boot_android", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
boot_android = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(boot_android)


class BootAndroidTests(unittest.TestCase):
    def installed_release(self, root: Path, name: str = "accepted-1") -> Path:
        release = root / name
        (release / "boot").mkdir(parents=True)
        (release / "guest").mkdir()
        (release / "release-manifest.json").write_text("{}\n", encoding="utf-8")
        (release / "boot/kernel-command-line.txt").write_text(
            "console=ttyS0\n", encoding="utf-8"
        )
        (release / "guest/android-17-uml-arm64.img").write_bytes(b"image")
        return release

    def test_defaults_use_native_portrait_configuration(self) -> None:
        options = boot_android.parse_arguments([])
        command = boot_android.emulator_command(
            options,
            Path("/tmp/release"),
            Path("/tmp/instance.cow"),
            Path("/tmp/uml-darwin-android17"),
        )
        self.assertIn("--gpu-display=native", command)
        self.assertIn("--gpu-width=720", command)
        self.assertIn("--gpu-height=1280", command)
        self.assertIn("--execution-engine=gadgets", command)
        self.assertIn("ncpus=10", command)
        self.assertIn("mem=8G", command)
        self.assertIn("--android-boot-mode=normal", command)
        self.assertFalse(any("android-disk-size" in value for value in command))
        self.assertIn("--android-base-disk=/tmp/instance.base.img", command)
        self.assertFalse(options.build)
        self.assertTrue(boot_android.parse_arguments(["--build"]).build)

    def test_adb_forward_is_loopback_only(self) -> None:
        options = boot_android.parse_arguments(["--adb-port", "15555"])
        command = boot_android.emulator_command(
            options, Path("/r"), Path("/i"), Path("/e")
        )
        self.assertIn("--net-forward=tcp:127.0.0.1:15555-5555", command)
        self.assertFalse(any("0.0.0.0" in argument for argument in command))

    def test_recovery_flag_selects_recovery_boot(self) -> None:
        options = boot_android.parse_arguments(["--recovery"])
        command = boot_android.emulator_command(
            options, Path("/r"), Path("/i"), Path("/e")
        )
        self.assertEqual(options.boot_mode, "recovery")
        self.assertIn("--android-boot-mode=recovery", command)
        with self.assertRaises(SystemExit):
            boot_android.parse_arguments(["--recovery", "--boot-mode=auto"])

    def test_accepted_descriptor_selects_installed_release(self) -> None:
        with tempfile.TemporaryDirectory(prefix="android-launcher-") as temporary:
            root = Path(temporary)
            releases = root / "releases"
            release = self.installed_release(releases)
            descriptor = root / "accepted.env"
            descriptor.write_text(
                "# fixture\nANDROID17_ACCEPTED_DIRECTORY_NAME='accepted-1'\n",
                encoding="utf-8",
            )
            selected = boot_android.release_path(
                None, descriptor=descriptor, releases_root=releases
            )
            self.assertEqual(selected, release.resolve())

    def test_accepted_descriptor_rejects_path_escape(self) -> None:
        with tempfile.TemporaryDirectory(prefix="android-launcher-") as temporary:
            descriptor = Path(temporary) / "accepted.env"
            descriptor.write_text(
                "ANDROID17_ACCEPTED_DIRECTORY_NAME='../escape'\n",
                encoding="utf-8",
            )
            with self.assertRaises(boot_android.LauncherError):
                boot_android.accepted_directory(descriptor)

    def test_release_requires_runtime_payloads(self) -> None:
        with tempfile.TemporaryDirectory(prefix="android-launcher-") as temporary:
            release = Path(temporary) / "incomplete"
            release.mkdir()
            with self.assertRaises(boot_android.LauncherError):
                boot_android.release_path(release)

    def test_cli_rejects_invalid_resources(self) -> None:
        for arguments in (
            ["--cpus", "0"],
            ["--width", "0"],
            ["--height", "0"],
            ["--adb-port", "65536"],
            ["--memory", "8 G"],
            ["--disk-size-gib", "3"],
            ["--disk-size-gib", "65"],
        ):
            with self.subTest(arguments=arguments):
                with self.assertRaises(SystemExit):
                    boot_android.parse_arguments(arguments)

    def test_release_specific_default_instance_preserves_other_guests(self) -> None:
        release = Path("/tmp/android/releases/release-2")
        instance = boot_android.instance_path(None, release)
        self.assertEqual(instance.name, "release-2.cow")
        self.assertEqual(instance.parent, boot_android.INSTANCES_ROOT)

    def test_instance_base_is_resized_with_homebrew_sgdisk(self) -> None:
        with tempfile.TemporaryDirectory(prefix="android-launcher-") as temporary:
            root = Path(temporary)
            release = self.installed_release(root / "releases")
            instance = root / "guest.cow"
            base = boot_android.instance_base_path(instance)
            sgdisk = root / "sgdisk"
            sgdisk.write_text("#!/bin/sh\n", encoding="utf-8")
            sgdisk.chmod(0o755)
            boot_android.prepare_instance_base(release, instance, base)
            self.assertEqual(base.read_bytes(), b"image")
            self.assertEqual(base.name, "guest.base.img")
            completed = boot_android.subprocess.CompletedProcess(
                [], 0, "The operation has completed successfully.\n", ""
            )
            with (
                mock.patch.object(
                    boot_android,
                    "gpt_layout_matches",
                    return_value=True,
                ),
                mock.patch.object(
                    boot_android,
                    "run_sgdisk",
                    return_value=completed,
                ) as run_sgdisk,
            ):
                self.assertTrue(
                    boot_android.resize_instance_base(
                        base, instance, 4, sgdisk=sgdisk
                    )
                )
                self.assertFalse(
                    boot_android.resize_instance_base(
                        base, instance, 4, sgdisk=sgdisk
                    )
                )
            self.assertEqual(base.stat().st_size, 4 * 1024**3)
            mutation = run_sgdisk.call_args.args
            self.assertEqual(mutation[0], sgdisk)
            self.assertIn("--move-second-header", mutation)
            self.assertIn("--delete=10", mutation)
            self.assertIn(
                "--new=10:3796992:8386559",
                mutation,
            )

    def test_existing_instance_locks_disk_capacity(self) -> None:
        with tempfile.TemporaryDirectory(prefix="android-launcher-") as temporary:
            root = Path(temporary)
            base = root / "base.img"
            instance = root / "instance.cow"
            base.write_bytes(b"seed")
            instance.write_bytes(b"cow")
            sgdisk = root / "sgdisk"
            sgdisk.write_text("#!/bin/sh\n", encoding="utf-8")
            sgdisk.chmod(0o755)
            with (
                mock.patch.object(
                    boot_android, "gpt_layout_matches", return_value=False
                ),
                self.assertRaises(boot_android.LauncherError),
            ):
                boot_android.resize_instance_base(
                    base, instance, 8, sgdisk=sgdisk
                )

    def test_homebrew_sgdisk_requires_an_executable_candidate(self) -> None:
        with tempfile.TemporaryDirectory(prefix="android-launcher-") as temporary:
            unavailable = Path(temporary) / "sgdisk"
            with self.assertRaises(boot_android.LauncherError):
                boot_android.homebrew_sgdisk((unavailable,))

    def test_build_commands_use_the_checked_in_preset(self) -> None:
        self.assertEqual(
            boot_android.build_commands(Path("/usr/bin/cmake")),
            [
                ["/usr/bin/cmake", "--preset", "macos-android17"],
                [
                    "/usr/bin/cmake", "--build", "--preset", "macos-android17"
                ],
            ],
        )

    def test_normal_exit_disconnects_adb_and_reaps_guest(self) -> None:
        class Guest:
            pid = 123

            def __init__(self) -> None:
                self.exited = False

            def poll(self) -> int | None:
                return 0 if self.exited else None

            def wait(self, timeout: int | None = None) -> int:
                del timeout
                self.exited = True
                return 0

        with tempfile.TemporaryDirectory(prefix="android-launcher-") as temporary:
            root = Path(temporary)
            release = self.installed_release(root / "releases")
            emulator = root / "uml-darwin-android17"
            adb = root / "adb"
            for executable in (emulator, adb):
                executable.write_text("#!/bin/sh\n", encoding="utf-8")
                executable.chmod(0o755)
            instance = root / "instance.cow"
            guest = Guest()
            with (
                mock.patch.object(
                    boot_android.subprocess, "Popen", return_value=guest
                ) as popen,
                mock.patch.object(boot_android, "prepare_instance_base"),
                mock.patch.object(boot_android, "resize_instance_base"),
                mock.patch.object(boot_android, "ensure_loopback_port_available"),
                mock.patch.object(boot_android, "wait_for_adb"),
                mock.patch.object(boot_android, "install_shutdown_handlers"),
                mock.patch.object(boot_android, "disconnect_adb") as disconnect,
            ):
                result = boot_android.main(
                    [
                        "--release",
                        str(release),
                        "--instance",
                        str(instance),
                        "--emulator",
                        str(emulator),
                        "--adb",
                        str(adb),
                    ]
                )
            self.assertEqual(result, 0)
            self.assertTrue(guest.exited)
            popen.assert_called_once()
            self.assertTrue(popen.call_args.kwargs["start_new_session"])
            self.assertEqual(
                disconnect.call_args_list,
                [
                    mock.call(adb.resolve(), "127.0.0.1:5555"),
                    mock.call(adb.resolve(), "127.0.0.1:5555"),
                ],
            )


if __name__ == "__main__":
    unittest.main()
