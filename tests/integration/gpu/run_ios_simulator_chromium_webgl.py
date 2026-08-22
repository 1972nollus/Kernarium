#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the frozen native WebGL Aquarium canary in Kernarium Simulator."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time
from typing import Any, Sequence

import kernarium_vk_gl_cts_common as kernarium
import run_chromium_webgl as aquarium
import run_ios_simulator_vk_gl_cts as simulator
import run_vk_gl_cts as cts


REPO_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_APP = simulator.DEFAULT_APP
DEFAULT_ARTIFACT_ROOT = REPO_ROOT / ".build/chromium-webgl-ios-simulator"
FIXED_CPUS = 4
FIXED_MACOS_MEMORY_MIB = 1536
FIXED_SIMULATOR_MEMORY_MIB = 4096
FIXED_OVERLAY_GIB = 8
FIXED_WIDTH = 440
FIXED_HEIGHT = 860
FIXED_DPI = 160


def validate_macos_run(path: Path, page_profile: str) -> dict[str, Any]:
    try:
        metadata = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise cts.CampaignError(
            f"cannot read matching macOS Aquarium run: {error}"
        ) from error
    required = {
        "adapter": "macos",
        "workload": "webgl-aquarium-500-v1",
        "display": "native",
        "page_profile": page_profile,
        "engine": "gadgets",
        "cpus": FIXED_CPUS,
        "memory": f"{FIXED_MACOS_MEMORY_MIB}M",
        "width": FIXED_WIDTH,
        "height": FIXED_HEIGHT,
        "dpi": FIXED_DPI,
        "fish": aquarium.FISH_COUNT,
        "warmup_seconds": aquarium.DEFAULT_WARMUP_SECONDS,
        "sample_seconds": aquarium.DEFAULT_SAMPLE_SECONDS,
        "emulator_return_code": 0,
        "clean_shutdown": True,
    }
    if any(metadata.get(key) != value for key, value in required.items()):
        raise cts.CampaignError(
            "Simulator Aquarium requires the matching fixed native macOS run"
        )
    console_path = path.parent / "console.log"
    try:
        transcript = console_path.read_text(encoding="utf-8", errors="strict")
    except (OSError, UnicodeError) as error:
        raise cts.CampaignError(
            f"cannot read matching macOS Aquarium console: {error}"
        ) from error
    try:
        result_line = next(
            line for line in transcript.splitlines()
            if "UML_AQUARIUM_OK|" in line
        )
        metrics = aquarium.parse_metrics(result_line)
        samples = aquarium.parse_sample_lines(transcript)
        guest = aquarium.parse_guest_identity(transcript)
        guest_measurement = aquarium.parse_guest_snapshots(
            transcript, aquarium.DEFAULT_SAMPLE_SECONDS
        )
        native_display = aquarium.parse_native_display_final(transcript)
    except (RuntimeError, StopIteration) as error:
        raise cts.CampaignError(
            f"matching macOS Aquarium evidence is invalid: {error}"
        ) from error
    if (
        samples != metrics["RAW_FPS"]
        or guest["page_size"] != (16384 if page_profile == "16k" else 4096)
        or guest["cpus"] != FIXED_CPUS
        or native_display["cpu_fallback_frames"] != 0
        or metadata.get("metrics", {}).get("guest_measurement")
            != guest_measurement
        or metadata.get("metrics", {}).get("native_display_final")
            != native_display
    ):
        raise cts.CampaignError("matching macOS Aquarium evidence changed")
    try:
        aquarium.validate_native_shutdown(transcript)
    except RuntimeError as error:
        raise cts.CampaignError(
            f"matching macOS Aquarium shutdown is invalid: {error}"
        ) from error
    return metadata


def matching_process_ids(ps_output: str, executable: Path) -> list[int]:
    expected = str(executable)
    matches: list[int] = []
    for line in ps_output.splitlines():
        stripped = line.strip()
        pid_text, separator, command = stripped.partition(" ")
        if not separator or command.split(" ", 1)[0] != expected:
            continue
        try:
            matches.append(int(pid_text))
        except ValueError:
            continue
    return matches


def process_for_executable(executable: Path, timeout: float = 30.0) -> int:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = subprocess.run(
            ["ps", "-axo", "pid=,command="],
            check=True,
            capture_output=True,
            text=True,
        )
        matches = matching_process_ids(result.stdout, executable)
        if len(matches) == 1:
            return matches[0]
        if len(matches) > 1:
            raise cts.CampaignError(
                "multiple installed Kernarium apps are running"
            )
        time.sleep(0.25)
    raise cts.CampaignError("cannot locate the running Kernarium process")


