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

from test_wine_arm64ec_pe import write_arm64ec_pe, write_pe


ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("steam_image", ROOT / "tools/rootfs/steam-arm64ec/verify-image.py")
assert spec and spec.loader
image = importlib.util.module_from_spec(spec)
spec.loader.exec_module(image)


def write_elf(path: Path, machine: int) -> None:
    data = bytearray(64)
    data[:6] = b"\x7fELF\x02\x01"
    data[18:20] = machine.to_bytes(2, "little")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


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

    def test_vulkan_receipt_rejects_modified_runtime(self) -> None:
        library = self.root / "usr/lib/aarch64-linux-gnu/libvulkan_virtio.so"
        write_elf(library, 183)
        metadata = self.root / "usr/share/uml-steam/vulkan-runtime.json"
        metadata.parent.mkdir(parents=True)
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

    def test_steam_ui_library_required(self) -> None:
        (self.root / image.REQUIRED_LIBRARIES[0]).unlink()
        with self.assertRaisesRegex(RuntimeError, "required native Linux library"):
            image.inventory(self.root)

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


if __name__ == "__main__":
    unittest.main()
