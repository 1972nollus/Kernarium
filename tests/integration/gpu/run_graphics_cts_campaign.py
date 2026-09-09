#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run eight receipt-bound Vulkan and/or Zink GL fractions in a six-guest pool.

GL runs come from authenticated XML and an ordered runtime EGL inventory. This is project
qualification, never a conformance submission or an all-features-supported claim.
Completed attempts are independently reparsed; failed attempts are never retried.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
from collections import Counter
from concurrent.futures import FIRST_COMPLETED, ThreadPoolExecutor, wait
from dataclasses import dataclass
from datetime import datetime, timezone
import fcntl
import hashlib
import json
import os
from pathlib import Path
import secrets
import signal
import stat
import subprocess
import sys
import threading
import time
from typing import Any, Callable, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_vk_gl_cts as common  # noqa: E402
import run_vk_cts as vk  # noqa: E402

FRACTIONS = 8
MAX_JOBS = 6
CPUS = 4
MEMORY = "4G"
KIND = "uml-venus-zink-graphics-cts-campaign"
RUNNERS = {
    "vulkan": Path(vk.__file__).resolve(),
    "gl46": Path(common.__file__).resolve(),
}


@dataclass(frozen=True, order=True)
class Shard:
    suite: str
    index: int
    run_index: int | None = None

    def name(self) -> str:
        run = f"-run{self.run_index:04d}" if self.run_index is not None else ""
        return f"{self.suite}{run}-{self.index:02d}of{FRACTIONS:02d}"


def shards(spec: dict[str, Any]) -> tuple[Shard, ...]:
    result = []
    if "gl46" in spec["suites"]:
        validate_opengl_run_plan(spec["opengl"])
    for index in range(FRACTIONS):
        for suite in spec["suites"]:
            if suite == "vulkan":
                result.append(Shard(suite, index))
            else:
                leaves = {leaf["id"]: leaf for leaf in
                          spec["opengl"]["receipt"]["opengl_cts"]["mustpass"]["leaves"]}
                for run in spec["opengl"]["runs"]:
                    # Preserve eight mathematical fractions, but never launch an
                    # empty one (some official no-context leaves have one case).
                    if index < leaves[run["leaf_id"]]["case_count"]:
                        result.append(Shard(suite, index, run["index"]))
    return tuple(result)


def validate_opengl_run_plan(item: dict[str, Any]) -> None:
    try:
        expected = common.opengl_cts_plan.expand_runs(
            item["receipt"]["opengl_cts"]["mustpass"], item["egl_configs"],
        )
    except common.opengl_cts_plan.PlanError as error:
        raise common.CampaignError(str(error)) from error
    if item["runs"] != expected:
        raise common.CampaignError("OpenGL run plan differs from its XML and ordered EGL inventory")


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def regular_file(path: Path) -> None:
    if not stat.S_ISREG(path.lstat().st_mode):
        raise common.CampaignError(f"not a regular, non-symlink file: {path}")


def load_json(path: Path) -> dict[str, Any]:
    regular_file(path)
    value = json.loads(path.read_text(encoding="utf-8"),
                       object_pairs_hook=common.reject_duplicate_keys)
    if not isinstance(value, dict):
        raise common.CampaignError(f"not a JSON object: {path}")
    return value


def write_json(path: Path, value: dict[str, Any]) -> None:
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("x", encoding="utf-8") as output:
        json.dump(value, output, indent=2, sort_keys=True)
        output.write("\n")
    os.replace(temporary, path)


def require(actual: dict[str, Any], expected: dict[str, Any]) -> None:
    for key, value in expected.items():
        if actual.get(key) != value:
            raise common.CampaignError(f"campaign evidence changed {key}")


def source_identity() -> dict[str, Any]:
    root = common.REPO_ROOT
    inputs = (
        Path(__file__).resolve(), RUNNERS["vulkan"], RUNNERS["gl46"],
        root / "tests/integration/gpu/run_gpu_guest.py",
        root / "tests/integration/gpu/guest_process.py",
        root / "tests/integration/gpu/vulkan_cts_receipt.py",
        root / "tests/integration/gpu/opengl_cts_plan.py",
        root / "tests/integration/gpu/vk_gl_cts_waivers.json",
        root / "scripts/boot_kde_vnc.py",
        root / "dependencies/manifest.tsv",
        root / "dependencies/patches/mesa/series.tsv",
        root / "dependencies/patches/utm-virglrenderer/series.tsv",
    )
    return {"git": common.git_state(), "files": {
        str(path.relative_to(root)): common.sha256_file(path) for path in inputs
    }}