def wait_for_runtime_result(
    process: subprocess.Popen[bytes],
    log_path: Path,
    timeout: float,
) -> tuple[str, int]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        transcript = log_path.read_text(encoding="utf-8", errors="replace")
        matches = kernarium.RUNTIME_RESULT.findall(transcript)
        if matches:
            return transcript, int(matches[-1])
        if process.poll() is not None:
            return transcript, -1
        time.sleep(0.25)
    raise TimeoutError("Simulator Aquarium guest did not shut down")


def parse_arguments(arguments: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="run the fixed Aquarium canary in Kernarium Simulator"
    )
    parser.add_argument("--page-profile", choices=("16k", "4k"), default="16k")
    parser.add_argument("--device", default="booted")
    parser.add_argument("--macos-run", type=Path, required=True)
    parser.add_argument("--app", type=Path, default=DEFAULT_APP)
    parser.add_argument("--rootfs", type=Path, default=cts.DEFAULT_ROOTFS)
    parser.add_argument(
        "--artifact-root", type=Path, default=DEFAULT_ARTIFACT_ROOT
    )
    parser.add_argument(
        "--guest-timeout", type=cts.positive_integer, default=1200
    )
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args(arguments)


def main(arguments: Sequence[str] | None = None) -> int:
    options = parse_arguments(arguments)
    guest_page_size = 16384 if options.page_profile == "16k" else 4096
    macos_run = options.macos_run.resolve()
    launch_environment = {
        "SIMCTL_CHILD_KERNARIUM_WEBGL_AQUARIUM": "1",
        "SIMCTL_CHILD_KERNARIUM_KERNEL_PAGE_SIZE": str(guest_page_size),
        "SIMCTL_CHILD_KERNARIUM_ROOTFS": "rootfs.erofs",
        "SIMCTL_CHILD_KERNARIUM_EXECUTION_ENGINE": "gadgets",
        "SIMCTL_CHILD_KERNARIUM_VCPU_COUNT": str(FIXED_CPUS),
        "SIMCTL_CHILD_KERNARIUM_GUEST_MEMORY_MIB": str(
            FIXED_SIMULATOR_MEMORY_MIB
        ),
        "SIMCTL_CHILD_KERNARIUM_OVERLAY_SIZE_GIB": str(FIXED_OVERLAY_GIB),
        "SIMCTL_CHILD_KERNARIUM_RETINA_MODE": "0",
        "SIMCTL_CHILD_UML_GPU_FRAME_STATS": "1",
    }
    if options.dry_run:
        print(
            json.dumps(
                {
                    "page_profile": options.page_profile,
                    "macos_run": str(macos_run),
                    "launch_environment": launch_environment,
                },
                indent=2,
                sort_keys=True,
            )
        )
        return 0

    identity = cts.load_identity()
    macos_metadata = validate_macos_run(macos_run, options.page_profile)
    app = options.app.resolve()
    rootfs = options.rootfs.resolve()
    kernarium.validate_app(app)
    kernarium.validate_rootfs(rootfs, identity)
    device = simulator.resolve_device(options.device)
    simulator.require_booted_device(device)
    installed_rootfs = simulator.provision(device, app, rootfs)
    installed_app = Path(
        simulator.checked_output(
            [
                "xcrun",
                "simctl",
                "get_app_container",
                device,
                kernarium.BUNDLE_ID,
                "app",
            ]
        )
    )
    installed_executable = installed_app / "Kernarium"

    timestamp = time.strftime("%Y%m%d-%H%M%S")
    artifact = options.artifact_root.resolve() / (
        f"{timestamp}-{os.getpid()}-{options.page_profile}-native"
    )
    artifact.mkdir(parents=True, exist_ok=False)
    command = [
        "xcrun",
        "simctl",
        "launch",
        "--console-pty",
        "--terminate-running-process",
        device,
        kernarium.BUNDLE_ID,
    ]
    metadata: dict[str, Any] = {
        "schema_version": 1,
        "adapter": "ios-simulator-kernarium",
        "workload": "webgl-aquarium-500-v1",
        "git": aquarium.git_state(),
        "device": {
            "udid": device,
            "name": simulator.simulator_environment(
                device, "SIMULATOR_DEVICE_NAME"
            ),
            "model": simulator.simulator_environment(
                device, "SIMULATOR_MODEL_IDENTIFIER"
            ),
            "runtime": simulator.simulator_environment(
                device, "SIMULATOR_RUNTIME_VERSION"
            ),
        },
        "app": str(app),
        "rootfs": str(installed_rootfs),
        "rootfs_sha256": identity["rootfs"]["sha256"],
        "page_profile": options.page_profile,
        "engine": "gadgets",
        "cpus": FIXED_CPUS,
        "memory_mib": FIXED_SIMULATOR_MEMORY_MIB,
        "overlay_gib": FIXED_OVERLAY_GIB,
        "retina": False,
        "expected_width": FIXED_WIDTH,
        "expected_height": FIXED_HEIGHT,
        "expected_dpi": FIXED_DPI,
        "fish": aquarium.FISH_COUNT,
        "warmup_seconds": aquarium.DEFAULT_WARMUP_SECONDS,
        "sample_seconds": aquarium.DEFAULT_SAMPLE_SECONDS,
        "macos_run": str(macos_run),
        "macos_git": macos_metadata["git"],
        "command": command,
        "launch_environment": launch_environment,
    }
    cts.write_json(artifact / "run.json", metadata)

    environment = os.environ.copy()
    for key in (
        "SIMCTL_CHILD_KERNARIUM_CTS_CAMPAIGN",
        "SIMCTL_CHILD_KERNARIUM_GEEKBENCH",
        "SIMCTL_CHILD_KERNARIUM_GEEKBENCH_WORKLOADS",
    ):
        environment.pop(key, None)
    environment.update(launch_environment)
    log_path = artifact / "console.log"
    runtime_status = -1
    with log_path.open("wb") as log:
        process = subprocess.Popen(
            command, stdout=log, stderr=subprocess.STDOUT, env=environment
        )
        try:
            app_pid = process_for_executable(installed_executable)
            _, host_measurement = aquarium.wait_for_log(
                process,
                log_path,
                (
                    "UML_AQUARIUM_OK|",
                    "UML_AQUARIUM_ERROR|",
                    "UML_CHROMIUM_ERROR:",
                ),
                options.guest_timeout,
                host_process_pid=app_pid,
            )
            transcript, runtime_status = wait_for_runtime_result(
                process, log_path, 180
            )
        finally:
            simulator.stop_app(device)
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.terminate()
                process.wait(timeout=5)

    metadata["simctl_return_code"] = process.returncode
    metadata["runtime_return_code"] = runtime_status
    if runtime_status != 0:
        cts.write_json(artifact / "run.json", metadata)
        raise cts.CampaignError(
            f"Simulator Aquarium guest returned {runtime_status}"
        )
    kernarium.validate_clean_shutdown(transcript)
    cts.validate_host_backend(transcript)
    try:
        result_line = next(
            line for line in transcript.splitlines()
            if "UML_AQUARIUM_OK|" in line
        )
    except StopIteration as error:
        raise cts.CampaignError(
            "Simulator Aquarium success marker is missing"
        ) from error
    metrics = aquarium.parse_metrics(result_line)
    samples = aquarium.parse_sample_lines(transcript)
    if samples != metrics["RAW_FPS"]:
        raise cts.CampaignError("Simulator Aquarium sample stream changed")
    guest_identity = aquarium.parse_guest_identity(transcript)
    if (
        guest_identity["page_size"] != guest_page_size
        or guest_identity["cpus"] != FIXED_CPUS
        or f"Native Metal display: {FIXED_WIDTH}x{FIXED_HEIGHT}" not in transcript
    ):
        raise cts.CampaignError("Simulator Aquarium used another configuration")
    guest_measurement = aquarium.parse_guest_snapshots(
        transcript, aquarium.DEFAULT_SAMPLE_SECONDS
    )
    native_display = aquarium.parse_native_display_final(transcript)
    if host_measurement is None:
        raise cts.CampaignError("Simulator Aquarium host measurement is missing")
    metrics.update({
        "host_measurement": host_measurement,
        "guest_identity": guest_identity,
        "guest_measurement": guest_measurement,
        "native_display_final": native_display,
    })
    metadata.update({
        "metrics": metrics,
        "clean_shutdown": True,
    })
    cts.write_json(artifact / "run.json", metadata)
    print(
        "Simulator Aquarium steady-state average: "
        f"{metrics['AVG_FPS']:.3f} FPS ({metrics['SAMPLES']} samples)"
    )
    print(f"Artifacts: {artifact}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print(
            "run_ios_simulator_chromium_webgl.py: interrupted; app stopped",
            file=sys.stderr,
        )
        raise SystemExit(130) from None
    except (
        cts.CampaignError,
        OSError,
        RuntimeError,
        subprocess.SubprocessError,
        TimeoutError,
    ) as error:
        print(
            f"run_ios_simulator_chromium_webgl.py: {error}",
            file=sys.stderr,
        )
        raise SystemExit(1) from error
