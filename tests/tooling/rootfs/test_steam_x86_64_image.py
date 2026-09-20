#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the pure x86-64 Steam appliance audit, seed pinning, and launcher."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
PRODUCER = "tools/rootfs/steam-x86_64"
STEAM = "usr/share/uml-steam/home/uml/.local/share/Steam"


def load_tool(name: str, relative: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


image = load_tool("steam_x86_image", f"{PRODUCER}/verify-image.py")
inputs = load_tool("steam_x86_inputs", f"{PRODUCER}/prepare-inputs.py")
boot = load_tool("steam_x86_boot", f"{PRODUCER}/boot.py")


def write_elf(path: Path, machine: int) -> None:
    data = bytearray(64)
    data[:6] = b"\x7fELF\x02\x01"
    data[18:20] = machine.to_bytes(2, "little")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


AMD64, I386, AARCH64 = 62, 3, 183


class ImageAudit(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        for relative in image.REQUIRED_ELF:
            write_elf(self.root / relative, AMD64)
        for relative in image.REQUIRED_LIBRARIES:
            write_elf(self.root / relative, AMD64)
        launcher = self.root / "usr/local/bin/steam"
        launcher.parent.mkdir(parents=True, exist_ok=True)
        launcher.write_text('exec "$steam/steamrt64/steam" "$@"\n')
        rule = self.root / "etc/udev/rules.d/70-uml-virtio-video.rules"
        rule.parent.mkdir(parents=True, exist_ok=True)
        rule.write_text('SUBSYSTEM=="video4linux", SYMLINK+="video-dec%n"\n')
        icd = self.root / "usr/share/vulkan/icd.d/virtio_icd.json"
        icd.parent.mkdir(parents=True, exist_ok=True)
        icd.write_text("{}\n")

    def test_complete_image_is_accepted(self) -> None:
        entries = image.inventory(self.root)
        self.assertTrue(entries)
        self.assertTrue(all(row[2] == "AMD64" for row in entries))

    def test_bundled_steam_i386_is_retained_but_system_i386_is_rejected(self) -> None:
        write_elf(self.root / STEAM / "steamrt64/pv-runtime/lib/libsteam.so", I386)
        self.assertIn((f"{STEAM}/steamrt64/pv-runtime/lib/libsteam.so", "ELF", "I386"),
                      image.inventory(self.root))
        write_elf(self.root / "usr/lib/i386-linux-gnu/libc.so.6", I386)
        with self.assertRaisesRegex(RuntimeError, "system i386"):
            image.inventory(self.root)

    def test_foreign_architecture_is_rejected(self) -> None:
        write_elf(self.root / "usr/bin/foreign", AARCH64)
        with self.assertRaisesRegex(RuntimeError, "foreign ELF"):
            image.inventory(self.root)

    def test_native_client_is_required(self) -> None:
        write_elf(self.root / STEAM / "steamrt64/steam", I386)
        with self.assertRaisesRegex(RuntimeError, "absent or foreign"):
            image.inventory(self.root)

    def test_bundled_compatibility_tool_is_rejected(self) -> None:
        (self.root / "opt/proton-uml-arm64ec").mkdir(parents=True)
        with self.assertRaisesRegex(RuntimeError, "no compatibility tool"):
            image.inventory(self.root)

    def test_desktop_environment_is_rejected(self) -> None:
        write_elf(self.root / "usr/bin/startplasma-wayland", AMD64)
        with self.assertRaisesRegex(RuntimeError, "desktop environment"):
            image.inventory(self.root)

    def test_installed_vulkan_runtime_must_match_its_record(self) -> None:
        library = self.root / "usr/lib/x86_64-linux-gnu/libvulkan.so.1.4.0"
        write_elf(library, AMD64)
        metadata = self.root / "usr/share/uml-vulkan/guest-runtime.json"
        metadata.parent.mkdir(parents=True, exist_ok=True)
        metadata.write_text(json.dumps({
            "architecture": {"debian": "amd64"},
            "installed_files": [{"path": "/usr/lib/x86_64-linux-gnu/libvulkan.so.1.4.0",
                                 "sha256": "00" * 32}],
            "symlinks": {}}))
        with self.assertRaisesRegex(RuntimeError, "Vulkan runtime differs"):
            image.inventory(self.root)


class SeedPinning(unittest.TestCase):
    def test_pinned_manifest_matches_the_lock(self) -> None:
        # The x86-64 seed must stay on the client release the ARM64EC appliance
        # seeds, so a first-boot comparison starts from the same client.
        lock = json.loads((ROOT / PRODUCER / "sources.json").read_text())
        self.assertEqual(inputs.validate_policy(lock), inputs.validate_policy(lock))
        arm64 = (ROOT / "tools/rootfs/steam-arm64ec/steam-client.manifest").read_text()
        self.assertIn(f'"{lock["steam_client_version"]}"', arm64)

    def test_package_names_must_be_plain_filenames(self) -> None:
        packages = inputs.client_packages((ROOT / PRODUCER / "steam-client.manifest").read_text())
        self.assertTrue(packages)
        self.assertTrue(all(len(digest) == 64 for _, digest in packages))
        with self.assertRaisesRegex(RuntimeError, "unsafe Steam package name"):
            inputs.client_packages('"pkg" { "file" "../escape.zip" "sha2" "%s" }' % ("0" * 64))

    def test_lock_rejects_a_manifest_it_does_not_describe(self) -> None:
        lock = json.loads((ROOT / PRODUCER / "sources.json").read_text())
        with self.assertRaises(RuntimeError):
            inputs.validate_policy({**lock, "steam_client_version": "1"})
        with self.assertRaises(RuntimeError):
            inputs.validate_policy({**lock, "steam_platform": "linuxarm64"})


class GamingSession(unittest.TestCase):
    def setUp(self) -> None:
        self.unit = (ROOT / "tools/rootfs/steam-common/overlay/etc/systemd/system"
                     "/steam-gaming.service").read_text()

    def test_session_keeps_no_capabilities(self) -> None:
        # A TTY-owning PAM service is handed an ambient capability, and
        # bubblewrap refuses to build Steam's runtime container for a process
        # that holds capabilities without being setuid.
        self.assertIn("\nCapabilityBoundingSet=\n", self.unit)
        self.assertIn("PAMName=login", self.unit)

    def test_session_selects_zink_only_with_the_imported_runtime(self) -> None:
        session = (ROOT / PRODUCER / "overlay/usr/local/bin/steam-gaming-session").read_text()
        self.assertIn("/usr/share/uml-vulkan/guest-runtime.json", session)
        self.assertIn("MESA_LOADER_DRIVER_OVERRIDE=zink", session)
        self.assertIn("GALLIUM_DRIVER=zink", session)
        # Nothing ARM64EC specific belongs in the pure x86-64 session.
        self.assertNotIn("PROTON_USE_ARM64", session)


class Launcher(unittest.TestCase):
    def test_command_line_matches_the_appliance_contract(self) -> None:
        argv = boot.command(Path("/emulator"), Path("/new-root.erofs"), Path("/data.img"),
                            "8G", "graphical.target", ["--video"])
        self.assertIn("--execution-engine=gadgets", argv)
        self.assertIn("--audio", argv)
        self.assertIn("ubd0rc=/new-root.erofs", argv)
        self.assertIn("ubd1=/data.img", argv)
        self.assertIn("init=/usr/local/libexec/kernarium-init", argv)
        self.assertIn("ro", argv)
        self.assertIn("--video", argv)

    def test_storage_and_engine_arguments_are_refused(self) -> None:
        for argument in ("ubd1=/other.img", "root=/dev/ubdb", "init=/bin/sh",
                         "--execution-engine=jit", "mem=1G"):
            with self.subTest(argument=argument), self.assertRaisesRegex(ValueError, "launcher controls"):
                boot.command(Path("/emulator"), Path("/root.erofs"), Path("/data.img"),
                             "8G", "graphical.target", [argument])


if __name__ == "__main__":
    unittest.main()
