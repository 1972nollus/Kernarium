#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import importlib.util
from pathlib import Path
import tempfile
import unittest


SCRIPT = (Path(__file__).resolve().parents[2] /
          "integration/gpu/run_chromium_webgl.py")
SPEC = importlib.util.spec_from_file_location("run_chromium_webgl", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
run_chromium_webgl = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(run_chromium_webgl)


class AquariumMetricTests(unittest.TestCase):
    def test_only_unrelated_site_favicon_is_ignored(self) -> None:
        script = run_chromium_webgl.EXTENSION_SCRIPT
        self.assertIn('url.pathname === "/favicon.ico"', script)
        self.assertIn("!isIgnoredSiteDecoration(entry.name)", script)
        self.assertIn("resource returned ", script)

    def test_complete_metric_line_is_parsed(self) -> None:
        fields = run_chromium_webgl.parse_metrics(
            "[browser] UML_AQUARIUM_OK|AVG_FPS=23.750|MIN_FPS=21.0"
            "|MAX_FPS=26|SAMPLES=30|WARMUP_SECONDS=15|RESOURCES=73"
            "|FISH=500|CANVAS=1024x1024|RENDERER=virgl"
        )
        self.assertEqual(fields["AVG_FPS"], 23.75)
        self.assertEqual(fields["SAMPLES"], 30)
        self.assertEqual(fields["FISH"], 500)

    def test_chromium_console_source_suffix_is_not_part_of_renderer(self) -> None:
        fields = run_chromium_webgl.parse_metrics(
            'UML_AQUARIUM_OK|AVG_FPS=14|MIN_FPS=14|MAX_FPS=14'
            '|SAMPLES=30|WARMUP_SECONDS=15|RESOURCES=113|FISH=500'
            '|CANVAS=1024x1024|RENDERER=virgl (Apple M1 Ultra)", '
            "source: chrome-extension://test/verify.js (1)"
        )
        self.assertEqual(fields["RENDERER"], "virgl (Apple M1 Ultra)")

    def test_incomplete_metric_line_is_rejected(self) -> None:
        with self.assertRaises(RuntimeError):
            run_chromium_webgl.parse_metrics(
                "UML_AQUARIUM_OK|AVG_FPS=10|MIN_FPS=9|MAX_FPS=11"
            )


class AquariumCaptureTests(unittest.TestCase):
    def test_varied_scene_and_raster_marker_are_detected(self) -> None:
        width = 1280
        height = 720
        pixels = bytearray(width * height * 3)
        for y in range(height):
            for x in range(width):
                offset = (y * width + x) * 3
                pixels[offset] = (x * 3) & 255
                pixels[offset + 1] = (y * 5) & 255
                pixels[offset + 2] = ((x + y) * 7) & 255
        for y in range(90, 110):
            for x in range(width - 260, width - 240):
                offset = (y * width + x) * 3
                pixels[offset : offset + 3] = b"\x00\xff\x00"

        with tempfile.TemporaryDirectory() as directory:
            capture = Path(directory) / "aquarium.ppm"
            capture.write_bytes(
                f"P6\n{width} {height}\n255\n".encode("ascii") + pixels
            )
            self.assertTrue(
                run_chromium_webgl.raster_marker_visible(capture)
            )
            self.assertTrue(
                run_chromium_webgl.aquarium_scene_visible(capture)
            )


if __name__ == "__main__":
    unittest.main()
