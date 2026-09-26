#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

import receipt_fixtures


REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPT = REPO_ROOT / "tests/integration/gpu/run_chromium_webgl.py"
GENERATOR = REPO_ROOT / "Kernarium/scripts/generate-aquarium-header.py"


def load_module(name: str, path: Path):
    specification = importlib.util.spec_from_file_location(name, path)
    assert specification is not None and specification.loader is not None
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


run_chromium_webgl = load_module("run_chromium_webgl", SCRIPT)
generate_aquarium_header = load_module(
    "generate_aquarium_header", GENERATOR
)


def metric_line(samples: list[float]) -> str:
    raw = ",".join(f"{sample:.3f}" for sample in samples)
    return (
        "[browser] UML_AQUARIUM_OK"
        f"|AVG_FPS={sum(samples) / len(samples):.3f}"
        f"|MIN_FPS={min(samples):.3f}|MAX_FPS={max(samples):.3f}"
        f"|SAMPLES={len(samples)}|WARMUP_SECONDS=15|RESOURCES=73"
        "|FISH=500|CANVAS=440x860|SHADER_COMPILES=18"
        "|SHADER_COMPILE_CPU_MS=7.250|PROGRAM_LINKS=9"
        "|PROGRAM_LINK_CPU_MS=3.125|INSTRUMENTED_METHODS=4"
        f"|INSTRUMENTATION_ERRORS=0|RAW_FPS={raw}"
        "|VERSION=WebGL 2.0 (OpenGL ES 3.0 Chromium)"
        "|RENDERER=ANGLE (Mesa, virgl, OpenGL ES 3.0)"
    )


def guest_measurement_transcript(samples: int = 30) -> str:
    rows = []
    phases = [("BEGIN", 0)]
    phases.extend(("SAMPLE", index) for index in range(1, samples + 1))
    phases.append(("END", 0))
    for offset, (phase, index) in enumerate(phases):
        cpu = (
            100 + offset,
            0,
            20 + offset,
            200 + offset,
            10,
            0,
            5 + offset,
            0,
            0,
            0,
        )
        rows.append(
            f"UML_AQUARIUM_GUEST_{phase}|INDEX={index}"
            f"|UPTIME={100 + offset:.2f}|CLK_TCK=100"
            f"|CPU_JIFFIES={','.join(str(value) for value in cpu)}"
            f"|MEM_AVAILABLE_KIB={900000 - offset}"
            f"|MEM_FREE_KIB={500000 - offset}"
            f"|COMMITTED_AS_KIB={400000 + offset}"
        )
    return "\n".join(rows)


class AquariumMetricTests(unittest.TestCase):
    def test_complete_metric_line_is_parsed_from_raw_samples(self) -> None:
        samples = [20.0 + index / 10 for index in range(30)]
        fields = run_chromium_webgl.parse_metrics(metric_line(samples))
        self.assertEqual(fields["RAW_FPS"], samples)
        self.assertEqual(fields["SAMPLES"], 30)
        self.assertEqual(fields["FISH"], 500)
        self.assertEqual(fields["SHADER_COMPILES"], 18)
        self.assertGreater(fields["P95_FRAME_TIME_MS"], 0)

    def test_chromium_console_source_suffix_is_removed(self) -> None:
        line = metric_line([14.0] * 30).replace(
            "ANGLE (Mesa, virgl, OpenGL ES 3.0)",
            "virgl (Apple M1 Ultra)",
        )
        fields = run_chromium_webgl.parse_metrics(
            line + '", source: chrome-extension://test/verify.js (1)'
        )
        self.assertEqual(fields["RENDERER"], "virgl (Apple M1 Ultra)")

    def test_incomplete_or_inconsistent_metrics_are_rejected(self) -> None:
        with self.assertRaises(RuntimeError):
            run_chromium_webgl.parse_metrics(
                "UML_AQUARIUM_OK|AVG_FPS=10|MIN_FPS=9|MAX_FPS=11"
            )
        with self.assertRaises(RuntimeError):
            run_chromium_webgl.parse_metrics(
                metric_line([10.0] * 30).replace(
                    "|AVG_FPS=10.000", "|AVG_FPS=11.000"
                )
            )
        with self.assertRaises(RuntimeError):
            run_chromium_webgl.parse_metrics(
                metric_line([10.0] * 30).replace(
                    "|INSTRUMENTATION_ERRORS=0",
                    "|INSTRUMENTATION_ERRORS=1",
                )
            )

    def test_sample_stream_requires_exact_order(self) -> None:
        transcript = "\n".join(
            f"UML_AQUARIUM_SAMPLE|INDEX={index}|FPS={index + 0.5:.3f}"
            for index in range(1, 31)
        )
        self.assertEqual(
            run_chromium_webgl.parse_sample_lines(transcript),
            [index + 0.5 for index in range(1, 31)],
        )
        with self.assertRaises(RuntimeError):
            run_chromium_webgl.parse_sample_lines(
                transcript.replace("INDEX=15", "INDEX=16", 1)
            )


