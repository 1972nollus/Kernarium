#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The desktop image descriptor binds each accepted image to its inputs."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import hashlib
import importlib.util
import json
import pathlib
import re
import subprocess
import tempfile
import unittest

DESKTOP = pathlib.Path(__file__).resolve().parents[3] / "tools/rootfs/desktop"


def load_derive():
    specification = importlib.util.spec_from_file_location(
        "desktop_derive", DESKTOP / "derive.py"
    )
    assert specification is not None and specification.loader is not None
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


class DesktopImageTests(unittest.TestCase):
    def test_accepted_images_name_the_setup_that_produced_them(self) -> None:
        descriptor = json.loads(
            (DESKTOP / "images.json").read_text(encoding="utf-8")
        )
        self.assertEqual(descriptor["schema_version"], 1)
        self.assertEqual(
            set(descriptor["images"]), {"kde", "phrog", "postmarketos"}
        )
        for name, entry in descriptor["images"].items():
            with self.subTest(image=name):
                setup = DESKTOP / entry["setup"]
                self.assertEqual(
                    hashlib.sha256(setup.read_bytes()).hexdigest(),
                    entry["accepted"]["setup_sha256"],
                    "a setup change needs a new derived release",
                )
                self.assertTrue(setup.stat().st_mode & 0o111)
                subprocess.run(["sh", "-n", str(setup)], check=True)
                self.assertRegex(entry["source"]["sha256"], r"^[0-9a-f]{64}$")
                self.assertRegex(
                    entry["accepted"]["sha256"], r"^[0-9a-f]{64}$"
                )
                # A kernel command line names the image, so the name is plain.
                self.assertRegex(
                    entry["accepted"]["file"], r"^[A-Za-z0-9._-]+\.erofs$"
                )
                self.assertRegex(
                    entry["accepted"]["release"], r"^desktop-\d{8}-r\d+$"
                )
                self.assertIsNotNone(
                    re.fullmatch(r"(zstd,level=\d+|lz4hc)", entry["compression"])
                )

    def test_derivation_refuses_a_source_that_does_not_match(self) -> None:
        derive = load_derive()
        with tempfile.TemporaryDirectory() as name:
            directory = pathlib.Path(name)
            source = directory / "rootfs.erofs"
            source.write_bytes(b"not the pinned image")
            with self.assertRaisesRegex(derive.DerivationError, "size"):
                derive.derive(
                    "kde", source, directory / "out.erofs", directory
                )
            with self.assertRaisesRegex(derive.DerivationError, "no image"):
                derive.derive(
                    "gnome", source, directory / "out.erofs", directory
                )
            self.assertEqual(sorted(directory.iterdir()), [source])


if __name__ == "__main__":
    unittest.main()
