# SPDX-License-Identifier: GPL-2.0-or-later
"""Common validation for VK-GL-CTS campaigns run through Kernarium."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
from dataclasses import replace
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
SIMULATOR_GUEST_MEMORY_MIB = 4096
DEVICE_DEFAULT_GUEST_MEMORY_MIB = 2048
DEVICE_MINIMUM_GUEST_MEMORY_MIB = 512
DEVICE_GUEST_MEMORY_STEP_MIB = 64
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


def device_guest_memory_mib(value: str) -> int:
    try:
        parsed = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError(
            "guest memory must be an integer number of MiB"
        ) from error
    if (
        parsed < DEVICE_MINIMUM_GUEST_MEMORY_MIB
        or parsed > DEVICE_DEFAULT_GUEST_MEMORY_MIB
        or parsed % DEVICE_GUEST_MEMORY_STEP_MIB != 0
    ):
        raise argparse.ArgumentTypeError(
            "guest memory must be 512..2048 MiB in 64 MiB steps"
        )
    return parsed


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
        cts.decode_qpa(qpa.read_bytes()), selected_cases
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


def validate_macos_config_inventory_evidence(
    path: Path,
    api: str,
    page_profile: str,
    rootfs_contract: cts.RootfsContract,
) -> tuple[dict[str, Any], tuple[cts.EglConfig, ...]]:
    configs, digest = cts.load_config_inventory_evidence(
        path, api, page_profile, "vnc", rootfs_contract
    )
    try:
        metadata = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise cts.CampaignError(
            f"cannot read matching macOS config inventory: {error}"
        ) from error
    if (
        metadata.get("egl_config_sha256") != digest
        or metadata.get("engine") != "jit"
        or metadata.get("cpus") != 10
        or [row.get("requested") for row in metadata.get("host_contexts", [])]
        != [[3, 0], [3, 1], [3, 2]]
        or [row.get("id") for row in metadata.get("virgl_capsets", [])]
        != [1, 2]
    ):
        raise cts.CampaignError(
            "matching macOS config inventory omitted required evidence"
        )
    try:
        transcript = (path.parent / "console.log").read_text(
            encoding="utf-8", errors="strict"
        )
    except (OSError, UnicodeError) as error:
        raise cts.CampaignError(
            f"cannot read matching macOS config-inventory console: {error}"
        ) from error
    capability = cts.parse_gpu_capability_report(transcript)
    if any(
        metadata.get(key) != capability[key]
        for key in ("host_contexts", "virgl_capsets")
    ):
        raise cts.CampaignError(
            "matching macOS config-inventory GPU evidence changed"
        )
    return metadata, configs


def matrix_semantics(shard: cts.AppleMatrixShard) -> dict[str, Any]:
    matrix = shard.matrix
    configuration = matrix.configuration
    return {
        "mustpass_xml_path": matrix.mustpass_xml_path,
        "mustpass_xml_size": matrix.mustpass_xml_size,
        "mustpass_xml_sha256": matrix.mustpass_xml_sha256,
        "runner_oracle_path": shard.oracle.path,
        "runner_oracle_size": shard.oracle.size,
        "runner_oracle_sha256": shard.oracle.sha256,
        "successful_case_statuses": list(
            shard.oracle.successful_case_statuses
        ),
        "configuration_name": configuration.name,
        "configuration_role": (
            "first" if matrix.egl_config_index == 0 else "other"
        ),
        "xml_arguments": list(configuration.xml_arguments),
        "runner_arguments": cts.matrix_runner_arguments(
            matrix, "/run/uml-cts/selected.txt"
        ),
        "egl_config_index": matrix.egl_config_index,
        "egl_config_id": matrix.egl_config.identifier,
        "egl_surface_types": list(matrix.egl_config.surface_types),
        "egl_surface_type": matrix.egl_config.selected_surface_type,
        "config_inventory_sha256": matrix.config_inventory_sha256,
    }


def validate_matrix_semantics(
    value: Any,
    shard: cts.AppleMatrixShard,
    require_signed_manifest: bool,
) -> None:
    if not isinstance(value, dict):
        raise cts.CampaignError("matrix semantics are not an object")
    expected = matrix_semantics(shard)
    if any(value.get(key) != item for key, item in expected.items()):
        raise cts.CampaignError("matrix shard semantics changed")
    if require_signed_manifest and value.get(
        "signed_manifest_sha256"
    ) != shard.manifest_sha256:
        raise cts.CampaignError("signed matrix manifest identity changed")


def parse_matrix_artifacts(
    artifacts: dict[str, bytes],
    shard: cts.AppleMatrixShard,
    guest_page_size: int,
) -> tuple[dict[str, str], dict[str, int]]:
    required = cts.SHARD_ARTIFACTS - set(artifacts)
    if required:
        raise cts.CampaignError(
            f"matrix shard omitted artifacts: {sorted(required)!r}"
        )
    unexpected = set(artifacts) - cts.SHARD_ARTIFACTS
    if unexpected:
        raise cts.CampaignError(
            f"matrix shard emitted unexpected artifacts: {sorted(unexpected)!r}"
        )
    renderer_text = artifacts["renderer.txt"].decode(
        "utf-8", errors="strict"
    )
    selected_payload = artifacts["selected.txt"]
    selected_text = selected_payload.decode("utf-8", errors="strict")
    summary_text = artifacts["summary.txt"].decode(
        "utf-8", errors="strict"
    )
    cts.validate_renderer_probe(renderer_text, guest_page_size)
    selection = shard.selection
    if hashlib.sha256(selected_payload).hexdigest() != shard.selected_sha256:
        raise cts.CampaignError(
            "matrix selected-list identity differs from the signed fraction"
        )
    selected_cases = cts.parse_case_list(selected_text)
    if len(selected_cases) != selection.expected_count:
        raise cts.CampaignError("matrix selected-case count changed")
    results = cts.validate_qpa_union(
        cts.decode_qpa(artifacts["results.qpa"]), selected_cases
    )
    if summary_text.strip() != "cts_rc=0":
        raise cts.CampaignError(
            f"matrix CTS process failed: {summary_text.strip()}"
        )
    return results, cts.status_totals(results)


def validate_matrix_artifacts(
    artifacts: dict[str, bytes],
    shard: cts.AppleMatrixShard,
    guest_page_size: int,
) -> tuple[dict[str, str], dict[str, int]]:
    results, totals = parse_matrix_artifacts(
        artifacts, shard, guest_page_size
    )
    cts.validate_official_runner_results(
        results, shard.oracle.successful_case_statuses
    )
    return results, totals


def read_matrix_artifacts(path: Path) -> dict[str, bytes]:
    if path.is_symlink() or not path.is_dir():
        raise cts.CampaignError("matching matrix artifact directory is invalid")
    entries = {entry.name: entry for entry in path.iterdir()}
    if set(entries) != set(cts.SHARD_ARTIFACTS):
        raise cts.CampaignError("matching matrix artifact set changed")
    if any(entry.is_symlink() or not entry.is_file() for entry in entries.values()):
        raise cts.CampaignError("matching matrix artifact is not a regular file")
    return {name: entry.read_bytes() for name, entry in entries.items()}


def validate_macos_matrix_evidence(
    path: Path,
    shard: cts.AppleMatrixShard,
    page_profile: str,
    rootfs_contract: cts.RootfsContract,
) -> dict[str, Any]:
    try:
        metadata = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise cts.CampaignError(
            f"cannot read matching macOS matrix evidence: {error}"
        ) from error
    required = {
        "schema_version": 1,
        "adapter": "macos",
        "backend": "angle-metal",
        "display": "vnc",
        "page_profile": page_profile,
        "rootfs_type": rootfs_contract.filesystem_type,
        "rootfs_sha256": rootfs_contract.sha256,
        "engine": "jit",
        "cpus": 10,
        "memory": "8G",
        "surface": [1280, 720],
        "context": "egl",
        "mode": "mustpass-matrix-shard",
        "qualification": True,
        "api": shard.api,
        "leaf": shard.leaf.identifier,
        "leaf_sha256": shard.leaf.sha256,
        "fraction": [
            shard.selection.fraction_index,
            shard.selection.fraction_count,
        ],
        "exact_cases": [],
        "expected_cases": shard.selection.expected_count,
        "seed": shard.seed,
        "emulator_return_code": 0,
    }
    if any(metadata.get(key) != value for key, value in required.items()):
        raise cts.CampaignError(
            "Simulator matrix requires the exact passing macOS shard"
        )
    validate_matrix_semantics(metadata.get("matrix"), shard, False)
    artifacts = read_matrix_artifacts(path.parent / "artifacts")
    results, totals = validate_matrix_artifacts(
        artifacts,
        shard,
        16384 if page_profile == "16k" else 4096,
    )
    hashes = {
        "selected_caselist_sha256": hashlib.sha256(
            artifacts["selected.txt"]
        ).hexdigest(),
        "renderer_sha256": hashlib.sha256(
            artifacts["renderer.txt"]
        ).hexdigest(),
        "qpa_sha256": hashlib.sha256(
            artifacts["results.qpa"]
        ).hexdigest(),
    }
    if (
        metadata.get("completed_cases") != len(results)
        or metadata.get("status_totals") != totals
        or any(metadata.get(key) != value for key, value in hashes.items())
    ):
        raise cts.CampaignError("matching macOS matrix artifacts changed")
    try:
        transcript = (path.parent / "console.log").read_text(
            encoding="utf-8", errors="strict"
        )
    except (OSError, UnicodeError) as error:
        raise cts.CampaignError(
            f"cannot read matching macOS matrix console: {error}"
        ) from error
    cts.validate_host_backend(transcript)
    cts.validate_guest_shutdown(transcript)
    capability = cts.parse_gpu_capability_report(transcript)
    if any(
        metadata.get(key) != capability[key]
        for key in ("host_contexts", "virgl_capsets")
    ):
        raise cts.CampaignError(
            "matching macOS matrix GPU evidence changed"
        )
    return metadata


def validate_macos_full_matrix_evidence(
    path: Path,
    shard: cts.AppleMatrixShard,
    page_profile: str,
    rootfs_contract: cts.RootfsContract,
) -> dict[str, Any]:
    """Validate a complete config run as predecessor for a mobile fraction."""
    full_selection = cts.Selection(shard.leaf, (), 0, 1)
    full_shard = replace(
        shard,
        matrix=replace(shard.matrix, selection=full_selection),
        selected_sha256=shard.leaf.sha256,
    )
    return validate_macos_matrix_evidence(
        path, full_shard, page_profile, rootfs_contract
    )


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
    qpa_text = cts.decode_qpa(artifacts["results.qpa"])
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


def validate_matrix_transcript(
    transcript: str,
    shard: cts.AppleMatrixShard,
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
    results, totals = parse_matrix_artifacts(
        artifacts, shard, guest_page_size
    )
    return artifacts, results, totals, capability, native_display


def validate_config_inventory_transcript(
    transcript: str,
    api: str,
    guest_page_size: int,
) -> tuple[
    dict[str, bytes],
    tuple[cts.EglConfig, ...],
    dict[str, Any],
    dict[str, int],
]:
    cts.validate_host_backend(transcript)
    validate_clean_shutdown(transcript)
    capability = cts.parse_gpu_capability_report(transcript)
    native_display = cts.parse_native_display_final(transcript)
    artifacts = cts.decode_artifacts(transcript)
    required = cts.CONFIG_INVENTORY_ARTIFACTS - set(artifacts)
    if required:
        raise cts.CampaignError(
            "Kernarium config inventory omitted artifacts: "
            f"{sorted(required)!r}"
        )
    unexpected = set(artifacts) - cts.CONFIG_INVENTORY_ARTIFACTS
    if unexpected:
        raise cts.CampaignError(
            "Kernarium config inventory emitted unexpected artifacts: "
            f"{sorted(unexpected)!r}"
        )
    renderer_text = artifacts["renderer.txt"].decode(
        "utf-8", errors="strict"
    )
    summary_text = artifacts["summary.txt"].decode(
        "utf-8", errors="strict"
    )
    cts.validate_renderer_probe(renderer_text, guest_page_size)
    if summary_text.strip() != "cts_rc=0":
        raise cts.CampaignError(
            f"CTS config inventory failed: {summary_text.strip()}"
        )
    configs = cts.parse_egl_config_qpa(
        cts.decode_qpa(artifacts["results.qpa"]), api
    )
    return artifacts, configs, capability, native_display


def validate_matching_config_inventories(
    predecessor: tuple[cts.EglConfig, ...],
    current: tuple[cts.EglConfig, ...],
    predecessor_name: str,
) -> None:
    if current != predecessor:
        raise cts.CampaignError(
            f"config inventory differs from matching {predecessor_name} run"
        )


def validate_mobile_config_inventory_evidence(
    path: Path,
    adapter: str,
    api: str,
    page_profile: str,
    rootfs_contract: cts.RootfsContract,
) -> tuple[dict[str, Any], tuple[cts.EglConfig, ...]]:
    try:
        metadata = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise cts.CampaignError(
            f"cannot read matching config inventory: {error}"
        ) from error
    required = {
        "schema_version": 1,
        "adapter": adapter,
        "backend": "angle-metal",
        "mode": "config-inventory",
        "api": api,
        "page_profile": page_profile,
        "rootfs_type": rootfs_contract.filesystem_type,
        "rootfs_sha256": rootfs_contract.sha256,
        "engine": "gadgets",
        "runtime_return_code": 0,
        "completed_cases": 1,
        "status_totals": {"Pass": 1},
    }
    if any(metadata.get(key) != value for key, value in required.items()):
        raise cts.CampaignError(
            "matching mobile config-inventory evidence changed"
        )
    artifacts = path.parent / "artifacts"
    renderer = artifacts / "renderer.txt"
    qpa = artifacts / "results.qpa"
    summary = artifacts / "summary.txt"
    if not renderer.is_file() or not qpa.is_file() or not summary.is_file():
        raise cts.CampaignError(
            "matching mobile config-inventory artifacts are incomplete"
        )
    for artifact, key in (
        (renderer, "renderer_sha256"),
        (qpa, "qpa_sha256"),
    ):
        if hashlib.sha256(artifact.read_bytes()).hexdigest() != metadata.get(key):
            raise cts.CampaignError(
                f"matching mobile config-inventory {artifact.name} changed"
            )
    if summary.read_text(encoding="utf-8").strip() != "cts_rc=0":
        raise cts.CampaignError(
            "matching mobile config-inventory CTS process failed"
        )
    configs = cts.parse_egl_config_qpa(cts.decode_qpa(qpa.read_bytes()), api)
    if cts.egl_configs_from_json(metadata.get("egl_configs")) != configs:
        raise cts.CampaignError(
            "matching mobile config-inventory metadata changed"
        )
    if metadata.get("egl_config_sha256") != cts.egl_config_digest(configs):
        raise cts.CampaignError(
            "matching mobile config-inventory digest changed"
        )
    validate_recorded_gpu_evidence(path, metadata, "mobile config inventory")
    return metadata, configs


def validate_mobile_matrix_evidence(
    path: Path,
    adapter: str,
    shard: cts.AppleMatrixShard,
    page_profile: str,
    rootfs_contract: cts.RootfsContract,
) -> dict[str, Any]:
    try:
        metadata = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise cts.CampaignError(
            f"cannot read matching mobile matrix evidence: {error}"
        ) from error
    required = {
        "schema_version": 1,
        "adapter": adapter,
        "backend": "angle-metal",
        "mode": "mustpass-matrix-shard",
        "campaign": shard.identifier,
        "api": shard.api,
        "leaf": shard.leaf.identifier,
        "leaf_sha256": shard.leaf.sha256,
        "page_profile": page_profile,
        "rootfs_type": rootfs_contract.filesystem_type,
        "rootfs_sha256": rootfs_contract.sha256,
        "engine": "gadgets",
        "qualification": True,
        "fraction": [
            shard.selection.fraction_index,
            shard.selection.fraction_count,
        ],
        "expected_cases": shard.selection.expected_count,
        "seed": shard.seed,
        "runtime_return_code": 0,
    }
    if any(metadata.get(key) != value for key, value in required.items()):
        raise cts.CampaignError("matching mobile matrix identity changed")
    validate_matrix_semantics(metadata.get("matrix"), shard, True)
    artifacts = read_matrix_artifacts(path.parent / "artifacts")
    results, totals = validate_matrix_artifacts(
        artifacts,
        shard,
        16384 if page_profile == "16k" else 4096,
    )
    hashes = {
        "selected_caselist_sha256": hashlib.sha256(
            artifacts["selected.txt"]
        ).hexdigest(),
        "renderer_sha256": hashlib.sha256(
            artifacts["renderer.txt"]
        ).hexdigest(),
        "qpa_sha256": hashlib.sha256(
            artifacts["results.qpa"]
        ).hexdigest(),
    }
    if (
        metadata.get("completed_cases") != len(results)
        or metadata.get("status_totals") != totals
        or any(metadata.get(key) != value for key, value in hashes.items())
    ):
        raise cts.CampaignError("matching mobile matrix artifacts changed")
    try:
        transcript = (path.parent / "console.log").read_text(
            encoding="utf-8", errors="strict"
        )
    except (OSError, UnicodeError) as error:
        raise cts.CampaignError(
            f"cannot read matching mobile matrix console: {error}"
        ) from error
    cts.validate_host_backend(transcript)
    validate_clean_shutdown(transcript)
    validate_recorded_gpu_evidence(path, metadata, "mobile matrix")
    return metadata
