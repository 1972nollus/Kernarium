#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run and independently verify a complete signed Apple GLES matrix."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
from dataclasses import dataclass
from datetime import datetime, timezone
import fcntl
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
from typing import Any, Sequence

import kernarium_vk_gl_cts_common as common
import run_vk_gl_cts as cts


INTEGRATION_GPU = Path(__file__).resolve().parent
SIMULATOR_RUNNER = INTEGRATION_GPU / "run_ios_simulator_vk_gl_cts.py"
DEVICE_RUNNER = INTEGRATION_GPU / "run_ios_device_vk_gl_cts.py"
COMMON_RUNNER = INTEGRATION_GPU / "kernarium_vk_gl_cts_common.py"
CTS_RUNNER = INTEGRATION_GPU / "run_vk_gl_cts.py"


@dataclass(frozen=True)
class ShardEvidence:
    shard: cts.AppleMatrixShard
    run_directory: Path
    completed_cases: int
    status_totals: dict[str, int]
    selected_sha256: str
    qpa_sha256: str
    renderer_sha256: str
    native_display: dict[str, int]


@dataclass(frozen=True)
class Definition:
    platform: str
    adapter: str
    predecessor_adapter: str
    api: str
    page_profile: str
    device: str
    shards: tuple[cts.AppleMatrixShard, ...]
    configs: tuple[cts.EglConfig, ...]
    config_digest: str
    config_inventory_run: Path
    config_inventory_run_sha256: str
    predecessor_campaign: Path
    predecessor_campaign_sha256: str
    predecessor_runs: tuple[Path, ...]
    app: Path
    app_identity: dict[str, dict[str, Any]]
    rootfs: Path
    rootfs_contract: cts.RootfsContract
    rootfs_receipt_path: Path
    rootfs_receipt_sha256: str
    rootfs_receipt: dict[str, Any]
    guest_timeout: int
    drivers: dict[str, dict[str, Any]]

    @property
    def total_case_executions(self) -> int:
        return len(self.configs) * self.shards[0].leaf.case_count

    def serialized(self) -> dict[str, Any]:
        return {
            "execution": {
                "platform": self.platform,
                "adapter": self.adapter,
                "backend": "angle-metal",
                "engine": "gadgets",
                "device": self.device,
                "page_profile": self.page_profile,
                "guest_timeout": self.guest_timeout,
            },
            "api": self.api,
            "signed_matrix": {
                "manifest_sha256": self.shards[0].manifest_sha256,
                "shards": [shard.identifier for shard in self.shards],
                "leaf": {
                    "id": self.shards[0].leaf.identifier,
                    "case_count": self.shards[0].leaf.case_count,
                    "sha256": self.shards[0].leaf.sha256,
                },
                "config_sha256": self.config_digest,
                "configs": [
                    {
                        "id": config.identifier,
                        "surface_types": list(config.surface_types),
                        "selected_surface_type": config.selected_surface_type,
                    }
                    for config in self.configs
                ],
            },
            "config_inventory": {
                "run": str(self.config_inventory_run),
                "run_sha256": self.config_inventory_run_sha256,
            },
            "predecessor": {
                "adapter": self.predecessor_adapter,
                "campaign": str(self.predecessor_campaign),
                "campaign_sha256": self.predecessor_campaign_sha256,
                "runs": [str(path) for path in self.predecessor_runs],
            },
            "app": {
                "path": str(self.app),
                "files": self.app_identity,
            },
            "rootfs": {
                "path": str(self.rootfs),
                "filesystem_type": self.rootfs_contract.filesystem_type,
                "filesystem_uuid": self.rootfs_contract.filesystem_uuid,
                "size": self.rootfs_contract.size,
                "sha256": self.rootfs_contract.sha256,
                "glcts_sha256": self.rootfs_contract.glcts_sha256,
                "receipt_path": str(self.rootfs_receipt_path),
                "receipt_sha256": self.rootfs_receipt_sha256,
            },
            "drivers": self.drivers,
            "expected": {
                "egl_config_count": len(self.configs),
                "shard_count": len(self.shards),
                "unique_case_count": self.shards[0].leaf.case_count,
                "case_execution_count": self.total_case_executions,
            },
        }


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def load_json(path: Path) -> Any:
    try:
        return json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=cts.reject_duplicate_keys,
        )
    except (OSError, json.JSONDecodeError) as error:
        raise cts.CampaignError(f"cannot read JSON {path}: {error}") from error