class AquariumProbePolicyTests(unittest.TestCase):
    def test_common_probe_freezes_policy_and_main_world(self) -> None:
        script = run_chromium_webgl.build_probe_script()
        self.assertIn('"world": "MAIN"', run_chromium_webgl.EXTENSION_MANIFEST)
        self.assertIn("const warmupSeconds = 15;", script)
        self.assertIn("const sampleSeconds = 30;", script)
        self.assertIn("const fishCount = 500;", script)
        self.assertIn('canvas?.getContext("webgl2")', script)
        self.assertIn("UML_AQUARIUM_MEASURE_BEGIN", script)
        self.assertIn("INSTRUMENTED_METHODS", script)
        self.assertIn("RAW_FPS=", script)
        self.assertNotIn("@WARMUP_SECONDS@", script)

    def test_launcher_pins_angle_and_emits_synchronized_snapshots(self) -> None:
        launcher = run_chromium_webgl.build_guest_launcher(
            auto_poweroff=True
        )
        self.assertIn("--use-gl=angle", launcher)
        self.assertIn("--use-angle=gles-egl", launcher)
        self.assertIn("UML_AQUARIUM_GUEST_IDENTITY", launcher)
        self.assertIn("emit_guest_snapshot SAMPLE", launcher)
        self.assertIn("UML_AQUARIUM_GUEST_POWEROFF|RESULT=pass", launcher)
        self.assertIn("poweroff -f", launcher)
        self.assertIn('[ "0" = 1 ]', launcher)
        self.assertNotIn("@EGL_CONFIG_DIAGNOSTICS@", launcher)

    def test_launcher_can_capture_x11_egl_config_diagnostics(self) -> None:
        launcher = run_chromium_webgl.build_guest_launcher(
            auto_poweroff=False,
            egl_config_diagnostics=True,
        )
        self.assertIn('[ "1" = 1 ]', launcher)
        self.assertIn("UML_EGL_CONFIG_DIAGNOSTICS_BEGIN", launcher)
        self.assertIn("chromium --version", launcher)
        self.assertIn("eglinfo -v -a gles -p x11", launcher)
        self.assertIn("xdpyinfo", launcher)
        self.assertIn("xwininfo -root -stats", launcher)
        self.assertIn("UML_EGL_CONFIG_DIAGNOSTICS_END", launcher)

    def test_launcher_can_require_zink_for_kde_and_chromium(self) -> None:
        launcher = run_chromium_webgl.build_guest_launcher(
            auto_poweroff=False,
            renderer_backend="zink",
        )
        self.assertIn("export MESA_LOADER_DRIVER_OVERRIDE=zink", launcher)
        self.assertIn("UML_KDE_ZINK_READY", launcher)
        self.assertIn("/proc/$kwin_pid/environ", launcher)
        self.assertIn("qdbus6 org.kde.KWin /KWin supportInformation", launcher)
        self.assertIn("OpenGL renderer string:", launcher)
        self.assertIn("virtio-gpu venus", launcher)
        self.assertIn('display_socket="$runtime/wayland-0"', launcher)
        self.assertIn(
            'chromium_platform_flags="--ozone-platform=wayland"',
            launcher,
        )
        self.assertIn("eglinfo -v -a gles -p wayland", launcher)
        self.assertNotIn("@RENDERER_BACKEND@", launcher)

    def test_native_display_reports_frame_statistics(self) -> None:
        with mock.patch.dict(run_chromium_webgl.os.environ, {}, clear=True):
            environment = run_chromium_webgl.build_host_environment("vnc")
        self.assertEqual(environment, {})

        with mock.patch.dict(run_chromium_webgl.os.environ, {}, clear=True):
            environment = run_chromium_webgl.build_host_environment("native")
        self.assertEqual(environment, {"UML_GPU_FRAME_STATS": "1"})

    def test_guest_shutdown_escalates_for_a_wedged_emulator(self) -> None:
        process = mock.Mock()
        process.pid = 1234
        process.poll.side_effect = (None, None, None)
        process.wait.side_effect = (
            subprocess.TimeoutExpired("guest", 45),
            subprocess.TimeoutExpired("guest", 15),
            0,
        )
        with mock.patch.object(run_chromium_webgl.os, "killpg") as killpg:
            run_chromium_webgl.stop_guest(process)
        self.assertEqual(
            killpg.call_args_list,
            [
                mock.call(1234, run_chromium_webgl.signal.SIGINT),
                mock.call(1234, run_chromium_webgl.signal.SIGTERM),
                mock.call(1234, run_chromium_webgl.signal.SIGKILL),
            ],
        )

    def test_renderer_policy_distinguishes_virgl_and_zink(self) -> None:
        line = metric_line([20.0] * 30)
        # Guest Virgl embeds the host renderer, Zink over KosmicKrisp, as a
        # virgl Aquarium run reported it.
        virgl = line.replace(
            "ANGLE (Mesa, virgl, OpenGL ES 3.0)",
            "ANGLE (Mesa, virgl (zink Vulkan 1.4(Apple M1 Ultra "
            "(MESA_KOSMICKRISP))), OpenGL ES 3.2)",
        )
        # Guest Zink on Venus, as a GL CTS session header records it; no zink
        # Aquarium run has yet reported Chromium's ANGLE form of it.
        zink = line.replace(
            "ANGLE (Mesa, virgl, OpenGL ES 3.0)",
            "zink Vulkan 1.4(Virtio-GPU Venus (Apple M1 Ultra) "
            "(MESA_KOSMICKRISP))",
        )
        run_chromium_webgl.validate_renderer(virgl, "virgl")
        run_chromium_webgl.validate_renderer(zink, "zink")
        with self.assertRaisesRegex(RuntimeError, "did not report virgl"):
            run_chromium_webgl.validate_renderer(zink, "virgl")
        with self.assertRaisesRegex(RuntimeError, "did not report zink"):
            run_chromium_webgl.validate_renderer(virgl, "zink")

    def test_checked_vulkan_rootfs_is_accepted_for_zink(self) -> None:
        receipt, contract = run_chromium_webgl.load_vulkan_rootfs_contract(
            receipt_fixtures.current_epoch_receipt_file(
                self, receipt_fixtures.VULKAN_RECEIPT
            )
        )
        self.assertEqual(receipt["artifact"], "uml-vulkan-rootfs")
        self.assertEqual(contract.filesystem_type, "ext4")
        self.assertEqual(contract.sha256, receipt["derivative"]["sha256"])

    def test_kernarium_header_is_generated_from_common_probe(self) -> None:
        driver = generate_aquarium_header.load_driver(SCRIPT)
        header = generate_aquarium_header.generate_header(driver)
        self.assertIn("namespace kernarium::aquarium", header)
        self.assertIn('R"AQUARIUM(', header)
        self.assertIn("Kernarium Aquarium performance probe enabled", header)
        self.assertIn(driver.build_kernarium_setup(), header)

    def test_generated_setup_uses_bounded_lossless_console_lines(self) -> None:
        setup = run_chromium_webgl.build_kernarium_setup()
        self.assertLessEqual(
            max(len(line.encode("utf-8")) for line in setup.splitlines()),
            2048,
        )
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "probe.txt"
            content = "line\n" * 4096
            subprocess.run(
                [
                    "/bin/sh",
                    "-c",
                    run_chromium_webgl.install_text(
                        str(destination), content, "0600"
                    ),
                ],
                check=True,
            )
            self.assertEqual(destination.read_text(encoding="utf-8"), content)
            self.assertEqual(destination.stat().st_mode & 0o777, 0o600)

    def test_only_unrelated_site_favicon_is_ignored(self) -> None:
        script = run_chromium_webgl.EXTENSION_SCRIPT
        self.assertIn('url.pathname === "/favicon.ico"', script)
        self.assertIn("document load did not finish within 600 seconds", script)
        self.assertIn("!isIgnoredSiteDecoration(entry.name)", script)
        self.assertIn("resource returned ", script)


