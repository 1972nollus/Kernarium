#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import importlib.util
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[3] / "tools/boot_android.py"
SPEC = importlib.util.spec_from_file_location("boot_android", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
boot_android = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(boot_android)


class BootAndroidTests(unittest.TestCase):
    def installed_release(self, root: Path, name: str = "accepted-1") -> Path:
        release = root / name
        (release / "boot").mkdir(parents=True)
        (release / "guest").mkdir()
        (release / "release-manifest.json").write_text(
            '{"release_id":"' + name + '"}\n', encoding="utf-8"
        )
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
            Path("/tmp/android.img"),
            Path("/tmp/uml-darwin-android17"),
        )
        self.assertIn("--gpu-display=native", command)
        self.assertIn("--gpu-width=720", command)
        self.assertIn("--gpu-height=1280", command)
        self.assertIn("--execution-engine=gadgets", command)
        self.assertIn("ncpus=10", command)
        self.assertIn("mem=8G", command)
        self.assertIn("--android-boot-mode=normal", command)
        self.assertIn("--android-disk=/tmp/android.img", command)
        self.assertFalse(any("android-instance" in value for value in command))
        self.assertFalse(any("android-base-disk" in value for value in command))
        self.assertFalse(options.build)
        self.assertTrue(boot_android.parse_arguments(["--build"]).build)

    def test_vnc_display_names_its_port(self) -> None:
        options = boot_android.parse_arguments(
            ["--display", "vnc", "--vnc-port", "5931"]
        )
        command = boot_android.emulator_command(
            options, Path("/r"), Path("/i"), Path("/e")
        )
        self.assertIn("--gpu-display=vnc", command)
        self.assertIn("--gpu-vnc-port=5931", command)
        self.assertNotIn("--gpu-display=native", command)
        native = boot_android.emulator_command(
            boot_android.parse_arguments([]), Path("/r"), Path("/i"), Path("/e")
        )
        self.assertFalse(any("vnc" in argument for argument in native))

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
                "# fixture\nLINEAGE24_ACCEPTED_DIRECTORY_NAME='accepted-1'\n",
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
                "LINEAGE24_ACCEPTED_DIRECTORY_NAME='../escape'\n",
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
        ):
            with self.subTest(arguments=arguments):
                with self.assertRaises(SystemExit):
                    boot_android.parse_arguments(arguments)

    def test_default_disk_is_the_stable_persistent_image(self) -> None:
        disk = boot_android.disk_path(None)
        self.assertEqual(disk.name, "android-17.img")
        # disk_path() resolves symbolic links, and `.build` may be one.
        self.assertEqual(
            disk.parent, boot_android.INSTANCES_ROOT.resolve(strict=False)
        )

    def test_disk_is_resized_with_sgdisk(self) -> None:
        with tempfile.TemporaryDirectory(prefix="android-launcher-") as temporary:
            root = Path(temporary)
            disk = root / "android.img"
            disk.write_bytes(b"image")
            sgdisk = root / "sgdisk"
            sgdisk.write_text("#!/bin/sh\n", encoding="utf-8")
            sgdisk.chmod(0o755)
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
                    boot_android.resize_android_disk(disk, 4, sgdisk=sgdisk)
                )
                self.assertFalse(
                    boot_android.resize_android_disk(disk, 4, sgdisk=sgdisk)
                )
            self.assertEqual(disk.stat().st_size, 4 * 1024**3)
            mutation = run_sgdisk.call_args.args
            self.assertEqual(mutation[0], sgdisk)
            self.assertIn("--move-second-header", mutation)
            self.assertIn("--delete=10", mutation)
            self.assertIn(
                "--new=10:6131712:8386559",
                mutation,
            )

    def test_new_release_recreates_the_persistent_disk(self) -> None:
        with tempfile.TemporaryDirectory(prefix="android-launcher-") as temporary:
            root = Path(temporary)
            release = self.installed_release(root / "releases", "release-2")
            disk = root / "android.img"
            disk.write_bytes(b"old")
            boot_android.disk_release_path(disk).write_text(
                "release-1\n", encoding="utf-8"
            )
            sgdisk = root / "sgdisk"
            sgdisk.write_text("#!/bin/sh\n", encoding="utf-8")
            sgdisk.chmod(0o755)
            def resize(image: Path, _capacity: int, **_kwargs: object) -> bool:
                with image.open("r+b") as handle:
                    handle.truncate(boot_android.DEFAULT_DISK_SIZE_GIB * 1024**3)
                return True

            with mock.patch.object(
                boot_android, "resize_android_disk", side_effect=resize
            ):
                self.assertTrue(
                    boot_android.prepare_persistent_disk(
                        release, disk, sgdisk=sgdisk
                    )
                )
            self.assertEqual(
                boot_android.disk_release_path(disk).read_text(encoding="utf-8"),
                "release-2\n",
            )
            self.assertEqual(disk.stat().st_size, 8 * 1024**3)
            with mock.patch.object(
                boot_android, "gpt_layout_matches", return_value=True
            ):
                self.assertFalse(
                    boot_android.prepare_persistent_disk(
                        release, disk, sgdisk=sgdisk
                    )
                )

    def test_sgdisk_requires_an_executable_candidate(self) -> None:
        with tempfile.TemporaryDirectory(prefix="android-launcher-") as temporary:
            unavailable = Path(temporary) / "sgdisk"
            with self.assertRaises(boot_android.LauncherError):
                boot_android.find_sgdisk((unavailable,), search_path=temporary)

    def test_sgdisk_is_found_on_path_before_the_fallbacks(self) -> None:
        with tempfile.TemporaryDirectory(prefix="android-launcher-") as temporary:
            root = Path(temporary).resolve()
            on_path = root / "bin"
            on_path.mkdir()
            fallback = root / "fallback-sgdisk"
            for executable in (on_path / "sgdisk", fallback):
                executable.write_text("#!/bin/sh\n", encoding="utf-8")
                executable.chmod(0o755)
            self.assertEqual(
                boot_android.find_sgdisk((fallback,), search_path=str(on_path)),
                on_path / "sgdisk",
            )
            self.assertEqual(
                boot_android.find_sgdisk((fallback,), search_path=str(root)),
                fallback,
            )

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

    def test_adb_is_ready_in_the_device_or_recovery_state(self) -> None:
        guest = mock.Mock()
        guest.poll.return_value = None
        for state in ("device", "recovery"):
            def run(command, **_kwargs):
                text = state if "get-state" in command else "connected"
                return subprocess.CompletedProcess(command, 0, stdout=text + "\n")

            with mock.patch.object(boot_android.subprocess, "run", side_effect=run):
                boot_android.wait_for_adb(Path("adb"), "127.0.0.1:5555", guest, 5)
        for state in ("offline", "unauthorized"):
            def refuse(command, **_kwargs):
                text = state if "get-state" in command else "connected"
                return subprocess.CompletedProcess(command, 0, stdout=text + "\n")

            with (
                mock.patch.object(boot_android.subprocess, "run", side_effect=refuse),
                mock.patch.object(boot_android.time, "sleep"),
                mock.patch.object(
                    boot_android.time, "monotonic", side_effect=(0.0, 0.5, 6.0)
                ),
                self.assertRaises(boot_android.LauncherError),
            ):
                boot_android.wait_for_adb(Path("adb"), "127.0.0.1:5555", guest, 5)

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
            disk = root / "android.img"
            guest = Guest()
            with (
                mock.patch.object(
                    boot_android.subprocess, "Popen", return_value=guest
                ) as popen,
                mock.patch.object(
                    boot_android, "prepare_persistent_disk", return_value=False
                ),
                mock.patch.object(boot_android, "ensure_loopback_port_available"),
                mock.patch.object(boot_android, "wait_for_adb"),
                mock.patch.object(boot_android, "install_shutdown_handlers"),
                mock.patch.object(boot_android, "disconnect_adb") as disconnect,
            ):
                result = boot_android.main(
                    [
                        "--release",
                        str(release),
                        "--disk",
                        str(disk),
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

    def test_guest_that_leads_no_group_is_still_stopped(self) -> None:
        guest = mock.Mock()
        guest.pid = 123
        guest.poll.return_value = None
        guest.wait.side_effect = (
            subprocess.TimeoutExpired("guest", 10),
            subprocess.TimeoutExpired("guest", 5),
            0,
        )
        with mock.patch.object(
            boot_android.os, "killpg", side_effect=ProcessLookupError
        ):
            boot_android.stop_guest(guest)
        self.assertEqual(
            guest.send_signal.call_args_list,
            [
                mock.call(boot_android.signal.SIGINT),
                mock.call(boot_android.signal.SIGTERM),
                mock.call(boot_android.signal.SIGKILL),
            ],
        )
        self.assertEqual(guest.wait.call_count, 3)

    def test_a_killed_emulator_leaves_no_run_directories(self) -> None:
        with tempfile.TemporaryDirectory(prefix="android-launcher-") as temporary:
            root = Path(temporary)
            scratch = root / "tmp"
            scratch.mkdir()
            marker = root / "emulator.pid"
            # An emulator that ignores the stop requests, as a wedged one
            # does, so it is killed and no handler of its own runs.
            emulator = (
                "trap '' INT TERM; "
                'mkdir "$TMPDIR/uml-kern$$" "$TMPDIR/uml-guest-initrd-$$-x"; '
                'echo $$ > "$1"; exec sleep 30'
            )
            # Another run's directory is that run's.
            other = scratch / f"uml-kern{os.getpid()}"
            other.mkdir()
            stages = ((signal.SIGINT, 0.3), (signal.SIGTERM, 0.3))
            with (
                mock.patch.dict(os.environ, {"TMPDIR": str(scratch)}),
                mock.patch.object(tempfile, "tempdir", str(scratch)),
                mock.patch.object(boot_android, "GUEST_STOP_STAGES", stages),
            ):
                guest = subprocess.Popen(
                    ["/bin/sh", "-c", emulator, "emulator", str(marker)],
                    start_new_session=True,
                )
                try:
                    deadline = time.monotonic() + 5
                    while not marker.exists() or not marker.read_text():
                        self.assertLess(time.monotonic(), deadline)
                        time.sleep(0.01)
                    pid = int(marker.read_text())
                    self.assertTrue((scratch / f"uml-kern{pid}").is_dir())
                    boot_android.stop_guest(guest)
                finally:
                    if guest.poll() is None:
                        guest.kill()
                    guest.wait(timeout=5)
            self.assertEqual(guest.returncode, -signal.SIGKILL)
            self.assertEqual(
                sorted(path.name for path in scratch.iterdir()), [other.name]
            )


if __name__ == "__main__":
    unittest.main()
