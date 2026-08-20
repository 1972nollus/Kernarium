#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run one Simulator-proven VK-GL-CTS campaign on a connected iPad."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
from typing import Any, Sequence

import kernarium_vk_gl_cts_common as common
import run_vk_gl_cts as cts


REPO_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_APP = (
    REPO_ROOT
    / ".build/kernarium-device-derived/Build/Products/Release-iphoneos"
    / "Kernarium.app"
)
DEFAULT_ARTIFACT_ROOT = REPO_ROOT / ".build/vk-gl-cts-ios-device"


def device_details(device: str) -> dict[str, Any]:
    with tempfile.TemporaryDirectory() as temporary:
        output = Path(temporary) / "device.json"
        subprocess.run(
            [
                "xcrun",
                "devicectl",
                "device",
                "info",
                "details",
                "--device",
                device,
                "--json-output",
                str(output),
                "--quiet",
            ],
            check=True,
        )
        try:
            document = json.loads(output.read_text(encoding="utf-8"))
            if document["info"]["outcome"] != "success":
                raise KeyError("outcome")
            result = document["result"]
            hardware = result["hardwareProperties"]
            properties = result["deviceProperties"]
            connection = result["connectionProperties"]
        except (OSError, json.JSONDecodeError, KeyError, TypeError) as error:
            raise cts.CampaignError(
                "devicectl returned an invalid device identity"
            ) from error
    if (
        hardware.get("deviceType") != "iPad"
        or hardware.get("platform") != "iOS"
        or hardware.get("reality") != "physical"
        or properties.get("bootState") != "booted"
        or properties.get("developerModeStatus") != "enabled"
        or connection.get("pairingState") != "paired"
        or connection.get("tunnelState") != "connected"
    ):
        raise cts.CampaignError(
            "device must be a booted, paired, developer-enabled physical iPad"
        )
    return {
        "identifier": device,
        "name": properties.get("name"),
        "model": hardware.get("marketingName"),
        "product_type": hardware.get("productType"),
        "os_version": properties.get("osVersionNumber"),
        "os_build": properties.get("osBuildUpdate"),
        "transport": connection.get("transportType"),
    }


def validate_simulator_evidence(
    path: Path,
    campaign: cts.AppleCampaign,
    page_profile: str,
) -> dict[str, Any]:
    try:
        metadata = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise cts.CampaignError(
            f"cannot read matching Simulator evidence: {error}"
        ) from error
    required = {
        "adapter": "ios-simulator-kernarium",
        "backend": "angle-metal",
        "campaign": campaign.identifier,
        "page_profile": page_profile,
        "runtime_return_code": 0,
        "completed_cases": 1,
        "status_totals": {"Pass": 1},
    }
    if any(metadata.get(key) != value for key, value in required.items()):
        raise cts.CampaignError(
            "physical campaign requires a matching passing Simulator run"
        )
    artifacts = path.parent / "artifacts"
    selected = artifacts / "selected.txt"
    qpa = artifacts / "results.qpa"
    if not selected.is_file() or not qpa.is_file():
        raise cts.CampaignError("matching Simulator artifacts are incomplete")
    if cts.parse_case_list(selected.read_text(encoding="utf-8")) != [
        campaign.exact_case
    ]:
        raise cts.CampaignError("matching Simulator evidence selected another case")
    if hashlib.sha256(qpa.read_bytes()).hexdigest() != metadata.get("qpa_sha256"):
        raise cts.CampaignError("matching Simulator QPA identity changed")
    return metadata


def install_and_provision(device: str, app: Path, rootfs: Path) -> None:
    subprocess.run(
        [
            "xcrun",
            "devicectl",
            "device",
            "install",
            "app",
            "--device",
            device,
            str(app),
            "--quiet",
            "--timeout",
            "300",
        ],
        check=True,
    )
    subprocess.run(
        [
            "xcrun",
            "devicectl",
            "device",
            "copy",
            "to",
            "--device",
            device,
            "--source",
            str(rootfs),
            "--destination",
            "Documents/rootfs.erofs",
            "--domain-type",
            "appDataContainer",
            "--domain-identifier",
            common.BUNDLE_ID,
            "--quiet",
            "--timeout",
            "1800",
        ],
        check=True,
    )


