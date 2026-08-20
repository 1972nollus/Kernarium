#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run one allow-listed VK-GL-CTS campaign through the signed Simulator app."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
from typing import Any, Sequence

import kernarium_vk_gl_cts_common as common
import run_vk_gl_cts as cts


REPO_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_APP = (
    REPO_ROOT
    / ".build/kernarium-xcode-derived/Build/Products/Debug-iphonesimulator"
    / "Kernarium.app"
)
DEFAULT_ARTIFACT_ROOT = REPO_ROOT / ".build/vk-gl-cts-ios-simulator"


def checked_output(arguments: Sequence[str]) -> str:
    return subprocess.run(
        arguments,
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()


def resolve_device(requested: str) -> str:
    if requested != "booted":
        return requested
    device = checked_output(
        ["xcrun", "simctl", "getenv", "booted", "SIMULATOR_UDID"]
    )
    if not device:
        raise cts.CampaignError("no booted Simulator is available")
    return device


def require_booted_device(device: str) -> None:
    result = subprocess.run(
        ["xcrun", "simctl", "getenv", device, "SIMULATOR_UDID"],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0 or result.stdout.strip() != device:
        raise cts.CampaignError(f"Simulator is not booted: {device}")


def simulator_environment(device: str, name: str) -> str | None:
    result = subprocess.run(
        ["xcrun", "simctl", "getenv", device, name],
        capture_output=True,
        text=True,
    )
    value = result.stdout.strip()
    return value if result.returncode == 0 and value else None


def provision(device: str, app: Path, rootfs: Path) -> Path:
    subprocess.run(
        ["xcrun", "simctl", "terminate", device, common.BUNDLE_ID],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    subprocess.run(
        ["xcrun", "simctl", "install", device, str(app)], check=True
    )
    container = Path(
        checked_output(
            [
                "xcrun",
                "simctl",
                "get_app_container",
                device,
                common.BUNDLE_ID,
                "data",
            ]
        )
    )
    if not container.is_dir():
        raise cts.CampaignError("cannot locate the Simulator app data container")
    documents = container / "Documents"
    documents.mkdir(parents=True, exist_ok=True)
    installed_rootfs = documents / "rootfs.erofs"
    shutil.copyfile(rootfs, installed_rootfs)
    if installed_rootfs.stat().st_size != rootfs.stat().st_size:
        raise cts.CampaignError("Simulator rootfs copy is incomplete")
    return installed_rootfs


def stop_app(device: str) -> None:
    subprocess.run(
        ["xcrun", "simctl", "terminate", device, common.BUNDLE_ID],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def parse_arguments(arguments: Sequence[str] | None = None) -> argparse.Namespace:
    identity = cts.load_identity()
    campaigns = cts.load_apple_campaigns(identity)
    parser = argparse.ArgumentParser(
        description="run an allow-listed CTS campaign in Kernarium Simulator"
    )
    parser.add_argument("--campaign", required=True, choices=sorted(campaigns))
    parser.add_argument("--page-profile", choices=("16k", "4k"), default="16k")
    parser.add_argument("--device", default="booted")
    parser.add_argument("--macos-run", type=Path, required=True)
    parser.add_argument("--app", type=Path, default=DEFAULT_APP)
    parser.add_argument("--rootfs", type=Path, default=cts.DEFAULT_ROOTFS)
    parser.add_argument("--artifact-root", type=Path, default=DEFAULT_ARTIFACT_ROOT)
    parser.add_argument("--guest-timeout", type=cts.positive_integer, default=1200)
    parser.add_argument("--qualification", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    options = parser.parse_args(arguments)
    if options.qualification and not identity["cts"]["qualification_ready"]:
        parser.error(identity["cts"]["qualification_blocker"])
    options.identity = identity
    options.campaigns = campaigns
    return options


def main(arguments: Sequence[str] | None = None) -> int:
    options = parse_arguments(arguments)
    campaign = options.campaigns[options.campaign]
    guest_page_size = 16384 if options.page_profile == "16k" else 4096
    macos_run = options.macos_run.resolve()
    launch_environment = {
        "SIMCTL_CHILD_KERNARIUM_CTS_CAMPAIGN": campaign.identifier,
        "SIMCTL_CHILD_KERNARIUM_KERNEL_PAGE_SIZE": str(guest_page_size),
        "SIMCTL_CHILD_KERNARIUM_ROOTFS": "rootfs.erofs",
        "SIMCTL_CHILD_KERNARIUM_EXECUTION_ENGINE": "gadgets",
        "SIMCTL_CHILD_UML_GPU_CAPABILITY_REPORT": "1",
        "SIMCTL_CHILD_UML_GPU_FRAME_STATS": "1",
    }
    if options.dry_run:
        print(
            json.dumps(
                {
                    "campaign": campaign.identifier,
                    "exact_case": campaign.exact_case,
                    "page_profile": options.page_profile,
                    "macos_run": str(macos_run),
                    "launch_environment": launch_environment,
                },
                indent=2,
                sort_keys=True,
            )
        )
        return 0

    app = options.app.resolve()
    rootfs = options.rootfs.resolve()
    macos_metadata = common.validate_macos_evidence(
        macos_run, campaign, options.page_profile
    )
    common.validate_app(app)
    common.validate_rootfs(rootfs, options.identity)
    device = resolve_device(options.device)
    require_booted_device(device)
    installed_rootfs = provision(device, app, rootfs)

    timestamp = time.strftime("%Y%m%d-%H%M%S")
    artifact_directory = options.artifact_root.resolve() / (
        f"{timestamp}-{os.getpid()}-{options.page_profile}-{campaign.identifier}"
    )
    artifact_directory.mkdir(parents=True, exist_ok=False)
    command = [
        "xcrun",
        "simctl",
        "launch",
        "--console-pty",
        "--terminate-running-process",
        device,
        common.BUNDLE_ID,
    ]
    metadata: dict[str, Any] = {
        "schema_version": 1,
        "git": cts.git_state(),
        "adapter": "ios-simulator-kernarium",
        "backend": "angle-metal",
        "device": {
            "udid": device,
            "name": simulator_environment(device, "SIMULATOR_DEVICE_NAME"),
            "model": simulator_environment(device, "SIMULATOR_MODEL_IDENTIFIER"),
            "runtime": simulator_environment(device, "SIMULATOR_RUNTIME_VERSION"),
        },
        "app": str(app),
        "rootfs": str(installed_rootfs),
        "rootfs_sha256": options.identity["rootfs"]["sha256"],
        "page_profile": options.page_profile,
        "engine": "gadgets",
        "campaign": campaign.identifier,
        "api": campaign.api,
        "leaf": campaign.leaf.identifier,
        "leaf_sha256": campaign.leaf.sha256,
        "exact_case": campaign.exact_case,
        "expected_cases": 1,
        "seed": campaign.seed,
        "qualification": options.qualification,
        "macos_run": str(macos_run),
        "macos_qpa_sha256": macos_metadata["qpa_sha256"],
        "macos_renderer_sha256": macos_metadata["renderer_sha256"],
        "command": command,
        "launch_environment": launch_environment,
    }
    cts.write_json(artifact_directory / "run.json", metadata)

    environment = os.environ.copy()
    for key in (
        "SIMCTL_CHILD_KERNARIUM_WEBGL_AQUARIUM",
        "SIMCTL_CHILD_KERNARIUM_GEEKBENCH",
        "SIMCTL_CHILD_KERNARIUM_GEEKBENCH_WORKLOADS",
    ):
        environment.pop(key, None)
    environment.update(launch_environment)
    log_path = artifact_directory / "console.log"
    runtime_status = -1
    with log_path.open("wb") as log:
        process = subprocess.Popen(
            command, stdout=log, stderr=subprocess.STDOUT, env=environment
        )
        try:
            transcript, runtime_status = common.wait_for_runtime(
                process,
                log_path,
                options.guest_timeout,
                "Simulator",
            )
        finally:
            stop_app(device)
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.terminate()
                process.wait(timeout=5)

    metadata["simctl_return_code"] = process.returncode
    metadata["runtime_return_code"] = runtime_status
    cts.write_json(artifact_directory / "run.json", metadata)
    if runtime_status != 0:
        guest_errors = cts.GUEST_ERROR.findall(transcript)
        detail = f": {guest_errors[-1]}" if guest_errors else ""
        raise cts.CampaignError(
            f"Simulator guest returned {runtime_status}{detail}"
        )

    (
        artifacts,
        results,
        totals,
        capability,
        native_display,
    ) = common.validate_transcript(transcript, campaign, guest_page_size)
    output_directory = artifact_directory / "artifacts"
    cts.write_artifacts(artifacts, output_directory)
    capset_comparison = cts.virgl_capset.compare_reports(
        macos_metadata["virgl_capsets"], capability["virgl_capsets"]
    )
    context_differences = cts.virgl_capset.structured_differences(
        macos_metadata["host_contexts"], capability["host_contexts"]
    )
    metadata.update(
        {
            **capability,
            "native_display_final": native_display,
            "completed_cases": len(results),
            "status_totals": totals,
            "selected_caselist_sha256": hashlib.sha256(
                artifacts["selected.txt"]
            ).hexdigest(),
            "renderer_sha256": hashlib.sha256(
                artifacts["renderer.txt"]
            ).hexdigest(),
            "qpa_sha256": hashlib.sha256(artifacts["results.qpa"]).hexdigest(),
            "macos_host_contexts_equal":
                not context_differences,
            "macos_host_context_differences": context_differences,
            "macos_virgl_capsets_equal": capset_comparison["equal"],
            "macos_virgl_capset_comparison": capset_comparison,
        }
    )
    cts.write_json(artifact_directory / "run.json", metadata)
    cts.validate_required_passes(results)
    print(f"Simulator VK-GL-CTS completed {len(results)} cases: {totals}")
    print(f"Artifacts: {artifact_directory}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print(
            "run_ios_simulator_vk_gl_cts.py: interrupted; app stopped",
            file=sys.stderr,
        )
        raise SystemExit(130) from None
    except (
        cts.CampaignError,
        OSError,
        subprocess.SubprocessError,
        TimeoutError,
    ) as error:
        print(f"run_ios_simulator_vk_gl_cts.py: {error}", file=sys.stderr)
        raise SystemExit(1) from error
