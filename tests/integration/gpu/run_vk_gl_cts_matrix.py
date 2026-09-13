#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run and verify the complete official GLES must-pass matrix on macOS.

The leaf runner owns one guest and one CTS process.  This orchestrator fixes
the qualification environment to VNC, JIT, and ten vCPUs, schedules the
cartesian product of discovered EGL configs and deterministic fractions, and
then independently reparses every retained artifact.  A campaign directory is
resumable only for shards that were never attempted; a failed or incomplete
attempt remains a campaign failure rather than being erased by a retry.
"""

from __future__ import annotations

# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
from concurrent.futures import FIRST_COMPLETED, Future, ThreadPoolExecutor, wait
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
import time
from typing import Any, Sequence


INTEGRATION_GPU = Path(__file__).resolve().parent
sys.path.insert(0, str(INTEGRATION_GPU))
import run_vk_gl_cts as cts  # noqa: E402


RUNNER_PATH = INTEGRATION_GPU / "run_vk_gl_cts.py"
DEFAULT_CAMPAIGN_ROOT = cts.DEFAULT_ARTIFACT_ROOT / "matrix"
FIXED_ENGINE = "jit"
FIXED_CPUS = 10
FIXED_DISPLAY = "vnc"
FIXED_MEMORY = "8G"
FIXED_WIDTH = 1280
FIXED_HEIGHT = 720
MAX_JOBS = 2


@dataclass(frozen=True, order=True)
class ShardKey:
    config_index: int
    fraction_index: int

    def identifier(self, fraction_count: int) -> str:
        return (
            f"cfg{self.config_index:02d}-"
            f"{self.fraction_index:03d}of{fraction_count:03d}"
        )


@dataclass(frozen=True)
class CampaignDefinition:
    api: str
    page_profile: str
    fraction_count: int
    shard_timeout: int
    guest_timeout: int
    leaf: cts.LeafIdentity
    configs: tuple[cts.EglConfig, ...]
    config_digest: str
    first_configuration: cts.MustpassConfiguration
    other_configuration: cts.MustpassConfiguration
    oracle: cts.RunnerOracle
    config_inventory_run: Path
    config_inventory_run_sha256: str
    rootfs: Path
    rootfs_contract: cts.RootfsContract
    rootfs_receipt_path: Path
    rootfs_receipt_sha256: str
    rootfs_receipt: dict[str, Any]
    emulator: Path
    emulator_size: int
    emulator_sha256: str
    git: dict[str, Any]
    driver_sha256: str
    orchestrator_sha256: str
    mustpass_xml_path: str
    mustpass_xml_size: int
    mustpass_xml_sha256: str
    cts_release: str

    @property
    def total_shards(self) -> int:
        return len(self.configs) * self.fraction_count

    @property
    def total_case_executions(self) -> int:
        return len(self.configs) * self.leaf.case_count

    def all_keys(self) -> tuple[ShardKey, ...]:
        return tuple(
            ShardKey(config_index, fraction_index)
            for config_index in range(len(self.configs))
            for fraction_index in range(self.fraction_count)
        )

    def configuration(self, config_index: int) -> cts.MustpassConfiguration:
        return (
            self.first_configuration
            if config_index == 0
            else self.other_configuration
        )

    def selection(self, key: ShardKey) -> cts.Selection:
        return cts.Selection(
            self.leaf, (), key.fraction_index, self.fraction_count
        )

    def matrix(self, key: ShardKey) -> cts.MatrixRun:
        return cts.MatrixRun(
            configuration=self.configuration(key.config_index),
            egl_config=self.configs[key.config_index],
            egl_config_index=key.config_index,
            config_inventory_sha256=self.config_digest,
            selection=self.selection(key),
            mustpass_xml_path=self.mustpass_xml_path,
            mustpass_xml_size=self.mustpass_xml_size,
            mustpass_xml_sha256=self.mustpass_xml_sha256,
        )

    def serialized(self) -> dict[str, Any]:
        def serialize_configuration(
            configuration: cts.MustpassConfiguration,
        ) -> dict[str, Any]:
            return {
                "name": configuration.name,
                "use_for_first_egl_config": (
                    configuration.use_for_first_egl_config
                ),
                "screen_rotation": configuration.screen_rotation,
                "surface_width": configuration.surface_width,
                "surface_height": configuration.surface_height,
                "base_seed": configuration.base_seed,
                "watchdog": configuration.watchdog,
                "fbo_config": configuration.fbo_config,
                "xml_arguments": list(configuration.xml_arguments),
            }

        return {
            "api": self.api,
            "page_profile": self.page_profile,
            "fraction_count": self.fraction_count,
            "shard_timeout": self.shard_timeout,
            "guest_timeout": self.guest_timeout,
            "execution": {
                "adapter": "macos",
                "backend": "zink-kosmickrisp",
                "display": FIXED_DISPLAY,
                "engine": FIXED_ENGINE,
                "cpus": FIXED_CPUS,
                "memory": FIXED_MEMORY,
                "surface": [FIXED_WIDTH, FIXED_HEIGHT],
            },
            "git": self.git,
            "emulator": {
                "path": str(self.emulator),
                "size": self.emulator_size,
                "sha256": self.emulator_sha256,
            },
            "rootfs": {
                "path": str(self.rootfs),
                "filesystem_type": self.rootfs_contract.filesystem_type,
                "filesystem_uuid": self.rootfs_contract.filesystem_uuid,
                "size": self.rootfs_contract.size,
                "sha256": self.rootfs_contract.sha256,
                "glcts_sha256": self.rootfs_contract.glcts_sha256,
                "receipt_path": str(self.rootfs_receipt_path),
                "receipt_size": self.rootfs_receipt_path.stat().st_size,
                "receipt_sha256": self.rootfs_receipt_sha256,
            },
            "cts": {
                "release": self.cts_release,
                "leaf": {
                    "id": self.leaf.identifier,
                    "path": self.leaf.path,
                    "case_count": self.leaf.case_count,
                    "sha256": self.leaf.sha256,
                },
                "mustpass_xml": {
                    "path": self.mustpass_xml_path,
                    "size": self.mustpass_xml_size,
                    "sha256": self.mustpass_xml_sha256,
                },
                "runner_oracle": {
                    "path": self.oracle.path,
                    "size": self.oracle.size,
                    "sha256": self.oracle.sha256,
                    "successful_case_statuses": list(
                        self.oracle.successful_case_statuses
                    ),
                },
                "configurations": {
                    "first": serialize_configuration(
                        self.first_configuration
                    ),
                    "other": serialize_configuration(
                        self.other_configuration
                    ),
                },
            },
            "config_inventory": {
                "run": str(self.config_inventory_run),
                "run_sha256": self.config_inventory_run_sha256,
                "config_sha256": self.config_digest,
                "configs": [
                    {
                        "id": config.identifier,
                        "surface_types": list(config.surface_types),
                        "selected_surface_type": (
                            config.selected_surface_type
                        ),
                    }
                    for config in self.configs
                ],
            },
            "drivers": {
                "leaf_runner": {
                    "path": str(RUNNER_PATH.resolve()),
                    "sha256": self.driver_sha256,
                },
                "orchestrator": {
                    "path": str(Path(__file__).resolve()),
                    "sha256": self.orchestrator_sha256,
                },
            },
            "expected": {
                "egl_config_count": len(self.configs),
                "shard_count": self.total_shards,
                "unique_case_count": self.leaf.case_count,
                "case_execution_count": self.total_case_executions,
            },
        }


@dataclass(frozen=True)
class ShardEvidence:
    key: ShardKey
    run_directory: Path
    cases: tuple[str, ...]
    results: tuple[tuple[str, str], ...]
    qpa_sha256: str
    selected_sha256: str
    console_sha256: str
    artifact_transport_sha256: str

    @property
    def totals(self) -> dict[str, int]:
        return cts.status_totals(dict(self.results))


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


def regular_file(path: Path, label: str) -> None:
    try:
        details = path.lstat()
    except OSError as error:
        raise cts.CampaignError(f"missing {label}: {path}") from error
    if stat.S_ISLNK(details.st_mode) or not stat.S_ISREG(details.st_mode):
        raise cts.CampaignError(f"{label} is not a regular file: {path}")


def validate_iso_timestamp(value: Any, label: str) -> None:
    if not isinstance(value, str):
        raise cts.CampaignError(f"{label} is not a timestamp")
    try:
        parsed = datetime.fromisoformat(value)
    except ValueError as error:
        raise cts.CampaignError(f"{label} is not a timestamp") from error
    if parsed.tzinfo is None:
        raise cts.CampaignError(f"{label} has no timezone")


def build_definition(options: argparse.Namespace) -> CampaignDefinition:
    identity = cts.load_identity()
    leaves = cts.leaf_identities(identity)
    mustpass = cts.load_gles_mustpass(identity)
    oracle = cts.load_runner_oracle(identity, mustpass)
    receipt, contract = cts.select_rootfs_contract(
        identity, "ext4", options.rootfs_receipt
    )
    if receipt is None or contract.filesystem_type != "ext4":
        raise cts.CampaignError(
            "the official matrix requires the authenticated ext4 derivative"
        )
    rootfs = options.rootfs.resolve()
    cts.validate_rootfs_file(rootfs, contract)

    emulator = options.emulator.resolve()
    regular_file(emulator, "emulator")
    if not os.access(emulator, os.X_OK):
        raise cts.CampaignError(f"emulator is not executable: {emulator}")
    emulator_payload = emulator.read_bytes()
    for marker in cts.RETIRED_BACKEND_MARKERS:
        if marker.encode() in emulator_payload:
            raise cts.CampaignError(
                f"emulator contains a retired host GL provider: {marker}"
            )
    if cts.HOST_BACKEND_MARKER.encode() not in emulator_payload:
        raise cts.CampaignError(
            "emulator does not contain the Zink/KosmicKrisp provider marker"
        )

    config_inventory_run = options.config_inventory_run.resolve()
    regular_file(config_inventory_run, "config-inventory run")
    configs, config_digest = cts.load_config_inventory_evidence(
        config_inventory_run,
        options.api,
        options.page_profile,
        FIXED_DISPLAY,
        contract,
    )
    leaf = leaves[options.api]
    if options.fraction_count > leaf.case_count:
        raise cts.CampaignError(
            "fraction count exceeds the number of selected CTS cases"
        )
    xml_identity = identity["mustpass"]["gles_xml"]
    receipt_path = options.rootfs_receipt.resolve()
    return CampaignDefinition(
        api=options.api,
        page_profile=options.page_profile,
        fraction_count=options.fraction_count,
        shard_timeout=options.shard_timeout,
        guest_timeout=options.guest_timeout,
        leaf=leaf,
        configs=configs,
        config_digest=config_digest,
        first_configuration=cts.baseline_mustpass_configuration(
            mustpass, options.api, True
        ),
        other_configuration=cts.baseline_mustpass_configuration(
            mustpass, options.api, False
        ),
        oracle=oracle,
        config_inventory_run=config_inventory_run,
        config_inventory_run_sha256=cts.sha256_file(config_inventory_run),
        rootfs=rootfs,
        rootfs_contract=contract,
        rootfs_receipt_path=receipt_path,
        rootfs_receipt_sha256=cts.sha256_file(receipt_path),
        rootfs_receipt=receipt,
        emulator=emulator,
        emulator_size=len(emulator_payload),
        emulator_sha256=hashlib.sha256(emulator_payload).hexdigest(),
        git=cts.git_state(),
        driver_sha256=cts.sha256_file(RUNNER_PATH),
        orchestrator_sha256=cts.sha256_file(Path(__file__)),
        mustpass_xml_path=xml_identity["path"],
        mustpass_xml_size=xml_identity["size"],
        mustpass_xml_sha256=xml_identity["sha256"],
        cts_release=identity["cts"]["release_label"],
    )


def expected_matrix_metadata(
    definition: CampaignDefinition, key: ShardKey
) -> dict[str, Any]:
    matrix = definition.matrix(key)
    configuration = matrix.configuration
    return {
        "mustpass_xml_path": definition.mustpass_xml_path,
        "mustpass_xml_size": definition.mustpass_xml_size,
        "mustpass_xml_sha256": definition.mustpass_xml_sha256,
        "runner_oracle_path": definition.oracle.path,
        "runner_oracle_size": definition.oracle.size,
        "runner_oracle_sha256": definition.oracle.sha256,
        "successful_case_statuses": list(
            definition.oracle.successful_case_statuses
        ),
        "configuration_name": configuration.name,
        "configuration_role": "first" if key.config_index == 0 else "other",
        "xml_arguments": list(configuration.xml_arguments),
        "runner_arguments": cts.matrix_runner_arguments(
            matrix, "/run/uml-cts/selected.txt"
        ),
        "egl_config_index": key.config_index,
        "egl_config_id": matrix.egl_config.identifier,
        "egl_surface_types": list(matrix.egl_config.surface_types),
        "egl_surface_type": matrix.egl_config.selected_surface_type,
        "config_inventory_sha256": definition.config_digest,
        "config_inventory_run": str(definition.config_inventory_run),
    }


def require_metadata(metadata: dict[str, Any], expected: dict[str, Any]) -> None:
    for key, value in expected.items():
        if metadata.get(key) != value:
            raise cts.CampaignError(f"matrix shard metadata changed {key}")


def validate_driver_command(
    command: Any,
    definition: CampaignDefinition,
    run_directory: Path,
) -> None:
    if not isinstance(command, list) or any(
        not isinstance(argument, str) for argument in command
    ):
        raise cts.CampaignError("matrix shard command is invalid")
    required = {
        str(definition.emulator),
        str(definition.rootfs),
        "ncpus=10",
        "--execution-engine=jit",
        "--gpu-display=vnc",
        "--gpu-vnc-port=0",
        f"--virtio-serial-socket={cts.artifact_socket_path(run_directory)}",
    }
    for argument in required:
        if command.count(argument) != 1:
            raise cts.CampaignError(
                f"matrix shard command changed required argument {argument}"
            )
    forbidden = {
        "--execution-engine=interpreter",
        "--execution-engine=gadgets",
        "--gpu-display=native",
    }
    if any(argument in command for argument in forbidden):
        raise cts.CampaignError("matrix shard command selected a forbidden mode")


def validate_shard_run(
    run_directory: Path,
    definition: CampaignDefinition,
    key: ShardKey,
) -> ShardEvidence:
    if run_directory.is_symlink() or not run_directory.is_dir():
        raise cts.CampaignError("matrix shard run is not a directory")
    run_path = run_directory / "run.json"
    console_path = run_directory / "console.log"
    regular_file(run_path, "matrix shard run metadata")
    regular_file(console_path, "matrix shard console")
    metadata = load_json(run_path)
    if not isinstance(metadata, dict):
        raise cts.CampaignError("matrix shard metadata is not an object")
    selection = definition.selection(key)
    configuration = definition.configuration(key.config_index)
    require_metadata(
        metadata,
        {
            "schema_version": 1,
            "adapter": "macos",
            "backend": "zink-kosmickrisp",
            "page_profile": definition.page_profile,
            "engine": FIXED_ENGINE,
            "memory": FIXED_MEMORY,
            "cpus": FIXED_CPUS,
            "display": FIXED_DISPLAY,
            "surface": [FIXED_WIDTH, FIXED_HEIGHT],
            "context": "egl",
            "mode": "mustpass-matrix-shard",
            "rootfs_type": "ext4",
            "rootfs_sha256": definition.rootfs_contract.sha256,
            "glcts_sha256": definition.rootfs_contract.glcts_sha256,
            "emulator_sha256": definition.emulator_sha256,
            "rootfs_receipt": definition.rootfs_receipt,
            "qualification": True,
            "launch_environment": {"UML_GPU_CAPABILITY_REPORT": "1"},
            "api": definition.api,
            "leaf": definition.leaf.identifier,
            "leaf_sha256": definition.leaf.sha256,
            "fraction": [key.fraction_index, definition.fraction_count],
            "exact_cases": [],
            "expected_cases": selection.expected_count,
            "seed": configuration.base_seed,
            "git": definition.git,
            "emulator_return_code": 0,
            "artifact_transport": {
                "type": "virtio-serial-unix-socket",
                "guest_device": cts.VIRTIO_SERIAL_GUEST_DEVICE,
                "port_name": cts.VIRTIO_SERIAL_PORT_NAME,
                "acknowledged_frames": True,
            },
        },
    )
    if metadata.get("matrix") != expected_matrix_metadata(definition, key):
        raise cts.CampaignError("matrix shard expansion metadata changed")
    if "native_display_final" in metadata:
        raise cts.CampaignError("VNC matrix shard recorded native presentation")
    validate_driver_command(metadata.get("command"), definition, run_directory)

    artifact_directory = run_directory / "artifacts"
    artifact_transport_path = run_directory / cts.ARTIFACT_TRANSPORT_LOG
    regular_file(
        artifact_transport_path, "matrix shard artifact transport log"
    )
    if artifact_directory.is_symlink() or not artifact_directory.is_dir():
        raise cts.CampaignError("matrix shard artifact directory is invalid")
    entries = {entry.name: entry for entry in artifact_directory.iterdir()}
    if set(entries) != set(cts.SHARD_ARTIFACTS):
        raise cts.CampaignError("matrix shard artifact set changed")
    for name, path in entries.items():
        regular_file(path, f"matrix shard artifact {name}")

    selected_payload = entries["selected.txt"].read_bytes()
    qpa_payload = entries["results.qpa"].read_bytes()
    renderer_payload = entries["renderer.txt"].read_bytes()
    summary_payload = entries["summary.txt"].read_bytes()
    try:
        selected_text = selected_payload.decode("utf-8", errors="strict")
        renderer_text = renderer_payload.decode("utf-8", errors="strict")
        summary_text = summary_payload.decode("utf-8", errors="strict")
        console_text = console_path.read_text(
            encoding="utf-8", errors="strict"
        )
    except UnicodeDecodeError as error:
        raise cts.CampaignError("matrix shard artifact is not UTF-8") from error
    if summary_text.strip() != "cts_rc=0":
        raise cts.CampaignError("matrix shard CTS process did not exit zero")
    qpa_text = cts.decode_qpa(qpa_payload)
    selected = cts.parse_case_list(selected_text)
    if len(selected) != selection.expected_count:
        raise cts.CampaignError("matrix shard selected-case count changed")
    results = cts.validate_qpa_union(qpa_text, selected)
    cts.validate_official_runner_results(
        results, definition.oracle.successful_case_statuses
    )
    totals = cts.status_totals(results)
    selected_sha256 = hashlib.sha256(selected_payload).hexdigest()
    qpa_sha256 = hashlib.sha256(qpa_payload).hexdigest()
    renderer_sha256 = hashlib.sha256(renderer_payload).hexdigest()
    require_metadata(
        metadata,
        {
            "completed_cases": len(results),
            "status_totals": totals,
            "selected_caselist_sha256": selected_sha256,
            "qpa_sha256": qpa_sha256,
            "renderer_sha256": renderer_sha256,
            "artifact_transport_sha256": cts.sha256_file(
                artifact_transport_path
            ),
        },
    )
    cts.validate_renderer_probe(
        renderer_text, 16384 if definition.page_profile == "16k" else 4096
    )
    cts.validate_host_backend(console_text)
    cts.validate_guest_shutdown(console_text)
    capability_report = cts.parse_gpu_capability_report(console_text)
    for report_key, report_value in capability_report.items():
        if metadata.get(report_key) != report_value:
            raise cts.CampaignError(
                f"matrix shard capability metadata changed {report_key}"
            )
    return ShardEvidence(
        key=key,
        run_directory=run_directory,
        cases=tuple(selected),
        results=tuple(results.items()),
        qpa_sha256=qpa_sha256,
        selected_sha256=selected_sha256,
        console_sha256=cts.sha256_file(console_path),
        artifact_transport_sha256=cts.sha256_file(artifact_transport_path),
    )


def child_command(
    definition: CampaignDefinition,
    key: ShardKey,
    artifact_root: Path,
) -> list[str]:
    return [
        sys.executable,
        str(RUNNER_PATH.resolve()),
        "--matrix",
        definition.api,
        "--config-inventory-run",
        str(definition.config_inventory_run),
        "--egl-config-index",
        str(key.config_index),
        "--fraction",
        f"{key.fraction_index},{definition.fraction_count}",
        "--page-profile",
        definition.page_profile,
        "--engine",
        FIXED_ENGINE,
        "--display",
        FIXED_DISPLAY,
        "--memory",
        FIXED_MEMORY,
        "--cpus",
        str(FIXED_CPUS),
        "--width",
        str(FIXED_WIDTH),
        "--height",
        str(FIXED_HEIGHT),
        "--shard-timeout",
        str(definition.shard_timeout),
        "--guest-timeout",
        str(definition.guest_timeout),
        "--emulator",
        str(definition.emulator),
        "--rootfs",
        str(definition.rootfs),
        "--rootfs-type",
        "ext4",
        "--rootfs-receipt",
        str(definition.rootfs_receipt_path),
        "--artifact-root",
        str(artifact_root),
        "--qualification",
    ]


def attempt_directory(campaign_directory: Path, key: ShardKey, count: int) -> Path:
    return campaign_directory / "shards" / key.identifier(count) / "attempt"


def driver_run_directories(artifact_root: Path) -> list[Path]:
    if not artifact_root.is_dir() or artifact_root.is_symlink():
        return []
    entries = list(artifact_root.iterdir())
    if any(not entry.is_dir() or entry.is_symlink() for entry in entries):
        raise cts.CampaignError("leaf runner output root has an invalid entry")
    return sorted(entries)


def validate_attempt(
    campaign_directory: Path,
    definition: CampaignDefinition,
    key: ShardKey,
) -> ShardEvidence:
    attempt = attempt_directory(
        campaign_directory, key, definition.fraction_count
    )
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
        "driver_run",
    }
    if not isinstance(metadata, dict) or set(metadata) != expected_keys:
        raise cts.CampaignError("matrix attempt metadata is invalid")
    artifact_root = attempt / "driver-output"
    expected_command = child_command(definition, key, artifact_root)
    if (
        metadata["schema_version"] != 1
        or metadata["shard"] != key.identifier(definition.fraction_count)
        or metadata["command"] != expected_command
        or metadata["return_code"] != 0
        or metadata["runner_log_sha256"] != cts.sha256_file(log_path)
    ):
        raise cts.CampaignError("matrix attempt did not complete successfully")
    validate_iso_timestamp(metadata["started_at"], "attempt start")
    validate_iso_timestamp(metadata["finished_at"], "attempt finish")
    runs = driver_run_directories(artifact_root)
    if len(runs) != 1:
        raise cts.CampaignError("matrix attempt did not produce exactly one run")
    expected_relative = str(runs[0].relative_to(campaign_directory))
    if metadata["driver_run"] != expected_relative:
        raise cts.CampaignError("matrix attempt names another leaf run")
    return validate_shard_run(runs[0], definition, key)


def run_shard(
    campaign_directory: Path,
    definition: CampaignDefinition,
    key: ShardKey,
) -> ShardEvidence:
    attempt = attempt_directory(
        campaign_directory, key, definition.fraction_count
    )
    attempt.parent.mkdir(parents=True, exist_ok=True)
    attempt.mkdir(exist_ok=False)
    artifact_root = attempt / "driver-output"
    artifact_root.mkdir()
    log_path = attempt / "runner.log"
    command = child_command(definition, key, artifact_root)
    metadata: dict[str, Any] = {
        "schema_version": 1,
        "shard": key.identifier(definition.fraction_count),
        "started_at": utc_now(),
        "finished_at": None,
        "command": command,
        "return_code": None,
        "runner_log_sha256": None,
        "driver_run": None,
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
    runs = driver_run_directories(artifact_root)
    metadata.update(
        {
            "finished_at": utc_now(),
            "return_code": result.returncode,
            "runner_log_sha256": cts.sha256_file(log_path),
            "driver_run": (
                str(runs[0].relative_to(campaign_directory))
                if len(runs) == 1
                else None
            ),
        }
    )
    atomic_write_json(attempt / "attempt.json", metadata)
    if result.returncode != 0:
        raise cts.CampaignError(
            f"leaf runner exited with status {result.returncode}; "
            f"see {log_path}"
        )
    return validate_attempt(campaign_directory, definition, key)


def scan_campaign(
    campaign_directory: Path,
    definition: CampaignDefinition,
) -> tuple[
    dict[ShardKey, ShardEvidence], dict[ShardKey, str], list[ShardKey]
]:
    completed: dict[ShardKey, ShardEvidence] = {}
    failures: dict[ShardKey, str] = {}
    pending: list[ShardKey] = []
    for key in definition.all_keys():
        attempt = attempt_directory(
            campaign_directory, key, definition.fraction_count
        )
        if not attempt.exists():
            pending.append(key)
            continue
        try:
            completed[key] = validate_attempt(
                campaign_directory, definition, key
            )
        except (cts.CampaignError, OSError, UnicodeDecodeError) as error:
            failures[key] = str(error)
    return completed, failures, pending


def reconstruct_cases(
    definition: CampaignDefinition,
    evidence: dict[ShardKey, ShardEvidence],
    config_index: int,
) -> tuple[str, ...]:
    fractions = [
        evidence[ShardKey(config_index, fraction_index)].cases
        for fraction_index in range(definition.fraction_count)
    ]
    cases: list[str] = []
    for position in range(definition.leaf.case_count):
        fraction_index = position % definition.fraction_count
        offset = position // definition.fraction_count
        try:
            cases.append(fractions[fraction_index][offset])
        except IndexError as error:
            raise cts.CampaignError(
                f"config {config_index} has an incomplete fraction union"
            ) from error
    if any(
        len(fraction)
        != cts.Selection(
            definition.leaf, (), index, definition.fraction_count
        ).expected_count
        for index, fraction in enumerate(fractions)
    ):
        raise cts.CampaignError(
            f"config {config_index} has an invalid fraction length"
        )
    if len(cases) != len(set(cases)):
        raise cts.CampaignError(
            f"config {config_index} has duplicate selected cases"
        )
    payload = "".join(f"{case}\n" for case in cases).encode("utf-8")
    if hashlib.sha256(payload).hexdigest() != definition.leaf.sha256:
        raise cts.CampaignError(
            f"config {config_index} does not reconstruct the frozen leaf"
        )
    return tuple(cases)


def aggregate_matrix(
    campaign_directory: Path,
    definition: CampaignDefinition,
    evidence: dict[ShardKey, ShardEvidence],
) -> dict[str, Any]:
    expected = set(definition.all_keys())
    actual = set(evidence)
    if actual != expected:
        missing = sorted(expected - actual)
        unexpected = sorted(actual - expected)
        raise cts.CampaignError(
            "matrix evidence is incomplete: "
            f"missing={missing[:3]!r}, unexpected={unexpected[:3]!r}"
        )
    canonical: tuple[str, ...] | None = None
    aggregate_totals: dict[str, int] = {}
    per_config: list[dict[str, Any]] = []
    for config_index, config in enumerate(definition.configs):
        cases = reconstruct_cases(definition, evidence, config_index)
        if canonical is None:
            canonical = cases
        elif cases != canonical:
            raise cts.CampaignError(
                f"config {config_index} selected a different leaf order"
            )
        config_totals: dict[str, int] = {}
        shard_rows: list[dict[str, Any]] = []
        for fraction_index in range(definition.fraction_count):
            key = ShardKey(config_index, fraction_index)
            shard = evidence[key]
            for status, count in shard.totals.items():
                config_totals[status] = config_totals.get(status, 0) + count
                aggregate_totals[status] = (
                    aggregate_totals.get(status, 0) + count
                )
            shard_rows.append(
                {
                    "fraction_index": fraction_index,
                    "completed_cases": len(shard.results),
                    "status_totals": shard.totals,
                    "selected_sha256": shard.selected_sha256,
                    "qpa_sha256": shard.qpa_sha256,
                    "console_sha256": shard.console_sha256,
                    "artifact_transport_sha256": (
                        shard.artifact_transport_sha256
                    ),
                    "run": str(
                        shard.run_directory.relative_to(campaign_directory)
                    ),
                }
            )
        if sum(config_totals.values()) != definition.leaf.case_count:
            raise cts.CampaignError(
                f"config {config_index} result count is incomplete"
            )
        per_config.append(
            {
                "index": config_index,
                "id": config.identifier,
                "surface_type": config.selected_surface_type,
                "completed_cases": definition.leaf.case_count,
                "status_totals": dict(sorted(config_totals.items())),
                "shards": shard_rows,
            }
        )
    if sum(aggregate_totals.values()) != definition.total_case_executions:
        raise cts.CampaignError("matrix aggregate execution count is incomplete")
    return {
        "complete": True,
        "unwaived_runner_success": True,
        "egl_config_count": len(definition.configs),
        "shard_count": definition.total_shards,
        "unique_case_count": definition.leaf.case_count,
        "case_execution_count": definition.total_case_executions,
        "leaf_sha256": definition.leaf.sha256,
        "status_totals": dict(sorted(aggregate_totals.items())),
        "configs": per_config,
    }


def campaign_document(
    existing: dict[str, Any] | None,
    definition: CampaignDefinition,
    state: str,
    completed: int,
    failed: dict[ShardKey, str],
    pending: int,
    aggregate: dict[str, Any] | None,
) -> dict[str, Any]:
    return {
        "schema_version": 1,
        "kind": "uml-vk-gl-cts-official-gles-matrix",
        "created_at": existing["created_at"] if existing else utc_now(),
        "updated_at": utc_now(),
        "state": state,
        "spec": definition.serialized(),
        "progress": {
            "completed_shards": completed,
            "failed_shards": len(failed),
            "pending_shards": pending,
            "total_shards": definition.total_shards,
        },
        "failures": [
            {
                "shard": key.identifier(definition.fraction_count),
                "error": failed[key],
            }
            for key in sorted(failed)
        ],
        "aggregate": aggregate,
    }


def load_existing_campaign(
    path: Path, definition: CampaignDefinition
) -> dict[str, Any] | None:
    if not path.exists():
        return None
    value = load_json(path)
    if (
        not isinstance(value, dict)
        or value.get("schema_version") != 1
        or value.get("kind") != "uml-vk-gl-cts-official-gles-matrix"
        or value.get("spec") != definition.serialized()
    ):
        raise cts.CampaignError(
            "campaign directory belongs to a different immutable matrix spec"
        )
    validate_iso_timestamp(value.get("created_at"), "campaign creation")
    return value


def update_campaign(
    path: Path,
    existing: dict[str, Any] | None,
    definition: CampaignDefinition,
    state: str,
    completed: dict[ShardKey, ShardEvidence],
    failures: dict[ShardKey, str],
    pending: Sequence[ShardKey],
    aggregate: dict[str, Any] | None = None,
) -> dict[str, Any]:
    value = campaign_document(
        existing,
        definition,
        state,
        len(completed),
        failures,
        len(pending),
        aggregate,
    )
    atomic_write_json(path, value)
    return value


def run_pending_shards(
    campaign_directory: Path,
    campaign_path: Path,
    campaign: dict[str, Any],
    definition: CampaignDefinition,
    completed: dict[ShardKey, ShardEvidence],
    failures: dict[ShardKey, str],
    pending: list[ShardKey],
    jobs: int,
    keep_going: bool,
) -> tuple[
    dict[ShardKey, ShardEvidence], dict[ShardKey, str], list[ShardKey]
]:
    remaining = list(pending)
    running: dict[Future[ShardEvidence], ShardKey] = {}
    started = time.monotonic()
    with ThreadPoolExecutor(max_workers=jobs) as executor:
        while remaining or running:
            while (
                remaining
                and len(running) < jobs
                and (keep_going or not failures)
            ):
                key = remaining.pop(0)
                print(
                    f"Starting {key.identifier(definition.fraction_count)} "
                    f"({len(completed) + len(failures) + len(running) + 1}/"
                    f"{definition.total_shards})",
                    flush=True,
                )
                future = executor.submit(
                    run_shard, campaign_directory, definition, key
                )
                running[future] = key
            if not running:
                break
            done, _ = wait(
                set(running), timeout=30, return_when=FIRST_COMPLETED
            )
            if not done:
                identifiers = ",".join(
                    key.identifier(definition.fraction_count)
                    for key in sorted(running.values())
                )
                print(
                    "Matrix campaign is running; "
                    f"elapsed={int(time.monotonic() - started)}s "
                    f"complete={len(completed)}/{definition.total_shards} "
                    f"active={identifiers}",
                    flush=True,
                )
                continue
            for future in done:
                key = running.pop(future)
                try:
                    completed[key] = future.result()
                    print(
                        f"Completed {key.identifier(definition.fraction_count)}",
                        flush=True,
                    )
                except (cts.CampaignError, OSError, subprocess.SubprocessError) as error:
                    failures[key] = str(error)
                    print(
                        f"Failed {key.identifier(definition.fraction_count)}: "
                        f"{error}",
                        file=sys.stderr,
                        flush=True,
                    )
                campaign = update_campaign(
                    campaign_path,
                    campaign,
                    definition,
                    "failed" if failures else "running",
                    completed,
                    failures,
                    remaining + list(running.values()),
                )
    return completed, failures, remaining


def positive_integer(value: str) -> int:
    return cts.positive_integer(value)


def parse_arguments(arguments: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="run and verify one complete official GLES matrix"
    )
    parser.add_argument("--api", choices=cts.BASELINE_GLES_APIS, required=True)
    parser.add_argument("--config-inventory-run", type=Path, required=True)
    parser.add_argument("--campaign-dir", type=Path, required=True)
    parser.add_argument("--page-profile", choices=("16k", "4k"), default="16k")
    parser.add_argument("--fraction-count", type=positive_integer, default=1)
    parser.add_argument("--jobs", type=positive_integer, default=1)
    parser.add_argument("--shard-timeout", type=positive_integer, default=7200)
    parser.add_argument("--guest-timeout", type=positive_integer)
    parser.add_argument("--emulator", type=Path)
    parser.add_argument("--rootfs", type=Path, required=True)
    parser.add_argument("--rootfs-receipt", type=Path, required=True)
    parser.add_argument("--keep-going", action="store_true")
    parser.add_argument("--verify-only", action="store_true")
    options = parser.parse_args(arguments)
    if options.jobs > MAX_JOBS:
        parser.error(
            f"--jobs may not exceed {MAX_JOBS}; every guest owns ten vCPUs"
        )
    if options.guest_timeout is None:
        options.guest_timeout = options.shard_timeout + 600
    if options.guest_timeout <= options.shard_timeout:
        parser.error("--guest-timeout must exceed --shard-timeout")
    if options.emulator is None:
        options.emulator = (
            cts.DEFAULT_EMULATOR
            if options.page_profile == "16k"
            else cts.REPO_ROOT
            / ".build/presets/macos-integration-4k/bin/uml-darwin"
        )
    return options


def main(arguments: Sequence[str] | None = None) -> int:
    options = parse_arguments(arguments)
    definition = build_definition(options)
    campaign_directory = options.campaign_dir.resolve()
    campaign_path = campaign_directory / "campaign.json"
    if options.verify_only and not campaign_directory.is_dir():
        raise cts.CampaignError(
            f"campaign directory does not exist: {campaign_directory}"
        )
    campaign_directory.mkdir(parents=True, exist_ok=True)
    lock_path = campaign_directory / ".campaign.lock"
    lock_flags = os.O_RDWR if options.verify_only else os.O_RDWR | os.O_CREAT
    try:
        lock_descriptor = os.open(lock_path, lock_flags, 0o600)
    except OSError as error:
        raise cts.CampaignError(
            f"cannot open campaign lock {lock_path}: {error}"
        ) from error
    with os.fdopen(lock_descriptor, "r+") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise cts.CampaignError(
                "another process is already running this matrix campaign"
            ) from error
        campaign = load_existing_campaign(campaign_path, definition)
        if campaign is None:
            if options.verify_only:
                raise cts.CampaignError("campaign metadata does not exist")
            campaign = update_campaign(
                campaign_path,
                None,
                definition,
                "running",
                {},
                {},
                definition.all_keys(),
            )
        completed, failures, pending = scan_campaign(
            campaign_directory, definition
        )
        if options.verify_only:
            if failures:
                first = sorted(failures)[0]
                raise cts.CampaignError(
                    "campaign contains a failed attempt: "
                    f"{first.identifier(definition.fraction_count)}: "
                    f"{failures[first]}"
                )
            aggregate = aggregate_matrix(
                campaign_directory, definition, completed
            )
            print(
                f"Verified {aggregate['case_execution_count']} "
                f"{definition.api} case executions in "
                f"{aggregate['shard_count']} immutable shards"
            )
            return 0
        if failures and not options.keep_going:
            update_campaign(
                campaign_path,
                campaign,
                definition,
                "failed",
                completed,
                failures,
                pending,
            )
            first = sorted(failures)[0]
            raise cts.CampaignError(
                "campaign contains a failed attempt: "
                f"{first.identifier(definition.fraction_count)}: "
                f"{failures[first]}"
            )
        completed, failures, pending = run_pending_shards(
            campaign_directory,
            campaign_path,
            campaign,
            definition,
            completed,
            failures,
            pending,
            options.jobs,
            options.keep_going,
        )
        if failures:
            update_campaign(
                campaign_path,
                campaign,
                definition,
                "failed",
                completed,
                failures,
                pending,
            )
            raise cts.CampaignError(
                f"matrix campaign failed {len(failures)} shard(s)"
            )
        aggregate = aggregate_matrix(
            campaign_directory, definition, completed
        )
        update_campaign(
            campaign_path,
            campaign,
            definition,
            "passed",
            completed,
            {},
            [],
            aggregate,
        )
        print(
            f"VK-GL-CTS {definition.api} matrix completed "
            f"{aggregate['case_execution_count']} executions: "
            f"{aggregate['status_totals']}"
        )
        print(f"Campaign: {campaign_directory}")
        return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print(
            "run_vk_gl_cts_matrix.py: interrupted; active leaf runners stop "
            "their guests before returning",
            file=sys.stderr,
        )
        raise SystemExit(130) from None
    except (cts.CampaignError, OSError, subprocess.SubprocessError) as error:
        print(f"run_vk_gl_cts_matrix.py: {error}", file=sys.stderr)
        raise SystemExit(1) from error