def atomic_write_json(path: Path, value: Any) -> None:
    temporary = path.with_name(f".{path.name}.tmp-{os.getpid()}")
    temporary.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    os.replace(temporary, path)


def validate_timestamp(value: Any, label: str) -> None:
    if not isinstance(value, str):
        raise cts.CampaignError(f"{label} is not a timestamp")
    try:
        parsed = datetime.fromisoformat(value)
    except ValueError as error:
        raise cts.CampaignError(f"{label} is not a timestamp") from error
    if parsed.tzinfo is None:
        raise cts.CampaignError(f"{label} has no timezone")


def regular_file(path: Path, label: str) -> None:
    try:
        details = path.lstat()
    except OSError as error:
        raise cts.CampaignError(f"missing {label}: {path}") from error
    if stat.S_ISLNK(details.st_mode) or not stat.S_ISREG(details.st_mode):
        raise cts.CampaignError(f"{label} is not a regular file: {path}")


def file_identity(path: Path) -> dict[str, Any]:
    regular_file(path, "identity input")
    return {
        "size": path.stat().st_size,
        "sha256": cts.sha256_file(path),
    }


def app_file_identity(
    app: Path,
    page_profile: str,
    required_campaigns: Sequence[str],
) -> dict[str, dict[str, Any]]:
    common.validate_app(app)
    relative_files = [
        Path("Kernarium"),
        Path("Frameworks/EGL.framework/EGL"),
        Path("Frameworks/GLESv2.framework/GLESv2"),
    ]
    debug_library = app / "Kernarium.debug.dylib"
    if debug_library.is_file():
        relative_files.append(Path("Kernarium.debug.dylib"))
    suffix = "16K" if page_profile == "16k" else "4K"
    runtime_frameworks = sorted(
        (app / "Frameworks").glob(f"KernariumRuntime_*_{suffix}.framework")
    )
    if len(runtime_frameworks) != 1:
        raise cts.CampaignError(
            f"app does not contain one {suffix} runtime framework"
        )
    runtime = runtime_frameworks[0]
    runtime_binary = runtime / runtime.stem
    regular_file(runtime_binary, "selected app runtime")
    relative_files.append(runtime_binary.relative_to(app))
    runtime_payload = runtime_binary.read_bytes()
    missing = [
        identifier
        for identifier in required_campaigns
        if identifier.encode("ascii") not in runtime_payload
    ]
    if missing:
        raise cts.CampaignError(
            "app runtime omits signed matrix campaigns: "
            f"{missing[:3]!r}"
        )
    identities: dict[str, dict[str, Any]] = {}
    for relative in relative_files:
        path = app / relative
        identities[str(relative)] = file_identity(path)
    return identities


def safe_run_path(campaign_directory: Path, value: Any) -> Path:
    if not isinstance(value, str):
        raise cts.CampaignError("predecessor campaign run path is invalid")
    relative = Path(value)
    if relative.is_absolute():
        raise cts.CampaignError("predecessor campaign run path is absolute")
    resolved = (campaign_directory / relative).resolve()
    try:
        resolved.relative_to(campaign_directory)
    except ValueError as error:
        raise cts.CampaignError(
            "predecessor campaign run escapes its directory"
        ) from error
    return resolved / "run.json"


def predecessor_rows(document: dict[str, Any]) -> list[dict[str, Any]]:
    aggregate = document.get("aggregate")
    if (
        document.get("state") != "passed"
        or not isinstance(aggregate, dict)
        or aggregate.get("complete") is not True
        or not isinstance(aggregate.get("configs"), list)
    ):
        raise cts.CampaignError("predecessor matrix is not complete and passed")
    return aggregate["configs"]