def build_spec(options: argparse.Namespace) -> dict[str, Any]:
    emulator = options.emulator.resolve()
    regular_file(emulator)
    if not os.access(emulator, os.X_OK):
        raise common.CampaignError("emulator is not executable")
    suites = (["vulkan"] if options.suite == "vulkan" else
              ["gl46"] if options.suite == "opengl" else
              ["vulkan", "gl46"])
    spec = {
        "suites": suites, "fraction_count": FRACTIONS,
        "cpus": CPUS, "memory": MEMORY, "engine": "jit",
        "page_profile": options.page_profile,
        "shard_timeout": options.shard_timeout,
        "guest_timeout": options.guest_timeout,
        "console_stall_timeout": options.console_stall_timeout,
        "console_stall_busy_timeout": options.console_stall_busy_timeout,
        "max_resumptions": options.max_resumptions,
        "emulator": str(emulator),
        "emulator_sha256": common.sha256_file(emulator),
        "kosmickrisp_sha256": common.verify_staged_kosmickrisp(emulator),
        "source": source_identity(),
    }
    if "vulkan" in suites:
        receipt = vk.receipt_contract.validate_receipt(vk.RECEIPT_PATH)
        rootfs = options.vulkan_rootfs.resolve()
        vk.receipt_contract.verify_image(rootfs, receipt)
        spec["vulkan"] = {
            "rootfs": str(rootfs), "receipt": receipt,
            "receipt_path": str(vk.RECEIPT_PATH),
            "receipt_sha256": common.sha256_file(vk.RECEIPT_PATH),
        }
    if "gl46" in suites:
        receipt_path = options.opengl_receipt.resolve()
        receipt, contract = common.load_derivative_receipt(
            receipt_path, common.load_identity()
        )
        if (receipt.get("artifact") != "uml-vulkan-zink-gl-rootfs"
                or receipt.get("schema_version") != 4):
            raise common.CampaignError("OpenGL requires the XML-scoped schema-4 Zink receipt")
        rootfs = options.opengl_rootfs.resolve()
        common.validate_rootfs_file(rootfs, contract)
        inventory = options.opengl_config_inventory_run.resolve()
        configs, digest = common.load_config_inventory_evidence(
            inventory, "gl46", options.page_profile, "vnc", contract,
            {key: spec[key] for key in ("emulator_sha256", "kosmickrisp_sha256")},
        )
        config_rows = [{"id": config.identifier, "surface_types": list(config.surface_types)}
                       for config in configs]
        runs = common.opengl_cts_plan.expand_runs(receipt["opengl_cts"]["mustpass"], config_rows)
        inventory_files = [inventory, inventory.parent / "console.log",
                           inventory.parent / common.ARTIFACT_TRANSPORT_LOG,
                           *(inventory.parent / "artifacts" / name
                             for name in sorted(common.CONFIG_INVENTORY_ARTIFACTS))]
        for path in inventory_files:
            regular_file(path)
        spec["opengl"] = {
            "rootfs": str(rootfs), "receipt": receipt,
            "receipt_path": str(receipt_path),
            "receipt_sha256": common.sha256_file(receipt_path),
            "config_inventory_run": str(inventory), "egl_configs": config_rows,
            "egl_config_sha256": digest, "runs": runs,
            "inventory_files": {str(path): common.sha256_file(path) for path in inventory_files},
        }
    return spec


def check_current_inputs(spec: dict[str, Any]) -> None:
    emulator = Path(spec["emulator"])
    require({
        "emulator_sha256": common.sha256_file(emulator),
        "kosmickrisp_sha256": common.verify_staged_kosmickrisp(emulator),
        "source": source_identity(),
    }, {key: spec[key] for key in
        ("emulator_sha256", "kosmickrisp_sha256", "source")})
    for name in ("vulkan", "opengl"):
        if name in spec:
            item = spec[name]
            if common.sha256_file(Path(item["receipt_path"])) != item["receipt_sha256"]:
                raise common.CampaignError(f"{name} receipt changed during campaign")
    if "opengl" in spec:
        for name, digest in spec["opengl"]["inventory_files"].items():
            path = Path(name)
            regular_file(path)
            if common.sha256_file(path) != digest:
                raise common.CampaignError("OpenGL configuration inventory changed during campaign")


def child_command(spec: dict[str, Any], shard: Shard, output: Path) -> list[str]:
    command = [sys.executable, "-B", str(RUNNERS[shard.suite]),
               "--qualification", "--fraction", f"{shard.index},{FRACTIONS}",
               "--page-profile", spec["page_profile"], "--engine", "jit",
               "--cpus", str(CPUS), "--memory", MEMORY,
               "--shard-timeout", str(spec["shard_timeout"]),
               "--guest-timeout", str(spec["guest_timeout"]),
               "--console-stall-timeout", str(spec["console_stall_timeout"]),
               "--max-resumptions", str(spec["max_resumptions"]),
               "--emulator", spec["emulator"], "--artifact-root", str(output)]
    if shard.suite == "vulkan":
        command += ["--experimental-venus", "--mode", "current-must-pass",
                    "--rootfs", spec["vulkan"]["rootfs"]]
    else:
        command += ["--console-stall-busy-timeout", str(spec["console_stall_busy_timeout"])]
        run = spec["opengl"]["runs"][shard.run_index]
        command += ["--renderer", "zink", "--display", "vnc", "--matrix", "gl46",
                    "--egl-config-index", str(run["egl_config_index"]),
                    "--mustpass-config-index", str(run["configuration_index"]),
                    "--config-inventory-run", spec["opengl"]["config_inventory_run"],
                    "--rootfs-type", "ext4",
                    "--rootfs", spec["opengl"]["rootfs"],
                    "--rootfs-receipt", spec["opengl"]["receipt_path"]]
    return command


