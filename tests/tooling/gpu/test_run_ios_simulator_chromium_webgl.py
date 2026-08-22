#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest


REPO_ROOT = Path(__file__).resolve().parents[3]
INTEGRATION_DIR = REPO_ROOT / "tests/integration/gpu"
sys.path.insert(0, str(INTEGRATION_DIR))

import run_ios_simulator_chromium_webgl as runner  # noqa: E402


def metric_line(samples: list[float]) -> str:
    raw = ",".join(f"{sample:.3f}" for sample in samples)
    return (
        "UML_AQUARIUM_OK"
        f"|AVG_FPS={sum(samples) / len(samples):.3f}"
        f"|MIN_FPS={min(samples):.3f}|MAX_FPS={max(samples):.3f}"
        f"|SAMPLES={len(samples)}|WARMUP_SECONDS=15|RESOURCES=40"
        "|FISH=500|CANVAS=440x860|SHADER_COMPILES=12"
        "|SHADER_COMPILE_CPU_MS=4.500|PROGRAM_LINKS=6"
        "|PROGRAM_LINK_CPU_MS=2.250|INSTRUMENTED_METHODS=4"
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


def rootfs_contract(
    filesystem_type: str = "erofs",
) -> runner.cts.RootfsContract:
    identity = runner.cts.load_identity()
    receipt = (
        runner.kernarium.DERIVATIVE_RECEIPT_PATH
        if filesystem_type == "ext4"
        else None
    )
    _, contract = runner.cts.select_rootfs_contract(
        identity, filesystem_type, receipt
    )
    return contract


def macos_evidence(
    directory: Path,
    page_profile: str = "16k",
    contract: runner.cts.RootfsContract | None = None,
) -> Path:
    if contract is None:
        contract = rootfs_contract()
    samples = [10.0 + index / 10 for index in range(30)]
    sample_rows = "\n".join(
        f"UML_AQUARIUM_SAMPLE|INDEX={index + 1}|FPS={sample:.3f}"
        for index, sample in enumerate(samples)
    )
    page_size = 16384 if page_profile == "16k" else 4096
    transcript = "\n".join(
        (
            metric_line(samples),
            sample_rows,
            f"UML_AQUARIUM_GUEST_IDENTITY|PAGE_SIZE={page_size}|CPUS=4"
            "|MEM_TOTAL_KIB=1520944",
            guest_measurement_transcript(),
            "UML_AQUARIUM_GUEST_POWEROFF|RESULT=pass",
            "Native display final: guest 120 presented 240 dropped 1 "
            "cpu-fallback 0",
            "Darwin UML stopped: result=0 exitcode=0",
        )
    )
    console_path = directory / "console.log"
    console_path.write_text(transcript + "\n", encoding="utf-8")
    guest_measurement = runner.aquarium.parse_guest_snapshots(transcript, 30)
    native_display = runner.aquarium.parse_native_display_final(transcript)
    metadata = {
        "schema_version": 2,
        "adapter": "macos",
        "workload": "webgl-aquarium-500-v1",
        "git": {"revision": "0" * 40, "dirty_paths": []},
        "rootfs_contract": runner.aquarium.rootfs_contract_metadata(
            contract
        ),
        "display": "native",
        "page_profile": page_profile,
        "engine": "gadgets",
        "cpus": 4,
        "memory": "1536M",
        "width": 440,
        "height": 860,
        "dpi": 160,
        "fish": 500,
        "warmup_seconds": 15,
        "sample_seconds": 30,
        "emulator_return_code": 0,
        "clean_shutdown": True,
        "metrics": {
            "guest_measurement": guest_measurement,
            "native_display_final": native_display,
        },
    }
    run_path = directory / "run.json"
    run_path.write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return run_path


class SimulatorAquariumTests(unittest.TestCase):
    def test_dry_run_freezes_simulator_configuration(self) -> None:
        output = io.StringIO()
        with redirect_stdout(output):
            status = runner.main(
                [
                    "--macos-run",
                    "/tmp/matching-run.json",
                    "--page-profile",
                    "4k",
                    "--dry-run",
                ]
            )
        self.assertEqual(status, 0)
        payload = json.loads(output.getvalue())
        environment = payload["launch_environment"]
        self.assertEqual(
            environment["SIMCTL_CHILD_KERNARIUM_KERNEL_PAGE_SIZE"], "4096"
        )
        self.assertEqual(
            environment["SIMCTL_CHILD_KERNARIUM_EXECUTION_ENGINE"], "gadgets"
        )
        self.assertEqual(
            environment["SIMCTL_CHILD_KERNARIUM_GUEST_MEMORY_MIB"], "4096"
        )
        self.assertEqual(environment["SIMCTL_CHILD_KERNARIUM_VCPU_COUNT"], "4")
        self.assertEqual(environment["SIMCTL_CHILD_KERNARIUM_RETINA_MODE"], "0")
        self.assertEqual(environment["SIMCTL_CHILD_UML_GPU_FRAME_STATS"], "1")

        output = io.StringIO()
        with redirect_stdout(output):
            status = runner.main(
                [
                    "--macos-run",
                    "/tmp/matching-run.json",
                    "--rootfs",
                    str(REPO_ROOT / ".build/rootfs/rootfs-gl-gles.ext4"),
                    "--rootfs-type",
                    "ext4",
                    "--rootfs-receipt",
                    str(runner.kernarium.DERIVATIVE_RECEIPT_PATH),
                    "--dry-run",
                ]
            )
        self.assertEqual(status, 0)
        payload = json.loads(output.getvalue())
        self.assertEqual(payload["rootfs_type"], "ext4")
        self.assertEqual(
            payload["launch_environment"]["SIMCTL_CHILD_KERNARIUM_ROOTFS"],
            "rootfs.ext4",
        )

    def test_macos_predecessor_is_reparsed_and_shutdown_is_real(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            contract = rootfs_contract("ext4")
            run_path = macos_evidence(directory, contract=contract)
            metadata = runner.validate_macos_run(
                run_path, "16k", contract
            )
            self.assertTrue(metadata["clean_shutdown"])

            with self.assertRaises(runner.cts.CampaignError):
                runner.validate_macos_run(
                    run_path, "16k", rootfs_contract("erofs")
                )

            console_path = directory / "console.log"
            transcript = console_path.read_text(encoding="utf-8")
            console_path.write_text(
                transcript.replace(
                    "UML_AQUARIUM_GUEST_POWEROFF|RESULT=pass\n", ""
                ),
                encoding="utf-8",
            )
            with self.assertRaises(runner.cts.CampaignError):
                runner.validate_macos_run(run_path, "16k", contract)

    def test_process_match_requires_exact_executable(self) -> None:
        executable = Path("/tmp/Kernarium.app/Kernarium")
        output = (
            " 101 /tmp/Kernarium.app/Kernarium\n"
            " 102 /tmp/Kernarium.app/Kernarium --argument\n"
            " 103 /tmp/Kernarium.app/Kernarium-helper\n"
            " bad /tmp/Kernarium.app/Kernarium\n"
        )
        self.assertEqual(
            runner.matching_process_ids(output, executable), [101, 102]
        )

    def test_aquarium_is_compiled_out_on_physical_device(self) -> None:
        cmake = (
            REPO_ROOT / "Kernarium/Runtime/CMakeLists.txt"
        ).read_text(encoding="utf-8")
        runtime = (
            REPO_ROOT / "Kernarium/Runtime/KernariumRuntime.mm"
        ).read_text(encoding="utf-8")
        self.assertIn(
            'if(UML_APPLE_PLATFORM STREQUAL "ios-simulator")', cmake
        )
        self.assertIn("set(_kernarium_aquarium_supported 0)", cmake)
        self.assertIn("KERNARIUM_AQUARIUM_SUPPORTED == 0", runtime)
        self.assertIn("WebGL Aquarium is Simulator-only", runtime)


if __name__ == "__main__":
    unittest.main()