def load_predecessor_runs(
    platform: str,
    campaign_path: Path,
    shards: tuple[cts.AppleMatrixShard, ...],
    page_profile: str,
    rootfs_contract: cts.RootfsContract,
) -> tuple[Path, ...]:
    document = load_json(campaign_path)
    if not isinstance(document, dict):
        raise cts.CampaignError("predecessor campaign is not an object")
    expected_kind = (
        "uml-vk-gl-cts-official-gles-matrix"
        if platform == "simulator"
        else "uml-vk-gl-cts-signed-apple-matrix"
    )
    if document.get("kind") != expected_kind:
        raise cts.CampaignError("predecessor campaign kind changed")
    if platform == "device":
        execution = document.get("spec", {}).get("execution", {})
        if execution.get("platform") != "simulator":
            raise cts.CampaignError(
                "device matrix predecessor is not a Simulator campaign"
            )
    rows = predecessor_rows(document)
    if len(rows) != len(shards):
        raise cts.CampaignError("predecessor config count changed")
    campaign_directory = campaign_path.parent.resolve()
    result: list[Path] = []
    for shard, row in zip(shards, rows, strict=True):
        if (
            not isinstance(row, dict)
            or row.get("index") != shard.matrix.egl_config_index
            or row.get("id") != shard.matrix.egl_config.identifier
        ):
            raise cts.CampaignError("predecessor config ordering changed")
        if platform == "simulator":
            shard_rows = row.get("shards")
            if not isinstance(shard_rows, list) or len(shard_rows) != 1:
                raise cts.CampaignError("macOS predecessor shard count changed")
            run_path = safe_run_path(campaign_directory, shard_rows[0].get("run"))
            regular_file(run_path, "macOS predecessor run")
            common.validate_macos_matrix_evidence(
                run_path, shard, page_profile, rootfs_contract
            )
        else:
            run_path = safe_run_path(campaign_directory, row.get("run"))
            regular_file(run_path, "Simulator predecessor run")
            common.validate_mobile_matrix_evidence(
                run_path,
                "ios-simulator-kernarium",
                shard,
                page_profile,
                rootfs_contract,
            )
        result.append(run_path)
    return tuple(result)


def build_definition(options: argparse.Namespace) -> Definition:
    identity = cts.load_identity()
    all_shards = cts.load_apple_matrix_shards(identity)
    shards = tuple(
        shard for shard in all_shards.values() if shard.api == options.api
    )
    configs = cts.apple_matrix_configs(all_shards, options.api)
    config_digest = cts.egl_config_digest(configs)
    receipt, contract = cts.select_rootfs_contract(
        identity, "ext4", options.rootfs_receipt
    )
    if receipt is None or contract.filesystem_type != "ext4":
        raise cts.CampaignError(
            "a signed mobile matrix requires the authenticated ext4 derivative"
        )
    rootfs = options.rootfs.resolve()
    cts.validate_rootfs_file(rootfs, contract)
    app = options.app.resolve()
    app_identity = app_file_identity(
        app,
        options.page_profile,
        [shard.identifier for shard in shards],
    )

    adapter = (
        "ios-simulator-kernarium"
        if options.platform == "simulator"
        else "ios-device-kernarium"
    )
    predecessor_adapter = (
        "macos" if options.platform == "simulator" else "ios-simulator-kernarium"
    )
    config_inventory_run = options.config_inventory_run.resolve()
    regular_file(config_inventory_run, "mobile config inventory run")
    _, inventory_configs = common.validate_mobile_config_inventory_evidence(
        config_inventory_run,
        adapter,
        options.api,
        options.page_profile,
        contract,
    )
    common.validate_matching_config_inventories(
        configs, inventory_configs, "signed matrix manifest"
    )

    predecessor_campaign = (
        options.predecessor_campaign.resolve() / "campaign.json"
    )
    regular_file(predecessor_campaign, "predecessor campaign")
    predecessor_runs = load_predecessor_runs(
        options.platform,
        predecessor_campaign,
        shards,
        options.page_profile,
        contract,
    )
    rootfs_receipt_path = options.rootfs_receipt.resolve()
    runner = SIMULATOR_RUNNER if options.platform == "simulator" else DEVICE_RUNNER
    drivers = {
        "adapter": {"path": str(runner), **file_identity(runner)},
        "common": {"path": str(COMMON_RUNNER), **file_identity(COMMON_RUNNER)},
        "cts": {"path": str(CTS_RUNNER), **file_identity(CTS_RUNNER)},
        "orchestrator": {
            "path": str(Path(__file__).resolve()),
            **file_identity(Path(__file__).resolve()),
        },
    }
    return Definition(
        platform=options.platform,
        adapter=adapter,
        predecessor_adapter=predecessor_adapter,
        api=options.api,
        page_profile=options.page_profile,
        device=options.device,
        shards=shards,
        configs=configs,
        config_digest=config_digest,
        config_inventory_run=config_inventory_run,
        config_inventory_run_sha256=cts.sha256_file(config_inventory_run),
        predecessor_campaign=predecessor_campaign,
        predecessor_campaign_sha256=cts.sha256_file(predecessor_campaign),
        predecessor_runs=predecessor_runs,
        app=app,
        app_identity=app_identity,
        rootfs=rootfs,
        rootfs_contract=contract,
        rootfs_receipt_path=rootfs_receipt_path,
        rootfs_receipt_sha256=cts.sha256_file(rootfs_receipt_path),
        rootfs_receipt=receipt,
        guest_timeout=shards[0].shard_timeout + 600,
        drivers=drivers,
    )


