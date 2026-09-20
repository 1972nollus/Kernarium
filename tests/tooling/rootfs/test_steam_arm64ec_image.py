#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the Steam rootfs architecture and emulator-bridge admission checks."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import importlib.util
from pathlib import Path
import tempfile
import hashlib
import json
import unittest
from unittest.mock import patch
import subprocess
import os
import sys


def load_tool(name: str, relative: str):
    tool_spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    assert tool_spec and tool_spec.loader
    tool = importlib.util.module_from_spec(tool_spec)
    tool_spec.loader.exec_module(tool)
    return tool

from test_wine_arm64ec_pe import write_arm64ec_pe, write_pe


ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("steam_image", ROOT / "tools/rootfs/steam-arm64ec/verify-image.py")
assert spec and spec.loader
image = importlib.util.module_from_spec(spec)
spec.loader.exec_module(image)
boot = load_tool("steam_boot", "tools/rootfs/steam-arm64ec/boot.py")
# The storage and command-line contract is shared with the x86-64 appliance.
launcher = boot.launcher
storage = load_tool("steam_storage", "tools/rootfs/steam-common/overlay/usr/local/libexec/kernarium-storage.py")
installer = load_tool("steam_installer", "tools/rootfs/steam-common/overlay/usr/local/libexec/kernarium-steam.py")
compat = load_tool("steam_compat", "tools/rootfs/steam-arm64ec/overlay/usr/local/libexec/kernarium-compat.py")


def write_elf(path: Path, machine: int) -> None:
    data = bytearray(64)
    data[:6] = b"\x7fELF\x02\x01"
    data[18:20] = machine.to_bytes(2, "little")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


