# SPDX-License-Identifier: GPL-2.0-or-later
"""Common validation for VK-GL-CTS campaigns run through Kernarium."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import re
import subprocess
import sys
import time
from typing import Any

import run_vk_gl_cts as cts


REPO_ROOT = Path(__file__).resolve().parents[3]
BUNDLE_ID = "com.imxieyi.Kernarium"
DERIVATIVE_RECEIPT_PATH = Path(__file__).with_name(
    "vk_gl_cts_derivative_rootfs.json"
)
RUNTIME_RESULT = re.compile(r"Kernarium: runtime returned (-?[0-9]+)")
REQUIRED_SHUTDOWN_MARKERS = (
    "Darwin UML stopped: result=0 exitcode=0",
    "Kernarium: guest exited with status 0",
    "Kernarium: runtime returned 0",
)
DIRTY_DRM_SHUTDOWN = "Memory manager not clean during takedown."


def add_rootfs_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--rootfs", type=Path, default=cts.DEFAULT_ROOTFS)
    parser.add_argument(
        "--rootfs-type",
        choices=("erofs", "ext4"),
        default="erofs",
        help="authenticated lower filesystem type (default: erofs)",
    )
    parser.add_argument(
        "--rootfs-receipt",
        type=Path,
        help="required identity receipt for a derivative ext4 rootfs",
    )


def finish_rootfs_options(
    parser: argparse.ArgumentParser,
    options: argparse.Namespace,
    identity: dict[str, Any],
) -> None:
    try:
        receipt, contract = cts.select_rootfs_contract(
            identity, options.rootfs_type, options.rootfs_receipt
        )
        if receipt is not None:
            checked_receipt, checked_contract = cts.load_derivative_receipt(
                DERIVATIVE_RECEIPT_PATH, identity
            )
            if receipt != checked_receipt:
                raise cts.CampaignError(
                    "ext4 receipt does not match the checked derivative "
                    "rootfs identity"
                )
            receipt = checked_receipt
            contract = checked_contract
    except cts.CampaignError as error:
        parser.error(str(error))
    options.derivative_receipt = receipt
    options.rootfs_contract = contract


def validate_app(app: Path) -> None:
    info_path = app / "Info.plist"
    executable = app / "Kernarium"
    if not app.is_dir() or not executable.is_file() or not info_path.is_file():
        raise cts.CampaignError(f"Kernarium app is incomplete: {app}")
    try:
        with info_path.open("rb") as stream:
            info = plistlib.load(stream)
    except (OSError, plistlib.InvalidFileException) as error:
        raise cts.CampaignError(f"cannot read app identity: {error}") from error
    if info.get("CFBundleIdentifier") != BUNDLE_ID:
        raise cts.CampaignError("Kernarium app has the wrong bundle identifier")


def validate_rootfs(
    rootfs: Path,
    identity: dict[str, Any],
    contract: cts.RootfsContract | None = None,
) -> None:
    if contract is None:
        _, contract = cts.select_rootfs_contract(identity, "erofs", None)
    if contract.filesystem_type == "ext4":
        cts.validate_rootfs_file(rootfs, contract)
        return
    frozen = identity["rootfs"]
    if not rootfs.is_file() or rootfs.stat().st_size != frozen["size"]:
        raise cts.CampaignError("rootfs size does not match the frozen identity")
    subprocess.run(
        [
            sys.executable,
            "-B",
            str(REPO_ROOT / "tools/rootfs/verify.py"),
            "--image",
            str(rootfs),
            "--manifest",
            str(REPO_ROOT / "tools/rootfs/manifest.json"),
            "--quiet",
        ],
        check=True,
    )


def validate_recorded_gpu_evidence(
    run_path: Path,
    metadata: dict[str, Any],
    product: str,
) -> None:
    console_path = run_path.parent / "console.log"
    try:
        transcript = console_path.read_text(encoding="utf-8", errors="strict")
    except (OSError, UnicodeError) as error:
        raise cts.CampaignError(
            f"cannot read matching {product} console: {error}"
        ) from error
    capability = cts.parse_gpu_capability_report(transcript)
    native_display = cts.parse_native_display_final(transcript)
    if any(
        metadata.get(key) != capability[key]
        for key in ("host_contexts", "virgl_capsets")
    ) or metadata.get("native_display_final") != native_display:
        raise cts.CampaignError(
            f"matching {product} GPU evidence changed after capture"
        )


def validate_macos_evidence(
    path: Path,
    campaign: cts.AppleCampaign,
    page_profile: str,
    rootfs_contract: cts.RootfsContract,
) -> dict[str, Any]:
    try:
        metadata = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise cts.CampaignError(
            f"cannot read matching macOS evidence: {error}"
        ) from error
    required = {
        "adapter": "macos",
        "backend": "angle-metal",
        "display": "native",
        "page_profile": page_profile,
        "rootfs_type": rootfs_contract.filesystem_type,
        "rootfs_sha256": rootfs_contract.sha256,
        "engine": "gadgets",
        "api": campaign.api,
        "leaf": campaign.leaf.identifier,
        "leaf_sha256": campaign.leaf.sha256,
        "expected_cases": len(campaign.cases),
        "seed": campaign.seed,
        "emulator_return_code": 0,
        "completed_cases": len(campaign.cases),
        "status_totals": {"Pass": len(campaign.cases)},
    }
    if any(metadata.get(key) != value for key, value in required.items()):
        raise cts.CampaignError(
            "Simulator campaign requires a matching passing native macOS run"
        )
    if metadata.get("campaign") not in (None, campaign.identifier):
        raise cts.CampaignError(
            "Simulator campaign requires matching macOS campaign identity"
        )
    artifacts = path.parent / "artifacts"
    selected = artifacts / "selected.txt"
    renderer = artifacts / "renderer.txt"
    qpa = artifacts / "results.qpa"
    if not selected.is_file() or not renderer.is_file() or not qpa.is_file():
        raise cts.CampaignError("matching macOS artifacts are incomplete")
    selected_cases = cts.parse_case_list(
        selected.read_text(encoding="utf-8")
    )
    if selected_cases != list(campaign.cases):
        raise cts.CampaignError("matching macOS evidence selected other cases")
    if hashlib.sha256(selected.read_bytes()).hexdigest() != metadata.get(
        "selected_caselist_sha256"
    ):
        raise cts.CampaignError("matching macOS selected caselist changed")
    for artifact, key in (
        (renderer, "renderer_sha256"),
        (qpa, "qpa_sha256"),
    ):
        if hashlib.sha256(artifact.read_bytes()).hexdigest() != metadata.get(key):
            raise cts.CampaignError(f"matching macOS {artifact.name} changed")
    results = cts.validate_qpa_union(
        qpa.read_text(encoding="utf-8"), selected_cases
    )
    cts.validate_required_passes(results)
    if cts.status_totals(results) != metadata["status_totals"]:
        raise cts.CampaignError("matching macOS QPA totals changed")
    if (
        [row.get("requested") for row in metadata.get("host_contexts", [])]
        != [[3, 0], [3, 1], [3, 2]]
        or [row.get("id") for row in metadata.get("virgl_capsets", [])]
        != [1, 2]
        or any(
            not isinstance(row.get("decoded"), dict)
            for row in metadata.get("virgl_capsets", [])
        )
        or metadata.get("native_display_final", {}).get(
            "cpu_fallback_frames"
        )
        != 0
    ):
        raise cts.CampaignError(
            "matching macOS evidence omitted GPU capability or frame data"
        )
    validate_recorded_gpu_evidence(path, metadata, "macOS")
    return metadata


def wait_for_runtime(
    process: subprocess.Popen[bytes],
    log_path: Path,
    timeout: int,
    platform: str,
) -> tuple[str, int]:
    deadline = time.monotonic() + timeout
    next_heartbeat = time.monotonic() + 30
    while time.monotonic() < deadline:
        transcript = log_path.read_text(encoding="utf-8", errors="replace")
        matches = RUNTIME_RESULT.findall(transcript)
        if matches:
            return transcript, int(matches[-1])
        if process.poll() is not None:
            return transcript, -1
        now = time.monotonic()
        if now >= next_heartbeat:
            print(
                f"{platform} VK-GL-CTS guest is running; "
                f"console_bytes={log_path.stat().st_size}",
                flush=True,
            )
            next_heartbeat = now + 30
        time.sleep(0.5)
    raise TimeoutError(f"{platform} VK-GL-CTS guest exceeded {timeout} seconds")


def validate_clean_shutdown(transcript: str) -> None:
    if DIRTY_DRM_SHUTDOWN in transcript:
        raise cts.CampaignError(
            "Virtio-GPU memory manager was not clean during guest shutdown"
        )
    missing = [
        marker
        for marker in REQUIRED_SHUTDOWN_MARKERS
        if marker not in transcript
    ]
    if missing:
        raise cts.CampaignError(
            f"Kernarium campaign omitted clean shutdown markers: {missing!r}"
        )


def validate_transcript(
    transcript: str,
    campaign: cts.AppleCampaign,
    guest_page_size: int,
) -> tuple[
    dict[str, bytes],
    dict[str, str],
    dict[str, int],
    dict[str, Any],
    dict[str, int],
]:
    cts.validate_host_backend(transcript)
    validate_clean_shutdown(transcript)
    capability = cts.parse_gpu_capability_report(transcript)
    native_display = cts.parse_native_display_final(transcript)
    artifacts = cts.decode_artifacts(transcript)
    required = cts.SHARD_ARTIFACTS - set(artifacts)
    if required:
        raise cts.CampaignError(
            f"Kernarium campaign omitted artifacts: {sorted(required)!r}"
        )
    unexpected = set(artifacts) - cts.SHARD_ARTIFACTS
    if unexpected:
        raise cts.CampaignError(
            f"Kernarium campaign emitted unexpected artifacts: {sorted(unexpected)!r}"
        )
    renderer_text = artifacts["renderer.txt"].decode("utf-8", errors="strict")
    selected_text = artifacts["selected.txt"].decode("utf-8", errors="strict")
    qpa_text = artifacts["results.qpa"].decode("utf-8", errors="strict")
    summary_text = artifacts["summary.txt"].decode("utf-8", errors="strict")
    cts.validate_renderer_probe(renderer_text, guest_page_size)
    selected_cases = cts.parse_case_list(selected_text)
    if selected_cases != list(campaign.cases):
        raise cts.CampaignError("Kernarium campaign selected the wrong cases")
    results = cts.validate_qpa_union(qpa_text, selected_cases)
    if summary_text.strip() != "cts_rc=0":
        raise cts.CampaignError(f"CTS process failed: {summary_text.strip()}")
    return (
        artifacts,
        results,
        cts.status_totals(results),
        capability,
        native_display,
    )