def attempt_directory(campaign: Path, shard: cts.AppleMatrixShard) -> Path:
    return campaign / "shards" / shard.identifier / "attempt"


def child_command(
    definition: Definition,
    shard: cts.AppleMatrixShard,
    predecessor_run: Path,
    artifact_root: Path,
) -> list[str]:
    runner = SIMULATOR_RUNNER if definition.platform == "simulator" else DEVICE_RUNNER
    predecessor_option = (
        "--macos-run" if definition.platform == "simulator" else "--simulator-run"
    )
    return [
        sys.executable,
        "-B",
        str(runner),
        "--matrix-shard",
        shard.identifier,
        "--page-profile",
        definition.page_profile,
        "--device",
        definition.device,
        predecessor_option,
        str(predecessor_run),
        "--config-inventory-run",
        str(definition.config_inventory_run),
        "--app",
        str(definition.app),
        "--rootfs",
        str(definition.rootfs),
        "--rootfs-type",
        "ext4",
        "--rootfs-receipt",
        str(definition.rootfs_receipt_path),
        "--artifact-root",
        str(artifact_root),
        "--guest-timeout",
        str(definition.guest_timeout),
        "--qualification",
    ]


def adapter_run_directories(path: Path) -> list[Path]:
    if not path.is_dir() or path.is_symlink():
        return []
    entries = list(path.iterdir())
    if any(not entry.is_dir() or entry.is_symlink() for entry in entries):
        raise cts.CampaignError("adapter output root has an invalid entry")
    return sorted(entries)


def validate_run(
    definition: Definition,
    shard: cts.AppleMatrixShard,
    predecessor_run: Path,
    run_directory: Path,
) -> ShardEvidence:
    run_path = run_directory / "run.json"
    metadata = common.validate_mobile_matrix_evidence(
        run_path,
        definition.adapter,
        shard,
        definition.page_profile,
        definition.rootfs_contract,
    )
    predecessor_key = (
        "macos_run" if definition.platform == "simulator" else "simulator_run"
    )
    if (
        metadata.get(predecessor_key) != str(predecessor_run)
        or metadata.get("config_inventory_run")
        != str(definition.config_inventory_run)
        or metadata.get("rootfs_receipt") != definition.rootfs_receipt
    ):
        raise cts.CampaignError("mobile matrix input evidence changed")
    return ShardEvidence(
        shard=shard,
        run_directory=run_directory,
        completed_cases=metadata["completed_cases"],
        status_totals=metadata["status_totals"],
        selected_sha256=metadata["selected_caselist_sha256"],
        qpa_sha256=metadata["qpa_sha256"],
        renderer_sha256=metadata["renderer_sha256"],
        native_display=metadata["native_display_final"],
    )