class CompatibilityPolicy(unittest.TestCase):
    def test_targeted_config_preserves_unrelated_values(self) -> None:
        original = '// retained comment\n"Root" { "opaque" "a\\\\b" "Map" { "name" "old" } }\n'
        result = compat.update(original, ("Root", "Map", "name"), "bundled")
        self.assertEqual(result, original.replace('"old"', '"bundled"'))
        result = compat.update(result, ("Root", "New", "value"), "250")
        self.assertEqual(compat.Vdf(result).get(("root", "new", "value")), "250")
        with self.assertRaises(ValueError):
            compat.update('"Root" { "broken"', ("Root", "x"), "y")

    def test_policy_preserves_game_data_and_suspends_only_tools(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            steam = Path(temporary)
            apps = steam / "steamapps"
            apps.mkdir()
            for appid, name in (("602520", "Game"), ("1493710", "Proton Experimental"), ("3127680", "FEX")):
                (apps / f"appmanifest_{appid}.acf").write_text(f'"AppState" {{ "appid" "{appid}" "name" "{name}" }}')
            game = (apps / "appmanifest_602520.acf").read_bytes()
            (apps / "game-save").write_bytes(b"save data")
            compat.apply(steam)
            config = compat.Vdf((steam / "config/config.vdf").read_text())
            path = ("InstallConfigStore", "Software", "Valve", "Steam", "CompatToolMapping")
            for appid in ("0", "602520"):
                self.assertEqual(config.get((*path, appid, "name")), compat.TOOL)
                self.assertEqual(config.get((*path, appid, "priority")), "250")
            self.assertEqual((apps / "appmanifest_602520.acf").read_bytes(), game)
            self.assertEqual((apps / "game-save").read_bytes(), b"save data")
            self.assertEqual(len(list((apps / "kernarium-disabled-tools").glob("*.acf"))), 2)
            before = (steam / "config/config.vdf").read_bytes()
            compat.apply(steam)
            self.assertEqual((steam / "config/config.vdf").read_bytes(), before)


class ImageAudit(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="uml-steam-audit-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        for relative in image.REQUIRED_ELF:
            write_elf(self.root / relative, 183)
        for relative in image.REQUIRED_LIBRARIES:
            library = self.root / relative
            write_elf(library.with_name(library.name + ".1"), 183)
            library.symlink_to(library.name + ".1")
        for relative, architecture in image.REQUIRED_PE.items():
            if architecture == "ARM64EC":
                write_arm64ec_pe(self.root / relative)
            else:
                machine = {"ARM64": 0xAA64, "AMD64": 0x8664, "I386": 0x14C}[architecture]
                write_pe(self.root / relative, machine)
        registry = self.root / image.PROTON / "files/share/wine/wine.inf"
        registry.parent.mkdir(parents=True)
        registry.write_text('"xtajit64.dll"\n"xtajit.dll"\n')
        icd = self.root / "usr/share/vulkan/icd.d/lvp_icd.aarch64.json"
        icd.parent.mkdir(parents=True)
        icd.write_text("{}")
        rule = self.root / "etc/udev/rules.d/70-uml-virtio-video.rules"
        rule.parent.mkdir(parents=True)
        rule.write_text('SUBSYSTEM=="video4linux", SYMLINK+="video-dec%n"\n')
        (self.root / image.PROTON / "version").write_text("test-proton-version\n")
        (self.root / image.PROTON / "toolmanifest.vdf").write_text(
            '"manifest" { "commandline" "/proton %verb%" }\n')
        version = hashlib.sha256(b"test-proton-version\n").hexdigest()
        for relative in ("dist.lock", "files/share/default_pfx", "files/share/default_pfx_arm64"):
            path = self.root / image.PROTON / relative
            path.symlink_to(f"/home/uml/.local/share/proton-{version}-{path.name}")

    def test_proton_rejects_downloadable_runtime(self) -> None:
        manifest = self.root / image.PROTON / "toolmanifest.vdf"
        for appid in ("4185400", "4183110"):
            with self.subTest(appid=appid):
                manifest.write_text('"manifest" { "commandline" "/proton %verb%" '
                                    f'"require_tool_appid" "{appid}" }}\n')
                with self.assertRaisesRegex(RuntimeError, "downloadable Steam runtime"):
                    image.inventory(self.root)

    def test_proton_writable_state_requires_data_partition(self) -> None:
        path = self.root / image.PROTON / "dist.lock"
        path.unlink()
        path.write_text("")
        with self.assertRaisesRegex(RuntimeError, "Proton writable state"):
            image.inventory(self.root)


    def test_vulkan_receipt_rejects_modified_runtime(self) -> None:
        library = self.root / "usr/lib/aarch64-linux-gnu/libvulkan_virtio.so"
        write_elf(library, 183)
        metadata = self.root / "usr/share/uml-steam/vulkan-runtime.json"
        metadata.parent.mkdir(parents=True, exist_ok=True)
        metadata.write_text(json.dumps({"installed_files": [{
            "path": "/usr/lib/aarch64-linux-gnu/libvulkan_virtio.so",
            "sha256": hashlib.sha256(library.read_bytes()).hexdigest(),
        }], "symlinks": {}}))
        icds = self.root / "usr/share/vulkan/icd.d"
        (icds / "lvp_icd.aarch64.json").unlink()
        (icds / "virtio_icd.json").write_text("{}")
        self.assertTrue(image.inventory(self.root))
        library.write_bytes(library.read_bytes() + b"modified")
        with self.assertRaisesRegex(RuntimeError, "installed Vulkan runtime differs"):
            image.inventory(self.root)

    def test_decoder_discovery_rule_required(self) -> None:
        (self.root / "etc/udev/rules.d/70-uml-virtio-video.rules").unlink()
        with self.assertRaisesRegex(RuntimeError, "decoder discovery rule"):
            image.inventory(self.root)

    def test_steam_ui_library_required(self) -> None:
        for relative in image.REQUIRED_LIBRARIES:
            with self.subTest(library=relative):
                library = self.root / relative
                target = library.readlink()
                library.unlink()
                with self.assertRaisesRegex(RuntimeError, "required native Linux library"):
                    image.inventory(self.root)
                library.symlink_to(target)

    def test_native_stack_and_bundled_foreign_steam_assets(self) -> None:
        write_elf(self.root / image.STEAM / "steamrt64/pv-runtime/lib/libc.so", 62)
        self.assertTrue(image.inventory(self.root))

    def test_native_client_cannot_be_replaced_with_x86(self) -> None:
        write_elf(self.root / image.STEAM / "steamrtarm64/steam", 62)
        with self.assertRaisesRegex(RuntimeError, "foreign ELF"):
            image.inventory(self.root)

    def test_guest_system_cannot_gain_x86_elf(self) -> None:
        write_elf(self.root / "usr/bin/foreign", 3)
        with self.assertRaisesRegex(RuntimeError, "foreign ELF"):
            image.inventory(self.root)

    def test_dxvk_must_contain_arm64ec_metadata(self) -> None:
        write_pe(self.root / image.PROTON / "files/lib/wine/dxvk/aarch64-windows/d3d11.dll", 0x8664)
        with self.assertRaisesRegex(RuntimeError, "required ARM64EC"):
            image.inventory(self.root)

    def test_llvm_arm64ec_dxvk(self) -> None:
        write_arm64ec_pe(self.root / image.PROTON / "files/lib/wine/dxvk/aarch64-windows/d3d11.dll", metadata_version=1)
        self.assertTrue(image.inventory(self.root))

    def test_fex_engine_is_rejected(self) -> None:
        write_pe(self.root / image.PROTON / "files/lib/wine/aarch64-windows/libarm64ecfex.dll", 0xA641)
        with self.assertRaisesRegex(RuntimeError, "FEX engine"):
            image.inventory(self.root)

    def test_registry_cannot_select_fex(self) -> None:
        registry = self.root / image.PROTON / "files/share/wine/wine.inf"
        registry.write_text('"libarm64ecfex.dll"\n"xtajit.dll"\n')
        with self.assertRaisesRegex(RuntimeError, "registry still selects"):
            image.inventory(self.root)


class PersistentStorage(unittest.TestCase):
    def test_existing_invalid_disk_is_never_replaced(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            disk = Path(directory) / "userdata.img"
            disk.write_bytes(b"existing user data")
            with patch.object(launcher, "disk_command", return_value="Invalid GPT"):
                with self.assertRaisesRegex(ValueError, "GPT is invalid"):
                    boot.prepare_disk(disk, 32)
            self.assertEqual(disk.read_bytes(), b"existing user data")

    def test_existing_disk_ignores_new_creation_size(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            disk = Path(directory) / "userdata.img"
            disk.write_bytes(b"existing user data")
            with patch.object(launcher, "disk_command", side_effect=[
                    "No problems found", "   1 2048 8388574 4.0 GiB 8300 kernarium-data"]):
                boot.prepare_disk(disk, 64)
            self.assertEqual(disk.read_bytes(), b"existing user data")

    def test_unrecognized_filesystem_is_preserved(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            disk = Path(directory) / "partition"
            disk.write_bytes(b"valuable data" * 200)
            before = disk.read_bytes()
            with patch.object(storage.subprocess, "run", return_value=subprocess.CompletedProcess([], 2, "")) as run:
                with self.assertRaisesRegex(RuntimeError, "preserved for repair"):
                    storage.check_filesystem(disk)
                self.assertEqual(run.call_count, 1)
            self.assertEqual(disk.read_bytes(), before)

    def test_failed_ext4_repair_never_formats(self) -> None:
        with patch.object(storage, "status"), patch.object(storage.subprocess, "run", side_effect=[
                subprocess.CompletedProcess([], 0, "ext4\n"), subprocess.CompletedProcess([], 4)]) as run:
            with self.assertRaisesRegex(RuntimeError, "was not erased"):
                storage.check_filesystem(Path("/dev/test-data"))
            self.assertEqual([call.args[0][0] for call in run.call_args_list], ["blkid", "e2fsck"])

    def test_only_explicit_new_partition_marker_allows_formatting(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            disk = Path(directory) / "partition"
            disk.write_bytes(bytes(1024) + storage.NEW_DISK)
            with patch.object(storage, "status"), patch.object(storage.subprocess, "run", side_effect=[
                    subprocess.CompletedProcess([], 0, ""), subprocess.CompletedProcess([], 0),
                    subprocess.CompletedProcess([], 1)]) as run:
                storage.check_filesystem(disk)
                self.assertEqual([call.args[0][0] for call in run.call_args_list], ["blkid", "mkfs.ext4", "e2fsck"])

    def test_existing_home_is_not_reseeded(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, destination = root / "seed", root / "home"
            source.mkdir()
            destination.mkdir()
            (source / "steam").write_text("old bundled client")
            (destination / "steam").write_text("updated client and saves")
            storage.seed_tree(source, destination, "Installing")
            self.assertEqual((destination / "steam").read_text(), "updated client and saves")

    def test_update_progress_uses_actual_bytes(self) -> None:
        caption, fraction = installer.progress("[2026-09-08] Downloading update (327,680 of 655,360 KB)...")
        self.assertEqual(fraction, 0.5)
        self.assertIn("320.0 of 640.0 MiB", caption)
        self.assertEqual(installer.progress("Extracting package..."), ("Installing Steam • extracting update", None))
        self.assertIsNone(installer.progress("unrelated Steam diagnostic"))

    def test_steam_restart_protocol_and_normal_quit(self) -> None:
        original_popen = subprocess.Popen
        for status, expected_invocations in ((42, 2), (0, 1)):
            with self.subTest(status=status), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                counter = root / "invocations"
                client = root / "client.py"
                client.write_text(
                    "import pathlib, sys\n"
                    "p = pathlib.Path(sys.argv[1])\n"
                    "n = int(p.read_text()) + 1 if p.exists() else 1\n"
                    "p.write_text(str(n))\n"
                    "sys.exit(int(sys.argv[2]) if n == 1 else 0)\n")

                def launch(argv, **kwargs):
                    if argv[0] == "/usr/local/bin/steam":
                        return original_popen([sys.executable, str(client), str(counter), str(status)], **kwargs)
                    return original_popen([sys.executable, "-c", "import time; time.sleep(30)"], **kwargs)

                with patch.object(Path, "home", return_value=root), patch.dict(os.environ, {"XDG_RUNTIME_DIR": directory}), \
                        patch.object(installer.subprocess, "Popen", side_effect=launch), \
                        patch.object(installer, "big_picture_visible", return_value=False):
                    self.assertEqual(installer.main(), 0)
                self.assertEqual(int(counter.read_text()), expected_invocations)

    def test_launcher_has_no_overlay_and_distinct_data_device(self) -> None:
        argv = boot.command(Path("/emulator"), Path("/new-root.erofs"), Path("/data.img"), "8G", "graphical.target", [])
        self.assertIn("--execution-engine=gadgets", argv)
        self.assertIn("--audio", argv)
        with self.assertRaisesRegex(ValueError, "launcher controls"):
            boot.command(Path("/emulator"), Path("/root"), Path("/data"), "8G", "graphical.target", ["--execution-engine=interpreter"])
        self.assertIn("ubd0rc=/new-root.erofs", argv)
        self.assertIn("ubd1=/data.img", argv)
        self.assertIn("ro", argv)
        self.assertNotIn("console=tty0", argv)
        with self.assertRaisesRegex(ValueError, "controls this argument"):
            boot.command(Path("/emulator"), Path("/root.erofs"), Path("/data.img"), "8G", "graphical.target", ["ubd1=/other.img"])


if __name__ == "__main__":
    unittest.main()