class AquariumResourceMetricTests(unittest.TestCase):
    def test_guest_identity_and_measurement_window_are_parsed(self) -> None:
        transcript = (
            "UML_AQUARIUM_GUEST_IDENTITY|PAGE_SIZE=16384|CPUS=4"
            "|MEM_TOTAL_KIB=1520944\n"
            + guest_measurement_transcript()
        )
        self.assertEqual(
            run_chromium_webgl.parse_guest_identity(transcript),
            {"page_size": 16384, "cpus": 4, "memory_total_kib": 1520944},
        )
        measurement = run_chromium_webgl.parse_guest_snapshots(transcript, 30)
        self.assertEqual(len(measurement["snapshots"]), 32)
        self.assertAlmostEqual(measurement["wall_seconds"], 31.0)
        self.assertGreater(measurement["active_cpu_seconds"], 0)
        self.assertGreater(measurement["average_cpu_cores"], 0)
        self.assertEqual(measurement["minimum_available_memory_kib"], 899969)

    def test_incomplete_guest_window_is_rejected(self) -> None:
        with self.assertRaises(RuntimeError):
            run_chromium_webgl.parse_guest_snapshots(
                guest_measurement_transcript().replace(
                    "UML_AQUARIUM_GUEST_END", "UML_AQUARIUM_GUEST_ERROR"
                ),
                30,
            )

    def test_native_frame_and_shutdown_contracts_fail_closed(self) -> None:
        frames = run_chromium_webgl.parse_native_display_final(
            "Native display final: guest 120 presented 240 dropped 1 "
            "cpu-fallback 0"
        )
        self.assertEqual(frames["cpu_fallback_frames"], 0)
        clean = (
            "UML_AQUARIUM_GUEST_POWEROFF|RESULT=pass\n"
            "Darwin UML stopped: result=0 exitcode=0"
        )
        run_chromium_webgl.validate_native_shutdown(clean)
        with self.assertRaises(RuntimeError):
            run_chromium_webgl.validate_native_shutdown(
                clean + "\nMemory manager not clean during takedown."
            )
        with self.assertRaises(RuntimeError):
            run_chromium_webgl.validate_native_shutdown(
                "Darwin UML stopped: result=0 exitcode=0"
            )

    def test_host_summary_retains_raw_samples(self) -> None:
        begin = {
            "pids": [10, 11],
            "cpu_seconds": 2.0,
            "rss_kib": 100,
            "monotonic_seconds": 50.0,
        }
        end = {
            "pids": [10, 11],
            "cpu_seconds": 5.0,
            "rss_kib": 120,
            "monotonic_seconds": 52.0,
        }
        result = run_chromium_webgl.summarize_host_measurement(
            begin, end, 125, [begin, end]
        )
        self.assertEqual(result["active_cpu_seconds"], 3.0)
        self.assertEqual(result["average_cpu_cores"], 1.5)
        self.assertEqual(result["snapshots"], [begin, end])


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