def attempt_path(campaign: Path, shard: Shard) -> Path:
    return campaign / "shards" / shard.name()


def group_exists(pgid: int) -> bool:
    try:
        os.killpg(pgid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        # Darwin can report EPERM while the owned leader is an unreaped zombie.
        # It is never proof that the group is gone: keep reaping/waiting, and
        # fail closed if permission remains denied through the cleanup bound.
        return True


def check_guest_ownership(path: Path, token: str, *, required: bool = False) -> None:
    try:
        common.guest_process.validate_journal(path, token, required=required)
    except common.guest_process.OwnershipError as error:
        raise common.CampaignError(str(error)) from error


def stop_owned_group(process: subprocess.Popen[bytes],
                     grace: float = common.guest_process.LEAF_CLEANUP_GRACE,
                     term_grace: float = 20, kill_grace: float = 5,
                     *, ownership: tuple[Path, str] | None = None) -> None:
    """Only signal the new session created for this exact owned child.

    Let the leaf's finally block reap its guest first. TERM/KILL also cover a
    same-group descendant left behind by an abruptly failed leaf. An emulator
    can create its own session: group disappearance alone is not cleanup proof.
    Its direct owner must also acknowledge wait() and socket/console cleanup.
    Never enumerate or signal unrelated emulators.
    """
    if process.poll() is None:
        try:
            process.send_signal(signal.SIGINT)
        except ProcessLookupError:
            pass
    for sig, timeout in ((None, grace), (signal.SIGTERM, term_grace),
                         (signal.SIGKILL, kill_grace)):
        if not group_exists(process.pid):
            process.wait(timeout=1)
            if ownership is not None:
                check_guest_ownership(*ownership, required=process.returncode == 0)
            return
        if sig is not None:
            try:
                os.killpg(process.pid, sig)
            except (ProcessLookupError, PermissionError):
                pass
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            process.poll()
            if not group_exists(process.pid):
                process.wait(timeout=1)
                if ownership is not None:
                    check_guest_ownership(*ownership, required=process.returncode == 0)
                return
            time.sleep(0.05)
    raise common.CampaignError(f"owned process group {process.pid} did not terminate")


def run_shard(campaign: Path, spec: dict[str, Any], shard: Shard,
              cancelled: threading.Event) -> dict[str, Any]:
    if cancelled.is_set():
        raise common.CampaignError("campaign cancelled before launch")
    check_current_inputs(spec)
    attempt = attempt_path(campaign, shard)
    attempt.mkdir(parents=True, exist_ok=False)
    output = attempt / "driver-output"
    output.mkdir()
    command = child_command(spec, shard, output)
    ownership = (attempt / common.guest_process.JOURNAL_NAME, secrets.token_hex(16))
    failure_path = attempt / "emulator-failure.json"

    def check_emulator_failure() -> None:
        if not failure_path.is_file():
            return
        failure = json.loads(failure_path.read_text())
        if failure.get("token") != ownership[1] or failure.get("reason") not in {
            "stalled", "dirty-shutdown",
        }:
            raise common.CampaignError("invalid emulator failure notification")
        cancelled.set()
        raise common.CampaignError(f"emulator failure: {failure['reason']}")

    record = {"shard": shard.name(), "command": command,
              "started_at": utc_now(), "state": "started",
              "guest_ownership_token": ownership[1]}
    write_json(attempt / "attempt.json", record)
    process = None
    error = None
    try:
        with (attempt / "runner.log").open("xb") as log:
            if cancelled.is_set():
                raise common.CampaignError("campaign cancelled before child creation")
            process = subprocess.Popen(command, cwd=common.REPO_ROOT, stdout=log,
                                       stderr=subprocess.STDOUT, start_new_session=True,
                                       env={**os.environ, common.guest_process.OWNERSHIP_ENV:
                                            json.dumps({"path": str(ownership[0]),
                                                        "token": ownership[1]}),
                                            common.CAMPAIGN_FAILURE_ENV: json.dumps({
                                                "path": str(failure_path),
                                                "token": ownership[1]})})
            record.update({"pid": process.pid, "pgid": process.pid})
            write_json(attempt / "attempt.json", record)
            # Each resumption is a fresh guest with its own guest timeout.
            deadline = time.monotonic() + (
                (spec["max_resumptions"] + 1) * spec["guest_timeout"] + 120
            )
            while process.poll() is None:
                check_emulator_failure()
                if time.monotonic() >= deadline:
                    cancelled.set()
                    raise common.CampaignError("leaf exceeded host deadline")
                if cancelled.is_set():
                    raise common.CampaignError("campaign cancelled")
                time.sleep(0.1)
            check_emulator_failure()
            if group_exists(process.pid):
                cancelled.set()
                raise common.CampaignError("leaf exited with live owned descendants")
            if process.returncode != 0:
                raise common.CampaignError(f"leaf runner exited {process.returncode}")
            check_guest_ownership(*ownership, required=True)
        check_current_inputs(spec)
    except BaseException as exception:
        error = exception
    finally:
        if process is not None:
            try:
                stop_owned_group(process, ownership=ownership)
            except BaseException as exception:
                # A surviving group cannot free a slot, even with --keep-going.
                cancelled.set()
                error = exception
    record.update({
        "finished_at": utc_now(), "state": "failed" if error else "finished",
        "return_code": process.returncode if process is not None else None,
        "runner_log_sha256": (common.sha256_file(attempt / "runner.log")
                              if (attempt / "runner.log").is_file() else None),
        "error": str(error) if error else None,
        "guest_ownership_sha256": (common.sha256_file(ownership[0])
                                   if ownership[0].is_file() else None),
    })
    write_json(attempt / "attempt.json", record)
    if error:
        raise error
    return validate_attempt(campaign, spec, shard)


def read_artifacts(run: Path, names: set[str], merged: bool = False) -> dict[str, bytes]:
    directory = run / "artifacts"
    if directory.is_symlink() or not directory.is_dir():
        raise common.CampaignError("invalid artifact directory")
    if {entry.name for entry in directory.iterdir()} != names:
        raise common.CampaignError("artifact set changed")
    artifacts = {}
    for name in names:
        path = directory / name
        regular_file(path)
        if path.stat().st_size > common.MAX_UNCOMPRESSED_ARTIFACT:
            raise common.CampaignError("artifact exceeds the output bound")
        artifacts[name] = path.read_bytes()
    if merged:
        # A host-merged shard is checked against every session's own transport
        # by the shared provenance validator, which also recomputes the merge.
        return artifacts
    serial = run / common.ARTIFACT_TRANSPORT_LOG
    regular_file(serial)
    if serial.stat().st_size > len(names) * common.MAX_COMPRESSED_ARTIFACT * 2:
        raise common.CampaignError("artifact transport exceeds its bounded frame set")
    decoded = common.decode_artifacts(serial.read_text(encoding="ascii"))
    if decoded != artifacts:
        raise common.CampaignError("artifact transport differs from decoded files")
    return artifacts


def check_session_ownership(run: Path, metadata: dict[str, Any],
                            token: Any) -> tuple[Path, bool]:
    """Prove every continuation guest was reaped.

    Returns the last session's console and whether that guest still owes a
    clean stop marker, which it does unless the host recorded why it died.
    """
    sessions = metadata.get("sessions")
    if not isinstance(sessions, list) or not sessions:
        return run / "console.log", True
    for record in sessions[1:]:
        journal = record.get("ownership_journal")
        if not isinstance(journal, str):
            raise common.CampaignError("continuation guest omits its ownership journal")
        check_guest_ownership(run / journal, token, required=True)
    return (run / str(sessions[-1]["console"]),
            sessions[-1].get("terminated_case") is None)


def validate_attempt(campaign: Path, spec: dict[str, Any], shard: Shard,
                     *, include_results: bool = False) -> dict[str, Any]:
    attempt = attempt_path(campaign, shard)
    if attempt.is_symlink() or not attempt.is_dir():
        raise common.CampaignError("invalid attempt directory")
    record = load_json(attempt / "attempt.json")
    log = attempt / "runner.log"
    regular_file(log)
    output = attempt / "driver-output"
    if output.is_symlink() or not output.is_dir():
        raise common.CampaignError("invalid driver output directory")
    require(record, {"shard": shard.name(), "state": "finished", "return_code": 0,
                     "error": None, "command": child_command(spec, shard, output),
                     "runner_log_sha256": common.sha256_file(log)})
    ownership = attempt / common.guest_process.JOURNAL_NAME
    check_guest_ownership(ownership, record.get("guest_ownership_token"), required=True)
    require(record, {"guest_ownership_sha256": common.sha256_file(ownership)})
    runs = list(output.iterdir())
    if len(runs) != 1 or runs[0].is_symlink() or not runs[0].is_dir():
        raise common.CampaignError("attempt must contain exactly one leaf run")
    run = runs[0]
    metadata = load_json(run / "run.json")
    console = run / "console.log"
    regular_file(console)
    require(metadata, {
        "qualification": True, "formal_conformance": False,
        "engine": "jit", "memory": MEMORY, "cpus": CPUS,
        "page_profile": spec["page_profile"],
        "emulator_sha256": spec["emulator_sha256"],
        "kosmickrisp_sha256": spec["kosmickrisp_sha256"],
        "git": spec["source"]["git"],
        "console_sha256": common.sha256_file(console),
    })
    args = record["command"][3:]
    merged = metadata.get("artifacts_provenance") == "host-merged"
    final_console, clean_stop = check_session_ownership(
        run, metadata, record.get("guest_ownership_token"))
    require(metadata, {"max_resumptions": spec["max_resumptions"]})
    if shard.suite == "vulkan":
        receipt = spec["vulkan"]["receipt"]
        options = vk.parse_arguments(args)
        selection = vk.resolve_receipt_selection("current-must-pass", [], None,
                                                 vk.Fraction(shard.index, FRACTIONS))
        require(metadata, {
            "schema_version": 3, "state": "verified", "experimental_venus": True,
            "receipt_sha256": spec["vulkan"]["receipt_sha256"],
            "shard_timeout": spec["shard_timeout"], "guest_timeout": spec["guest_timeout"],
            "console_stall_timeout": spec["console_stall_timeout"],
            "cts_sha256": receipt["cts"]["executable"]["sha256"],
            "selection": {"mode": "current-must-pass", "exact_cases": [], "family": None,
                          "fraction": [shard.index, FRACTIONS], "expected_cases": []},
            "command": vk.build_receipt_harness_command(options, receipt, selection, run),
        })
        artifacts = read_artifacts(run, set(common.ENUMERATED_SHARD_ARTIFACTS), merged)
        vk.verify_receipt_only(run, metadata, receipt)
        selected = common.parse_case_list(artifacts["expected.txt"].decode("utf-8"))
        results = common.validate_qpa_union(common.decode_qpa(artifacts["results.qpa"]), selected)
    else:
        item = spec["opengl"]
        receipt = item["receipt"]
        options = common.parse_arguments(args)
        selection = options.matrix_run.selection
        leaf = selection.leaf
        waived = common.waived_cases(options.applied_waivers)
        require(metadata, {
            "console_stall_timeout": spec["console_stall_timeout"],
            "console_stall_busy_timeout": spec["console_stall_busy_timeout"],
            "schema_version": 1, "adapter": "macos", "backend": "venus-kosmickrisp",
            "renderer": "zink", "display": "vnc", "context": "egl", "mode": "mustpass-matrix-shard",
            "surface": [1280, 720], "rootfs_type": "ext4",
            "rootfs_receipt": receipt, "rootfs_receipt_sha256": item["receipt_sha256"],
            "rootfs_sha256": receipt["derivative"]["sha256"],
            "glcts_sha256": receipt["opengl_cts"]["executable"]["sha256"],
            "successful_case_statuses": receipt["runner_oracle"]["successful"],
            "api": "gl46", "leaf": leaf.identifier, "leaf_sha256": leaf.sha256,
            "fraction": [shard.index, FRACTIONS], "exact_cases": [],
            "seed": options.matrix_run.configuration.base_seed,
            "matrix": common.matrix_metadata(options),
            "expected_cases": selection.expected_count, "emulator_return_code": 0,
            "command": common.build_harness_command(options, selection, run),
        })
        artifacts = read_artifacts(run, set(common.SHARD_ARTIFACTS), merged)
        summary = artifacts["summary.txt"].decode("utf-8")
        if merged:
            common.validate_session_provenance(
                run, metadata, artifacts, selection.expected_count
            )
            if "provenance=host-merged" not in summary or "cts_rc=1" not in summary:
                raise common.CampaignError("OpenGL host-merged summary is not truthful")
        elif summary.strip() != "cts_rc=0":
            raise common.CampaignError("OpenGL CTS process failed")
        if clean_stop:
            common.validate_guest_shutdown(final_console.read_text(encoding="utf-8"))
        common.validate_renderer_probe(artifacts["renderer.txt"].decode("utf-8"),
                                      16384 if spec["page_profile"] == "16k" else 4096, "zink")
        selected = common.parse_case_list(artifacts["selected.txt"].decode("utf-8"))
        if len(selected) != selection.expected_count:
            raise common.CampaignError("OpenGL selected-case count changed")
        qpa_text = common.decode_qpa(artifacts["results.qpa"])
        results = common.validate_qpa_union(qpa_text, selected)
        common.validate_waiver_session(qpa_text, options.applied_waivers)
        require(metadata, {
            "completed_cases": len(results), "status_totals": common.status_totals(results),
            "waived_results": {case: status for case, status in results.items() if case in waived},
            "selected_caselist_sha256": hashlib.sha256(artifacts["selected.txt"]).hexdigest(),
            "qpa_sha256": hashlib.sha256(artifacts["results.qpa"]).hexdigest(),
            "renderer_sha256": hashlib.sha256(artifacts["renderer.txt"]).hexdigest(),
            "artifact_transport_sha256": common.sha256_file(run / common.ARTIFACT_TRANSPORT_LOG),
        })
    common.validate_official_runner_results(
        results, receipt["runner_oracle"]["successful"],
        waived if shard.suite == "gl46" else ())
    value = {"status_totals": common.status_totals(results), "cases": len(results)}
    if include_results:
        value["results"] = results
    return value


def aggregate(campaign: Path, spec: dict[str, Any]) -> dict[str, Any]:
    suites = {}
    for suite in spec["suites"]:
        if suite == "gl46":
            suites[suite] = aggregate_opengl(campaign, spec)
            continue
        counts: Counter[str] = Counter()
        totals: Counter[str] = Counter()
        first_status: dict[str, str] = {}
        for index in range(FRACTIONS):
            result = validate_attempt(campaign, spec, Shard(suite, index), include_results=True)
            rows = result["results"]
            totals.update(result["status_totals"])
            counts.update(rows.keys())
            for case, status in rows.items():
                if case in first_status and first_status[case] != status:
                    raise common.CampaignError("repeated mandatory case changed status")
                first_status[case] = status
        mandatory = set(vk.receipt_mandatory_cases())
        if (len(counts) != spec["vulkan"]["receipt"]["mustpass"]["unique_case_count"]
                or not mandatory.issubset(counts)
                or any(count != (FRACTIONS if case in mandatory else 1)
                       for case, count in counts.items())):
            raise common.CampaignError("Vulkan union omitted/repeated cases outside its mandatory list")
        suites[suite] = {
            "fractions": FRACTIONS, "unique_case_count": len(counts),
            "case_execution_count": sum(totals.values()),
            "status_totals": dict(sorted(totals.items())),
            "unique_status_totals": dict(sorted(Counter(first_status.values()).items())),
            "all_pass": set(totals) == {"Pass"},
            "waived_cases": sorted(
                case for case, status in first_status.items()
                if status == common.WAIVED_STATUS),
        }
    return {"complete": True, "formal_conformance": False,
            "unwaived_runner_success": not any(
                suite["waived_cases"] for suite in suites.values()),
            "suites": suites}


def aggregate_opengl(campaign: Path, spec: dict[str, Any]) -> dict[str, Any]:
    item = spec["opengl"]
    validate_opengl_run_plan(item)
    manifest = item["receipt"]["opengl_cts"]["mustpass"]
    leaves = {row["id"]: row for row in manifest["leaves"]}
    unique: set[str] = set()
    totals: Counter[str] = Counter()
    waived_cases: set[str] = set()
    observations = []
    for run in item["runs"]:
        leaf = leaves[run["leaf_id"]]
        fractions = [[] for _ in range(FRACTIONS)]
        counts: Counter[str] = Counter()
        run_totals: Counter[str] = Counter()
        run_waived: set[str] = set()
        for index in range(min(FRACTIONS, leaf["case_count"])):
            result = validate_attempt(
                campaign, spec, Shard("gl46", index, run["index"]), include_results=True,
            )
            rows = result["results"]
            expected = (leaf["case_count"] - index + FRACTIONS - 1) // FRACTIONS
            if len(rows) != expected:
                raise common.CampaignError("OpenGL fraction count changed")
            fractions[index] = list(rows)
            counts.update(rows.keys())
            run_totals.update(result["status_totals"])
            run_waived.update(
                case for case, status in rows.items() if status == common.WAIVED_STATUS)
        ordered = [fractions[index % FRACTIONS][index // FRACTIONS]
                   for index in range(leaf["case_count"])]
        payload = "".join(f"{case}\n" for case in ordered).encode("utf-8")
        if (len(counts) != leaf["case_count"] or any(count != 1 for count in counts.values())
                or len(payload) != leaf["size"]
                or hashlib.sha256(payload).hexdigest() != leaf["sha256"]):
            raise common.CampaignError("OpenGL fraction union changed the XML-referenced leaf")
        unique.update(counts)
        totals.update(run_totals)
        waived_cases.update(run_waived)
        observations.append({
            **run, "fraction_count": FRACTIONS,
            "nonempty_fraction_count": min(FRACTIONS, leaf["case_count"]),
            "unique_case_count": len(counts), "case_execution_count": sum(run_totals.values()),
            "status_totals": dict(sorted(run_totals.items())),
            "all_pass": set(run_totals) == {"Pass"},
            "waived_cases": sorted(run_waived),
        })
    if len(unique) != manifest["unique_case_count"]:
        raise common.CampaignError("OpenGL XML scope unique union changed")
    return {
        "fraction_count": FRACTIONS, "run_count": len(observations),
        "unique_case_count": len(unique), "case_execution_count": sum(totals.values()),
        "status_totals": dict(sorted(totals.items())), "all_pass": set(totals) == {"Pass"},
        "waived_cases": sorted(waived_cases),
        "runs": observations,
    }


def run_pending(campaign: Path, spec: dict[str, Any], pending: list[Shard],
                jobs: int, keep_going: bool, cancelled: threading.Event,
                completed: dict[str, Any], failures: dict[str, str],
                on_start: Callable[[Shard], None] | None = None) -> None:
    running = {}
    next_heartbeat = time.monotonic() + 30
    with ThreadPoolExecutor(max_workers=jobs) as executor:
        try:
            while pending or running:
                while pending and len(running) < jobs and not cancelled.is_set():
                    shard = pending.pop(0)
                    if on_start is not None:
                        on_start(shard)
                    running[executor.submit(run_shard, campaign, spec, shard, cancelled)] = shard
                    print(f"Starting {shard.name()}", flush=True)
                if not running:
                    break
                done, _ = wait(running, timeout=0.25, return_when=FIRST_COMPLETED)
                for future in done:
                    shard = running.pop(future)
                    try:
                        completed[shard.name()] = future.result()
                        print(f"Verified {shard.name()}: {completed[shard.name()]}", flush=True)
                    except Exception as error:
                        failures[shard.name()] = str(error)
                        print(f"Failed {shard.name()}: {error}", file=sys.stderr, flush=True)
                        if not keep_going:
                            cancelled.set()
                if time.monotonic() >= next_heartbeat:
                    print(f"Graphics CTS: {len(completed)} verified, {len(failures)} failed, "
                          f"{len(running)} active, {len(pending)} pending", flush=True)
                    next_heartbeat = time.monotonic() + 30
        finally:
            cancelled.set()


def parse_arguments(arguments: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", choices=("vulkan", "opengl", "both"), required=True)
    parser.add_argument("--campaign-dir", type=Path, required=True)
    parser.add_argument("--emulator", type=Path, required=True)
    parser.add_argument("--vulkan-rootfs", type=Path)
    parser.add_argument("--opengl-rootfs", type=Path)
    parser.add_argument("--opengl-receipt", type=Path)
    parser.add_argument("--opengl-config-inventory-run", type=Path,
                        help="verified run.json from Zink CTS-Configs.gl46 on these exact inputs")
    parser.add_argument("--page-profile", choices=("16k", "4k"), default="16k")
    parser.add_argument("--jobs", type=common.positive_integer, default=MAX_JOBS,
                        help=f"concurrent owned guests, 1 through {MAX_JOBS} (default: {MAX_JOBS}); each is 4 CPU/4 GiB")
    parser.add_argument("--shard-timeout", type=common.positive_integer, default=21600)
    parser.add_argument("--guest-timeout", type=common.positive_integer, default=22200)
    parser.add_argument("--console-stall-busy-timeout", type=common.positive_integer, default=3600,
                        help="bound for an OpenGL case that emits no QPA output but keeps the "
                             "guest CTS process busy (must be at least the stall timeout)")
    parser.add_argument("--console-stall-timeout", type=common.positive_integer, default=600,
                        help="guest console/transport progress bound in seconds (default: 600)")
    parser.add_argument("--max-resumptions", type=int, default=common.DEFAULT_MAX_RESUMPTIONS,
                        help="fresh guests a stalled or aborted shard may continue in "
                             f"(default: {common.DEFAULT_MAX_RESUMPTIONS}; 0 keeps one session)")
    parser.add_argument("--keep-going", action="store_true",
                        help="finish other fractions after CTS failures; emulator stalls "
                             "or dirty shutdown always stop the pool")
    parser.add_argument("--verify-only", action="store_true", help="reparse every immutable attempt without launching guests")
    parser.add_argument("--dry-run", action="store_true", help="verify inputs and print commands without writing or launching")
    options = parser.parse_args(arguments)
    if options.jobs > MAX_JOBS:
        parser.error(f"--jobs may not exceed {MAX_JOBS}")
    if options.console_stall_busy_timeout < options.console_stall_timeout:
        parser.error("--console-stall-busy-timeout must be at least --console-stall-timeout")
    if options.guest_timeout <= options.shard_timeout:
        parser.error("--guest-timeout must exceed --shard-timeout")
    if options.max_resumptions < 0:
        parser.error("--max-resumptions must not be negative")
    if options.verify_only and options.dry_run:
        parser.error("--verify-only and --dry-run are mutually exclusive")
    if options.suite != "opengl" and options.vulkan_rootfs is None:
        parser.error("Vulkan requires --vulkan-rootfs")
    if options.suite != "vulkan" and (
        options.opengl_rootfs is None or options.opengl_receipt is None
        or options.opengl_config_inventory_run is None
    ):
        parser.error("OpenGL requires --opengl-rootfs, --opengl-receipt and --opengl-config-inventory-run")
    return options


def main(arguments: Sequence[str] | None = None) -> int:
    options = parse_arguments(arguments)
    spec = build_spec(options)
    campaign = options.campaign_dir.absolute()
    if campaign.is_symlink() or campaign in (common.REPO_ROOT, Path("/")):
        raise common.CampaignError("campaign requires its own non-symlink directory")
    if options.dry_run:
        for shard in shards(spec):
            print(json.dumps(child_command(spec, shard, attempt_path(campaign, shard) / "driver-output")))
        return 0
    if options.verify_only and not campaign.is_dir():
        raise common.CampaignError("campaign does not exist")
    campaign.mkdir(parents=True, exist_ok=True)
    allowed = {"campaign.json", "campaign.lock", "shards"}
    if any(entry.name not in allowed for entry in campaign.iterdir()):
        raise common.CampaignError("campaign directory contains unrelated files")
    flags = os.O_RDONLY if options.verify_only else os.O_RDWR | os.O_CREAT
    lock = os.open(campaign / "campaign.lock", flags | os.O_NOFOLLOW, 0o600)
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        path = campaign / "campaign.json"
        if path.exists():
            record = load_json(path)
            require(record, {"schema_version": 1, "kind": KIND, "spec": spec})
        elif any(entry.name == "shards" for entry in campaign.iterdir()):
            raise common.CampaignError("attempts exist without an original campaign specification")
        else:
            if options.verify_only:
                raise common.CampaignError("campaign specification is missing")
            record = {"schema_version": 1, "kind": KIND, "spec": spec,
                      "created_at": utc_now(), "state": "pending", "attempted_shards": []}
            write_json(path, record)
        completed, failures, pending = {}, {}, []
        expected_names = {shard.name() for shard in shards(spec)}
        attempted = record.get("attempted_shards")
        if (not isinstance(attempted, list)
                or any(not isinstance(name, str) for name in attempted)
                or len(set(attempted)) != len(attempted)
                or not set(attempted).issubset(expected_names)):
            raise common.CampaignError("invalid immutable attempt catalog")
        shard_root = campaign / "shards"
        if shard_root.exists():
            if shard_root.is_symlink() or not shard_root.is_dir():
                raise common.CampaignError("invalid shard directory")
            if any(entry.name not in expected_names for entry in shard_root.iterdir()):
                raise common.CampaignError("campaign contains an unexpected shard")
        for shard in shards(spec):
            exists = os.path.lexists(attempt_path(campaign, shard))
            if exists != (shard.name() in attempted):
                failures[shard.name()] = "attempt directory and original launch catalog disagree"
            elif exists:
                try:
                    completed[shard.name()] = validate_attempt(campaign, spec, shard)
                except (common.CampaignError, OSError, ValueError) as error:
                    failures[shard.name()] = str(error)
            else:
                pending.append(shard)
        if failures:
            raise common.CampaignError(f"campaign has immutable failed/incomplete attempts: {failures}")
        if options.verify_only:
            if pending:
                raise common.CampaignError("campaign is incomplete")
            result = aggregate(campaign, spec)
            require(record, {"state": "complete", "aggregate": result})
            print(json.dumps(result, sort_keys=True))
            return 0
        cancelled = threading.Event()
        received_signal = []

        def cancel(signum: int, _frame: Any) -> None:
            if not received_signal:
                received_signal.append(signum)
            cancelled.set()

        handlers = {number: signal.signal(number, cancel) for number in (signal.SIGINT, signal.SIGTERM)}

        def mark_started(shard: Shard) -> None:
            # Persist before creating even the worker. A crash in the following
            # launch cannot turn an attempted shard back into a pending one.
            attempted.append(shard.name())
            record.update({"state": "running", "updated_at": utc_now()})
            write_json(path, record)

        try:
            run_pending(campaign, spec, pending, options.jobs, options.keep_going,
                        cancelled, completed, failures, mark_started)
        finally:
            for number, handler in handlers.items():
                signal.signal(number, handler)
        record.update({"updated_at": utc_now(), "completed": completed, "failures": failures,
                       "state": "failed" if failures or pending or received_signal else "complete"})
        if record["state"] == "complete":
            check_current_inputs(spec)
            record["aggregate"] = aggregate(campaign, spec)
        write_json(path, record)
        print(json.dumps(record.get("aggregate", {"state": record["state"], "failures": failures}), sort_keys=True))
        return 128 + received_signal[0] if received_signal else int(record["state"] != "complete")
    finally:
        os.close(lock)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (common.CampaignError, vk.receipt_contract.ReceiptError, OSError, ValueError) as error:
        print(f"{Path(__file__).name}: {error}", file=sys.stderr)
        raise SystemExit(1) from None