def validate_attempt(
    campaign: Path,
    definition: Definition,
    shard: cts.AppleMatrixShard,
    predecessor_run: Path,
) -> ShardEvidence:
    attempt = attempt_directory(campaign, shard)
    if attempt.is_symlink() or not attempt.is_dir():
        raise cts.CampaignError("matrix shard attempt is not a directory")
    attempt_path = attempt / "attempt.json"
    log_path = attempt / "runner.log"
    regular_file(attempt_path, "matrix attempt metadata")
    regular_file(log_path, "matrix attempt log")
    metadata = load_json(attempt_path)
    expected_keys = {
        "schema_version",
        "shard",
        "started_at",
        "finished_at",
        "command",
        "return_code",
        "runner_log_sha256",
        "adapter_run",
    }
    artifact_root = attempt / "adapter-output"
    expected_command = child_command(
        definition, shard, predecessor_run, artifact_root
    )
    if (
        not isinstance(metadata, dict)
        or set(metadata) != expected_keys
        or metadata.get("schema_version") != 1
        or metadata.get("shard") != shard.identifier
        or metadata.get("command") != expected_command
        or metadata.get("return_code") != 0
        or metadata.get("runner_log_sha256") != cts.sha256_file(log_path)
    ):
        raise cts.CampaignError("matrix attempt did not complete successfully")
    validate_timestamp(metadata["started_at"], "matrix attempt start")
    validate_timestamp(metadata["finished_at"], "matrix attempt finish")
    runs = adapter_run_directories(artifact_root)
    if len(runs) != 1:
        raise cts.CampaignError("matrix attempt did not produce exactly one run")
    expected_relative = str(runs[0].relative_to(campaign))
    if metadata.get("adapter_run") != expected_relative:
        raise cts.CampaignError("matrix attempt names another adapter run")
    return validate_run(
        definition, shard, predecessor_run, runs[0]
    )


def run_shard(
    campaign: Path,
    definition: Definition,
    shard: cts.AppleMatrixShard,
    predecessor_run: Path,
) -> ShardEvidence:
    attempt = attempt_directory(campaign, shard)
    attempt.parent.mkdir(parents=True, exist_ok=True)
    attempt.mkdir(exist_ok=False)
    artifact_root = attempt / "adapter-output"
    artifact_root.mkdir()
    log_path = attempt / "runner.log"
    command = child_command(definition, shard, predecessor_run, artifact_root)
    metadata: dict[str, Any] = {
        "schema_version": 1,
        "shard": shard.identifier,
        "started_at": utc_now(),
        "finished_at": None,
        "command": command,
        "return_code": None,
        "runner_log_sha256": None,
        "adapter_run": None,
    }
    atomic_write_json(attempt / "attempt.json", metadata)
    with log_path.open("wb") as log:
        result = subprocess.run(
            command,
            cwd=cts.REPO_ROOT,
            stdout=log,
            stderr=subprocess.STDOUT,
            check=False,
        )
    runs = adapter_run_directories(artifact_root)
    metadata.update(
        {
            "finished_at": utc_now(),
            "return_code": result.returncode,
            "runner_log_sha256": cts.sha256_file(log_path),
            "adapter_run": (
                str(runs[0].relative_to(campaign))
                if len(runs) == 1
                else None
            ),
        }
    )
    atomic_write_json(attempt / "attempt.json", metadata)
    if result.returncode != 0:
        raise cts.CampaignError(
            f"adapter exited with status {result.returncode}; see {log_path}"
        )
    return validate_attempt(campaign, definition, shard, predecessor_run)


def scan_campaign(
    campaign: Path,
    definition: Definition,
) -> tuple[
    dict[str, ShardEvidence],
    dict[str, str],
    list[cts.AppleMatrixShard],
]:
    completed: dict[str, ShardEvidence] = {}
    failures: dict[str, str] = {}
    pending: list[cts.AppleMatrixShard] = []
    for shard, predecessor in zip(
        definition.shards, definition.predecessor_runs, strict=True
    ):
        attempt = attempt_directory(campaign, shard)
        if not attempt.exists():
            pending.append(shard)
            continue
        try:
            completed[shard.identifier] = validate_attempt(
                campaign, definition, shard, predecessor
            )
        except (cts.CampaignError, OSError, UnicodeError) as error:
            failures[shard.identifier] = str(error)
    return completed, failures, pending