def stop_console_process(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    process.send_signal(signal.SIGINT)
    try:
        process.wait(timeout=15)
    except subprocess.TimeoutExpired:
        process.terminate()
        process.wait(timeout=5)


def parse_arguments(arguments: Sequence[str] | None = None) -> argparse.Namespace:
    identity = cts.load_identity()
    campaigns = cts.load_apple_campaigns(identity)
    parser = argparse.ArgumentParser(
        description="run a Simulator-proven CTS campaign on a connected iPad"
    )
    parser.add_argument("--campaign", required=True, choices=sorted(campaigns))
    parser.add_argument("--page-profile", choices=("16k", "4k"), default="16k")
    parser.add_argument("--device", required=True)
    parser.add_argument("--simulator-run", type=Path, required=True)
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
    simulator_run = options.simulator_run.resolve()
    simulator_metadata = validate_simulator_evidence(
        simulator_run, campaign, options.page_profile
    )
    launch_environment = {
        "DEVICECTL_CHILD_KERNARIUM_CTS_CAMPAIGN": campaign.identifier,
        "DEVICECTL_CHILD_KERNARIUM_KERNEL_PAGE_SIZE": str(guest_page_size),
        "DEVICECTL_CHILD_KERNARIUM_ROOTFS": "rootfs.erofs",
        "DEVICECTL_CHILD_KERNARIUM_EXECUTION_ENGINE": "gadgets",
    }
    if options.dry_run:
        print(
            json.dumps(
                {
                    "campaign": campaign.identifier,
                    "device": options.device,
                    "page_profile": options.page_profile,
                    "simulator_run": str(simulator_run),
                    "launch_environment": launch_environment,
                },
                indent=2,
                sort_keys=True,
            )
        )
        return 0

    app = options.app.resolve()
    rootfs = options.rootfs.resolve()
    common.validate_app(app)
    common.validate_rootfs(rootfs, options.identity)
    subprocess.run(
        ["codesign", "--verify", "--deep", "--strict", str(app)], check=True
    )
    device = device_details(options.device)
    install_and_provision(options.device, app, rootfs)

    timestamp = time.strftime("%Y%m%d-%H%M%S")
    artifact_directory = options.artifact_root.resolve() / (
        f"{timestamp}-{os.getpid()}-{options.page_profile}-{campaign.identifier}"
    )
    artifact_directory.mkdir(parents=True, exist_ok=False)
    command = [
        "xcrun",
        "devicectl",
        "device",
        "process",
        "launch",
        "--device",
        options.device,
        "--console",
        "--terminate-existing",
        common.BUNDLE_ID,
    ]
    metadata: dict[str, Any] = {
        "schema_version": 1,
        "git": cts.git_state(),
        "adapter": "ios-device-kernarium",
        "backend": "angle-metal",
        "device": device,
        "app": str(app),
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
        "simulator_run": str(simulator_run),
        "simulator_qpa_sha256": simulator_metadata["qpa_sha256"],
        "command": command,
        "launch_environment": launch_environment,
    }
    cts.write_json(artifact_directory / "run.json", metadata)

    environment = os.environ.copy()
    for key in (
        "DEVICECTL_CHILD_KERNARIUM_WEBGL_AQUARIUM",
        "DEVICECTL_CHILD_KERNARIUM_GEEKBENCH",
        "DEVICECTL_CHILD_KERNARIUM_GEEKBENCH_WORKLOADS",
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
                "Connected iPad",
            )
        finally:
            stop_console_process(process)

    metadata["devicectl_return_code"] = process.returncode
    metadata["runtime_return_code"] = runtime_status
    cts.write_json(artifact_directory / "run.json", metadata)
    if runtime_status != 0:
        guest_errors = cts.GUEST_ERROR.findall(transcript)
        detail = f": {guest_errors[-1]}" if guest_errors else ""
        raise cts.CampaignError(f"iPad guest returned {runtime_status}{detail}")

    artifacts, results, totals = common.validate_transcript(
        transcript, campaign, guest_page_size
    )
    output_directory = artifact_directory / "artifacts"
    cts.write_artifacts(artifacts, output_directory)
    metadata.update(
        {
            "completed_cases": len(results),
            "status_totals": totals,
            "selected_caselist_sha256": hashlib.sha256(
                artifacts["selected.txt"]
            ).hexdigest(),
            "qpa_sha256": hashlib.sha256(artifacts["results.qpa"]).hexdigest(),
        }
    )
    cts.write_json(artifact_directory / "run.json", metadata)
    cts.validate_required_passes(results)
    print(f"Connected-iPad VK-GL-CTS completed {len(results)} cases: {totals}")
    print(f"Artifacts: {artifact_directory}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print(
            "run_ios_device_vk_gl_cts.py: interrupted; app stopped",
            file=sys.stderr,
        )
        raise SystemExit(130) from None
    except (
        cts.CampaignError,
        OSError,
        subprocess.SubprocessError,
        TimeoutError,
    ) as error:
        print(f"run_ios_device_vk_gl_cts.py: {error}", file=sys.stderr)
        raise SystemExit(1) from error
