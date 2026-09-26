#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[3] / "scripts" / "boot_kde_vnc.py"
GPU_INTEGRATION = Path(__file__).resolve().parents[2] / "integration" / "gpu"
SPEC = importlib.util.spec_from_file_location("boot_kde_vnc", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
boot_kde_vnc = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(boot_kde_vnc)
sys.path.insert(0, str(GPU_INTEGRATION))
import run_webgl_samples  # noqa: E402


class KdeVncLauncherTests(unittest.TestCase):
    def test_defaults_to_localhost_without_a_password(self) -> None:
        options = boot_kde_vnc.parse_arguments([])
        self.assertEqual(options.bind, "127.0.0.1")
        self.assertEqual(options.display, "native")
        self.assertIsNone(options.password)
        self.assertEqual(options.cpus, 10)
        self.assertEqual(options.engine, "jit")
        self.assertEqual(options.rootfs, boot_kde_vnc.DEFAULT_ROOTFS)
        self.assertEqual(options.rootfs_type, "ext4")
        self.assertEqual(options.root_device, "/dev/ubda1")

    def test_default_starter_disk_contract_is_forwarded(self) -> None:
        command = boot_kde_vnc.build_harness_command(
            boot_kde_vnc.parse_arguments([])
        )
        separator = boot_kde_vnc.guest_argument_separator(command)
        harness_arguments = command[:separator]
        self.assertIn(str(boot_kde_vnc.DEFAULT_ROOTFS), harness_arguments)
        self.assertEqual(
            harness_arguments[
                harness_arguments.index("--rootfs-type") :
                harness_arguments.index("--rootfs-type") + 2
            ],
            ["--rootfs-type", "ext4"],
        )
        self.assertEqual(
            harness_arguments[
                harness_arguments.index("--root-device") :
                harness_arguments.index("--root-device") + 2
            ],
            ["--root-device", "/dev/ubda1"],
        )
        self.assertIn("ip=off", command[separator + 1 :])

    def test_execution_settings_are_forwarded(self) -> None:
        options = boot_kde_vnc.parse_arguments(
            ["--cpus", "6", "--engine", "gadgets"]
        )
        command = boot_kde_vnc.build_harness_command(options)
        separator = boot_kde_vnc.guest_argument_separator(command)
        self.assertIn("ncpus=6", command[separator + 1 :])
        self.assertIn("--execution-engine=gadgets", command[separator + 1 :])

    def test_standard_launch_attaches_host_audio(self) -> None:
        command = boot_kde_vnc.build_harness_command(
            boot_kde_vnc.parse_arguments([])
        )
        separator = boot_kde_vnc.guest_argument_separator(command)
        self.assertEqual(command.count("--audio"), 1)
        self.assertIn("--audio", command[separator + 1 :])

    def test_serial_socket_is_forwarded_to_the_guest_harness(self) -> None:
        options = boot_kde_vnc.parse_arguments(
            ["--serial-socket", "/tmp/uml-serial.sock"]
        )
        command = boot_kde_vnc.build_harness_command(options)
        separator = boot_kde_vnc.guest_argument_separator(command)
        self.assertEqual(
            command[command.index("--serial-socket") : separator],
            ["--serial-socket", "/tmp/uml-serial.sock"],
        )
        self.assertIn(
            "systemd.wants=serial-getty@ttyS0.service",
            command[separator + 1 :],
        )

    def test_virtio_console_socket_attaches_hvc0_getty(self) -> None:
        options = boot_kde_vnc.parse_arguments(
            ["--virtio-console-socket", "/tmp/uml-hvc0.sock"]
        )
        command = boot_kde_vnc.build_harness_command(options)
        separator = boot_kde_vnc.guest_argument_separator(command)
        self.assertEqual(
            command[command.index("--virtio-console-socket") : separator],
            ["--virtio-console-socket", "/tmp/uml-hvc0.sock"],
        )
        kernel_arguments = command[separator + 1 :]
        self.assertIn(
            "systemd.wants=serial-getty@hvc0.service", kernel_arguments
        )
        self.assertNotIn(
            "systemd.mask=serial-getty@hvc0.service", kernel_arguments
        )

    def test_console_socket_paths_must_be_distinct(self) -> None:
        with self.assertRaises(SystemExit):
            boot_kde_vnc.parse_arguments(
                [
                    "--serial-socket",
                    "/tmp/uml-console.sock",
                    "--virtio-console-socket",
                    "/tmp/uml-console.sock",
                ]
            )

    def test_iso_is_attached_read_only_and_mounted_at_cdrom(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            iso = Path(directory) / "installer.iso"
            iso.touch()
            options = boot_kde_vnc.parse_arguments(["--iso", str(iso)])
            command = boot_kde_vnc.build_harness_command(options)

        separator = boot_kde_vnc.guest_argument_separator(command)
        self.assertIn(f"ubd1r={iso.resolve()}", command[separator + 1 :])
        harness_arguments = command[:separator]
        mount_index = harness_arguments.index(boot_kde_vnc.ISO_MOUNT_COMMAND)
        self.assertEqual(harness_arguments[mount_index - 1], "--command")
        self.assertIn("mount -t iso9660 -o ro", boot_kde_vnc.ISO_MOUNT_COMMAND)
        self.assertIn("/dev/ubdb", boot_kde_vnc.ISO_MOUNT_COMMAND)
        self.assertIn("/mnt/cdrom", boot_kde_vnc.ISO_MOUNT_COMMAND)

    def test_no_iso_disk_or_mount_is_added_by_default(self) -> None:
        command = boot_kde_vnc.build_harness_command(
            boot_kde_vnc.parse_arguments([])
        )
        self.assertNotIn(boot_kde_vnc.ISO_MOUNT_COMMAND, command)
        self.assertFalse(any(argument.startswith("ubd1") for argument in command))

    def test_optional_display_density_is_forwarded_to_the_host_gpu(self) -> None:
        default_command = boot_kde_vnc.build_harness_command(
            boot_kde_vnc.parse_arguments([])
        )
        self.assertFalse(
            any(item.startswith("--gpu-dpi=") for item in default_command)
        )
        density_command = boot_kde_vnc.build_harness_command(
            boot_kde_vnc.parse_arguments(["--dpi", "288"])
        )
        self.assertIn("--gpu-dpi=288", density_command)

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

    def test_webgl_samples_pin_gles_and_bound_first_navigation(self) -> None:
        runner = (
            GPU_INTEGRATION / "run_webgl_samples.py"
        ).read_text(encoding="utf-8")
        self.assertIn("--use-gl=angle", runner)
        self.assertIn("--use-angle=gles-egl", runner)
        self.assertIn("--ignore-gpu-blocklist", runner)
        self.assertIn("--disable-quic", runner)
        self.assertIn("curl --fail --location", runner)
        self.assertIn("UML_WEBGL_FETCH_OK|BYTES=", runner)
        self.assertIn("first page fetch failed", runner)
        self.assertIn('"run_at": "document_end"', runner)
        self.assertNotIn('"run_at": "document_idle"', runner)
        self.assertIn('default=300', runner)
        self.assertIn("first WebGL sample did not render within", runner)
        self.assertIn('"state": "failed"', runner)
        self.assertIn('"state": "interrupted"', runner)

    def test_webgl_samples_reject_poisoned_virgl_contexts(self) -> None:
        clean = "UML_WEBGL_SAMPLE_OK|1/2|Aquarium"
        self.assertIsNone(run_webgl_samples.graphics_failure(clean))
        for signature in run_webgl_samples.GRAPHICS_FAILURES:
            line = "host: " + signature
            self.assertEqual(
                run_webgl_samples.graphics_failure(clean + "\n" + line),
                line,
            )

    def test_guest_harness_runs_directly(self) -> None:
        options = boot_kde_vnc.parse_arguments(
            [
                "--rootfs", "/tmp/rootfs.erofs",
                "--rootfs-type", "erofs",
            ]
        )
        command = boot_kde_vnc.build_harness_command(options)
        self.assertEqual(command[0], sys.executable)
        self.assertEqual(
            command[1:10],
            [
                str(boot_kde_vnc.GUEST_HARNESS),
                "--emulator",
                str(options.emulator),
                "--rootfs",
                "/tmp/rootfs.erofs",
                "--rootfs-type",
                "erofs",
                "--root-device",
                "/dev/ubda",
            ],
        )

    def test_ext4_root_type_is_forwarded_to_the_guest_harness(self) -> None:
        options = boot_kde_vnc.parse_arguments(
            ["--rootfs", "/tmp/rootfs.ext4", "--rootfs-type", "ext4"]
        )
        command = boot_kde_vnc.build_harness_command(options)
        separator = boot_kde_vnc.guest_argument_separator(command)
        rootfs_type = command.index("--rootfs-type")
        self.assertLess(rootfs_type, separator)
        self.assertEqual(
            command[rootfs_type : rootfs_type + 2],
            ["--rootfs-type", "ext4"],
        )
        root_device = command.index("--root-device")
        self.assertEqual(
            command[root_device : root_device + 2],
            ["--root-device", "/dev/ubda1"],
        )

    def test_desktop_idle_lock_is_disabled_in_overlay(self) -> None:
        commands = "\n".join(boot_kde_vnc.STARTER_KDE_OVERLAY_COMMANDS)
        self.assertIn("kscreenlockerrc", commands)
        self.assertIn("Autolock=false", commands)
        self.assertIn("LockOnResume=false", commands)

    def test_overlay_home_directories_name_every_parent(self) -> None:
        # install -d leaves unnamed parents owned by root, which the session
        # user then cannot write, so each parent must be named before its
        # children.
        for commands in (
            boot_kde_vnc.STARTER_KDE_OVERLAY_COMMANDS,
            boot_kde_vnc.LEGACY_EROFS_KDE_OVERLAY_COMMANDS,
        ):
            install = next(
                command
                for command in commands
                if command.startswith("install -d -m 0755 -o ")
            )
            directories = install.split()[8:]
            home = directories[0].rsplit("/", 1)[0]
            named: set[str] = set()
            for directory in directories:
                parent = directory.rsplit("/", 1)[0]
                self.assertTrue(
                    parent == home or parent in named,
                    f"{directory} is named before its parent",
                )
                named.add(directory)

    def test_starter_image_owns_account_and_login_policy(self) -> None:
        commands = "\n".join(boot_kde_vnc.STARTER_KDE_OVERLAY_COMMANDS)
        self.assertIn("getent passwd debian", commands)
        self.assertIn("/home/debian", commands)
        self.assertNotIn("useradd", commands)
        self.assertNotIn("usermod", commands)
        self.assertNotIn("systemctl", commands)
        self.assertNotIn("sddm", commands.lower())
        self.assertNotIn("serial-getty@", commands)
        self.assertNotIn("systemctl start getty@", commands)
        self.assertNotIn("agetty", commands)
        self.assertNotIn("chpasswd", commands)

    def test_conflicting_getties_are_masked_before_sddm(self) -> None:
        commands = "\n".join(
            boot_kde_vnc.LEGACY_EROFS_KDE_OVERLAY_COMMANDS
        )
        self.assertIn(
            "systemctl mask --now getty@tty0.service getty@tty1.service",
            commands,
        )
        self.assertNotIn("serial-getty@", commands)
        self.assertNotIn("systemctl start getty@", commands)
        self.assertNotIn("agetty", commands)
        self.assertNotIn("chpasswd", commands)

    def test_default_kernel_arguments_control_getties(self) -> None:
        command = boot_kde_vnc.build_harness_command(
            boot_kde_vnc.parse_arguments([])
        )
        separator = boot_kde_vnc.guest_argument_separator(command)
        kernel_arguments = command[separator + 1 :]
        self.assertIn("systemd.mask=serial-getty@hvc0.service", kernel_arguments)
        # The starter Plasma session starts from its getty@tty1 autologin.
        self.assertNotIn("systemd.mask=getty@tty0.service", kernel_arguments)
        self.assertNotIn("systemd.mask=getty@tty1.service", kernel_arguments)

    def test_legacy_erofs_kernel_arguments_mask_display_manager_getties(
        self,
    ) -> None:
        command = boot_kde_vnc.build_harness_command(
            boot_kde_vnc.parse_arguments(["--rootfs-type", "erofs"])
        )
        separator = boot_kde_vnc.guest_argument_separator(command)
        kernel_arguments = command[separator + 1 :]
        self.assertIn("systemd.mask=getty@tty0.service", kernel_arguments)
        self.assertIn("systemd.mask=getty@tty1.service", kernel_arguments)

    def test_legacy_erofs_override_retains_uml_compatibility(self) -> None:
        options = boot_kde_vnc.parse_arguments(["--rootfs-type", "erofs"])
        self.assertEqual(options.root_device, "/dev/ubda")
        command = boot_kde_vnc.build_harness_command(options)
        separator = boot_kde_vnc.guest_argument_separator(command)
        commands = "\n".join(command[:separator])
        self.assertIn("getent passwd uml", commands)
        self.assertIn("User=uml", commands)
        self.assertIn("/home/uml", commands)
        self.assertNotIn("useradd", commands)
        self.assertNotIn("getent passwd debian", commands)
        self.assertNotIn("ip=off", command[separator + 1 :])

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