def aggregate_matrix(
    campaign: Path,
    definition: Definition,
    completed: dict[str, ShardEvidence],
) -> dict[str, Any]:
    if len(completed) != len(definition.shards):
        raise cts.CampaignError("matrix aggregate is incomplete")
    totals: dict[str, int] = {}
    configs: list[dict[str, Any]] = []
    for shard in definition.shards:
        evidence = completed[shard.identifier]
        for status, count in evidence.status_totals.items():
            totals[status] = totals.get(status, 0) + count
        configs.append(
            {
                "index": shard.matrix.egl_config_index,
                "id": shard.matrix.egl_config.identifier,
                "surface_type": shard.matrix.egl_config.selected_surface_type,
                "run": str(evidence.run_directory.relative_to(campaign)),
                "completed_cases": evidence.completed_cases,
                "status_totals": evidence.status_totals,
                "selected_sha256": evidence.selected_sha256,
                "qpa_sha256": evidence.qpa_sha256,
                "renderer_sha256": evidence.renderer_sha256,
                "native_display_final": evidence.native_display,
            }
        )
    if sum(totals.values()) != definition.total_case_executions:
        raise cts.CampaignError("matrix execution count is incomplete")
    return {
        "complete": True,
        "unwaived_runner_success": True,
        "egl_config_count": len(definition.configs),
        "shard_count": len(definition.shards),
        "unique_case_count": definition.shards[0].leaf.case_count,
        "case_execution_count": definition.total_case_executions,
        "leaf_sha256": definition.shards[0].leaf.sha256,
        "status_totals": dict(sorted(totals.items())),
        "configs": configs,
    }


def campaign_document(
    existing: dict[str, Any] | None,
    definition: Definition,
    state: str,
    completed: dict[str, ShardEvidence],
    failures: dict[str, str],
    pending: Sequence[cts.AppleMatrixShard],
    aggregate: dict[str, Any] | None = None,
) -> dict[str, Any]:
    return {
        "schema_version": 1,
        "kind": "uml-vk-gl-cts-signed-apple-matrix",
        "created_at": existing["created_at"] if existing else utc_now(),
        "updated_at": utc_now(),
        "state": state,
        "spec": definition.serialized(),
        "progress": {
            "completed_shards": len(completed),
            "failed_shards": len(failures),
            "pending_shards": len(pending),
            "total_shards": len(definition.shards),
        },
        "failures": [
            {"shard": identifier, "error": failures[identifier]}
            for identifier in sorted(failures)
        ],
        "aggregate": aggregate,
    }


def update_campaign(
    path: Path,
    existing: dict[str, Any] | None,
    definition: Definition,
    state: str,
    completed: dict[str, ShardEvidence],
    failures: dict[str, str],
    pending: Sequence[cts.AppleMatrixShard],
    aggregate: dict[str, Any] | None = None,
) -> dict[str, Any]:
    value = campaign_document(
        existing,
        definition,
        state,
        completed,
        failures,
        pending,
        aggregate,
    )
    atomic_write_json(path, value)
    return value


def parse_arguments(arguments: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="run one complete fixed signed Apple GLES matrix"
    )
    parser.add_argument("--platform", choices=("simulator", "device"), required=True)
    parser.add_argument("--api", choices=cts.BASELINE_GLES_APIS, required=True)
    parser.add_argument("--page-profile", choices=("16k", "4k"), default="16k")
    parser.add_argument("--device", required=True)
    parser.add_argument("--config-inventory-run", type=Path, required=True)
    parser.add_argument("--predecessor-campaign", type=Path, required=True)
    parser.add_argument("--campaign-dir", type=Path, required=True)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--rootfs", type=Path, required=True)
    parser.add_argument("--rootfs-receipt", type=Path, required=True)
    parser.add_argument("--keep-going", action="store_true")
    parser.add_argument("--verify-only", action="store_true")
    return parser.parse_args(arguments)