class AuthenticatedRootfsValidationTests(unittest.TestCase):
    """The erofs branch of validate_rootfs shells out to tools/rootfs/verify.py.

    It reaches for `subprocess` and `sys`, which the module did not import, so
    every erofs-backed runner (run_chromium_webgl, run_webgl_samples) died with
    NameError before it ever launched a guest. The ext4 branch returns earlier
    and hid it.
    """

    def test_erofs_rootfs_validation_invokes_the_image_verifier(self) -> None:
        common = run_chromium_webgl.kernarium_common
        contract = mock.Mock(filesystem_type="erofs")
        with tempfile.TemporaryDirectory() as directory:
            rootfs = Path(directory) / "rootfs.erofs"
            rootfs.write_bytes(b"\x00" * 64)
            identity = {"rootfs": {"size": rootfs.stat().st_size}}
            with mock.patch.object(common.subprocess, "run") as run:
                common.validate_rootfs(rootfs, identity, contract)
            run.assert_called_once()
            argv = run.call_args.args[0]
            self.assertEqual(argv[0], common.sys.executable)
            self.assertIn("--image", argv)
            self.assertIn(str(rootfs), argv)

    def test_erofs_rootfs_of_the_wrong_size_is_refused(self) -> None:
        common = run_chromium_webgl.kernarium_common
        contract = mock.Mock(filesystem_type="erofs")
        with tempfile.TemporaryDirectory() as directory:
            rootfs = Path(directory) / "rootfs.erofs"
            rootfs.write_bytes(b"\x00" * 64)
            identity = {"rootfs": {"size": 65}}
            with mock.patch.object(common.subprocess, "run") as run:
                with self.assertRaises(Exception):
                    common.validate_rootfs(rootfs, identity, contract)
            run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