def main(arguments: Sequence[str] | None = None) -> int:
    options = parse_arguments(arguments)
    definition = build_definition(options)
    campaign = options.campaign_dir.resolve()
    campaign_path = campaign / "campaign.json"
    if options.verify_only and not campaign.is_dir():
        raise cts.CampaignError(f"campaign directory does not exist: {campaign}")
    campaign.mkdir(parents=True, exist_ok=True)
    lock_path = campaign / ".campaign.lock"
    lock_flags = os.O_RDWR if options.verify_only else os.O_RDWR | os.O_CREAT
    try:
        descriptor = os.open(lock_path, lock_flags, 0o600)
    except OSError as error:
        raise cts.CampaignError(
            f"cannot open campaign lock {lock_path}: {error}"
        ) from error
    with os.fdopen(descriptor, "r+") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise cts.CampaignError(
                "another process is already running this mobile matrix"
            ) from error
        existing = None
        if campaign_path.exists():
            existing = load_json(campaign_path)
            if (
                not isinstance(existing, dict)
                or existing.get("schema_version") != 1
                or existing.get("kind")
                != "uml-vk-gl-cts-signed-apple-matrix"
                or existing.get("spec") != definition.serialized()
            ):
                raise cts.CampaignError(
                    "campaign directory belongs to another immutable spec"
                )
            validate_timestamp(existing.get("created_at"), "campaign creation")
        elif options.verify_only:
            raise cts.CampaignError("campaign metadata does not exist")
        else:
            existing = update_campaign(
                campaign_path,
                None,
                definition,
                "running",
                {},
                {},
                definition.shards,
            )

        completed, failures, pending = scan_campaign(campaign, definition)
        if options.verify_only:
            if failures:
                first = sorted(failures)[0]
                raise cts.CampaignError(
                    f"campaign contains failed shard {first}: {failures[first]}"
                )
            aggregate = aggregate_matrix(campaign, definition, completed)
            print(
                f"Verified {aggregate['case_execution_count']} {definition.api} "
                f"case executions in {aggregate['shard_count']} signed "
                f"{definition.platform} shards"
            )
            return 0
        if failures and not options.keep_going:
            update_campaign(
                campaign_path,
                existing,
                definition,
                "failed",
                completed,
                failures,
                pending,
            )
            first = sorted(failures)[0]
            raise cts.CampaignError(
                f"campaign contains failed shard {first}: {failures[first]}"
            )
        predecessor_by_id = {
            shard.identifier: predecessor
            for shard, predecessor in zip(
                definition.shards,
                definition.predecessor_runs,
                strict=True,
            )
        }
        remaining = list(pending)
        while remaining and (options.keep_going or not failures):
            shard = remaining.pop(0)
            print(
                f"Starting {shard.identifier} "
                f"({len(completed) + len(failures) + 1}/"
                f"{len(definition.shards)})",
                flush=True,
            )
            try:
                completed[shard.identifier] = run_shard(
                    campaign,
                    definition,
                    shard,
                    predecessor_by_id[shard.identifier],
                )
                print(f"Completed {shard.identifier}", flush=True)
            except (cts.CampaignError, OSError, subprocess.SubprocessError) as error:
                failures[shard.identifier] = str(error)
                print(
                    f"Failed {shard.identifier}: {error}",
                    file=sys.stderr,
                    flush=True,
                )
            existing = update_campaign(
                campaign_path,
                existing,
                definition,
                "failed" if failures else "running",
                completed,
                failures,
                remaining,
            )
        if failures:
            raise cts.CampaignError(
                f"mobile matrix failed {len(failures)} immutable shard(s)"
            )
        aggregate = aggregate_matrix(campaign, definition, completed)
        update_campaign(
            campaign_path,
            existing,
            definition,
            "passed",
            completed,
            {},
            [],
            aggregate,
        )
        print(
            f"Signed {definition.platform} VK-GL-CTS {definition.api} matrix "
            f"completed {aggregate['case_execution_count']} executions: "
            f"{aggregate['status_totals']}"
        )
        print(f"Campaign: {campaign}")
        return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print(
            "run_ios_vk_gl_cts_matrix.py: interrupted; the active attempt is "
            "permanently retained",
            file=sys.stderr,
        )
        raise SystemExit(130) from None
    except (cts.CampaignError, OSError, subprocess.SubprocessError) as error:
        print(f"run_ios_vk_gl_cts_matrix.py: {error}", file=sys.stderr)
        raise SystemExit(1) from error
