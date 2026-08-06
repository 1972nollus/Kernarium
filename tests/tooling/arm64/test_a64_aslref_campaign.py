#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Publication, integrity, and resume fixtures for A64 ASLRef campaigns."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import copy
import gzip
import hashlib
import json
import os
import subprocess
import sys
import sysconfig
import tempfile
from pathlib import Path

import run_a64_aslref_campaign as campaign
from run_a64_aslref_campaign import (
    MAX_SHARD_CONTRACTS,
    PLAN_SCHEMA,
    campaign_lock,
    canonical_json,
    file_identity,
    publish_shard,
    require_root,
    run_campaign,
    validate_plan,
    verify_campaign,
    verify_published_shard,
)
from test_a64_input_materialization import event_stream, materialization


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def expect_failure(action, message: str) -> None:
    try:
        action()
    except (OSError, ValueError):
        return
    raise AssertionError(message)


def write_canonical(path: Path, value: object) -> None:
    path.write_text(canonical_json(value) + "\n", encoding="utf-8")


def fixture_plan(root: Path) -> tuple[Path, dict[str, object], dict[str, object]]:
    frozen = root / "frozen-source.txt"
    frozen.write_text("immutable source\n", encoding="utf-8")
    tooling_root = root / "probe-tools"
    tooling_root.mkdir()
    external_probe = tooling_root / "external-probe.py"
    external_probe.write_text("# frozen external probe\n", encoding="utf-8")
    python_runtime = root / "python-runtime"
    architecture_root = root / "architecture"
    closure_root = root / "closure-captures"
    for directory in (python_runtime, architecture_root, closure_root):
        directory.mkdir()
        (directory / "frozen.txt").write_text(
            f"{directory.name}\n", encoding="utf-8"
        )
    shard_input = root / "shard.inputs.jsonl.gz"
    record = materialization(4096)
    with gzip.open(shard_input, "wt", encoding="utf-8", newline="") as output:
        output.write(event_stream([record]))
    input_identity = file_identity(shard_input)
    count, input_digest = campaign.scan_shard_input(
        shard_input, guest_page_size=4096
    )
    shard = {
        "shard_id": "shard-00000000",
        "input": {
            "path": input_identity["path"],
            "size": input_identity["size"],
            "sha256": input_identity["sha256"],
        },
        "contract_count": count,
        "input_contract_ids_sha256": input_digest,
    }
    absolute = str(external_probe.resolve())
    frozen_entries = {
        str(entry["path"]): entry
        for entry in (
            file_identity(frozen),
            file_identity(external_probe),
            file_identity(shard_input),
            campaign.tree_identity(tooling_root),
            campaign.python_runtime_identity(python_runtime),
            campaign.tree_identity(architecture_root),
            campaign.tree_identity(closure_root),
        )
    }
    plan = {
        "schema": PLAN_SCHEMA,
        "campaign_id": "publication-fixture",
        "guest_page_size": 4096,
        "shard_contract_cap": MAX_SHARD_CONTRACTS,
        "total_contracts": 1,
        "source_freeze": [
            frozen_entries[path] for path in sorted(frozen_entries)
        ],
        "authoritative_input": {
            "schema": "uml.a64.aslref-authoritative-input/v1",
            "guest_page_size": 4096,
            "aggregate_manifest": file_identity(frozen),
            "aggregate_manifest_identity": {"fixture": True},
            "aggregate_manifest_identity_file": file_identity(frozen),
            "materialization_index": file_identity(frozen),
            "contract_manifests": [file_identity(shard_input)],
            "checkpoint_files": [file_identity(shard_input)],
            "join_model_digest": "1" * 64,
            "page_aggregate_count": 1,
            "eligible_aggregate_count": 1,
            "input_contract_count": 1,
            "input_contract_ids_sha256": "2" * 64,
            "aggregate_input_binding_count": 1,
            "aggregate_input_binding_sha256": "3" * 64,
            "checkpoint_count": 1,
            "checkpoint_manifest_sha256": "4" * 64,
        },
        "probe": {
            "path": absolute,
            "python": absolute,
            "python_library": absolute,
            "python_runtime": str(python_runtime.resolve()),
            "architecture_root": str(architecture_root.resolve()),
            "aslref": absolute,
            "bundler": absolute,
            "features_json": absolute,
            "closure_capture_directory": str(closure_root.resolve()),
            "required_variants": absolute,
            "interface": absolute,
            "observer": absolute,
            "timeout_seconds": 1,
            "max_rss_kib": 1,
            "max_encoded_bytes": 1,
            "max_artifact_bytes": 1024 * 1024,
            "max_artifact_files": 128,
            "max_log_bytes": 4096,
        },
        "shards": [shard],
    }
    validate_plan(plan)
    path = root / "campaign-plan.json"
    write_canonical(path, plan)
    return path, plan, shard


def populate_probe_artifact(
    artifact: Path, shard: dict[str, object], guest_page_size: int = 4096
) -> None:
    artifact.mkdir()
    source = shard["input"]
    assert isinstance(source, dict)
    identifiers: list[str] = []
    count, identifier_digest = campaign.scan_shard_input(
        Path(str(source["path"])),
        guest_page_size=guest_page_size,
        identifiers_out=identifiers,
    )
    _, result_digest = campaign._digest_tuples(
        (identifier, "official-asl-passed") for identifier in identifiers
    )
    workflow = {
        "purpose": "official-execution-shard-pending-campaign-join",
        "semantic_coverage_promotion": "requires-exact-frozen-join",
        "unique_input_contracts": count,
        "failures": 0,
        "guest_page_size": guest_page_size,
        "input_contract_ids_sha256": identifier_digest,
        "input_result_count": count,
        "input_results_sha256": result_digest,
        "input_results": [
            {
                "input_contract_id": identifier,
                "official_asl_result": "official-asl-passed",
            }
            for identifier in identifiers
        ],
        "input_sources": [
            {
                "path": str(Path(str(source["path"])).resolve()),
                "size": source["size"],
                "sha256": source["sha256"],
            }
        ],
    }
    report = {
        "schema": campaign.PROBE_FORMAT,
        "contracts": count,
        "selection": "explicit-unique-campaign-shard",
        "promotable": False,
        "diagnostic": True,
        "diagnostic_maximum_phase": "full",
        "official_closure_seed_contracts": 4,
        "executed_source_contracts": count,
        "unique_input_contracts": count,
        "scale_workflow": None,
        "campaign_workflow": workflow,
        "maximum_backing_contract": None,
        "full_extent_transfer_contract": None,
        "full_extent_execution_policy": None,
        "peak_rss_kib": 123456,
        "rss_measurement": "live-ps+rusage",
        "termination_reason": "exit",
        "elapsed_seconds": 1.0,
        "contracts_per_second": float(count),
    }
    expected_stdout = (
        f"FORMAT={campaign.PROBE_FORMAT}|COUNT={count}|FAILURES=0\n"
    )
    result = {
        "schema": "uml.a64.aslref-materialization-batch-result/v1",
        "returncode": 0,
        "stdout": expected_stdout,
        "stderr": "",
        "elapsed_seconds": 1.0,
        "peak_rss_kib": 123456,
        "rss_measurement": "live-ps+rusage",
        "termination_reason": "exit",
        "accepted": False,
        "passed": True,
        "promotable": False,
        "diagnostic": True,
        "diagnostic_maximum_phase": "full",
    }
    write_canonical(artifact / "batch-report.json", report)
    (artifact / "aslref-result.json").write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    (artifact / "aslref.stdout").write_text(expected_stdout, encoding="utf-8")
    (artifact / "aslref.stderr").write_bytes(b"")
    (artifact / "campaign-probe.stdout").write_text(
        canonical_json(report) + "\n", encoding="utf-8"
    )
    (artifact / "campaign-probe.stderr").write_bytes(b"")
    (artifact / "payload.txt").write_text("verified payload\n", encoding="utf-8")
    nested = artifact / "nested"
    nested.mkdir()
    (nested / "value.bin").write_bytes(bytes(range(32)))


def reseal_published_shard(final: Path) -> None:
    manifest = campaign._artifact_manifest(final)
    write_canonical(final / "artifact-manifest.json", manifest)
    success = dict(campaign.read_canonical_json(final / "SUCCESS.json"))
    files = manifest["files"]
    assert isinstance(files, list)
    success["artifact_manifest_sha256"] = file_identity(
        final / "artifact-manifest.json"
    )["sha256"]
    success["artifact_file_count"] = len(files)
    success["artifact_bytes"] = sum(int(entry["size"]) for entry in files)
    success["probe_report_sha256"] = file_identity(
        final / "batch-report.json"
    )["sha256"]
    success["probe_result_sha256"] = file_identity(
        final / "aslref-result.json"
    )["sha256"]
    write_canonical(final / "SUCCESS.json", success)


def run_options(
    plan: Path,
    output: Path,
    local: Path,
    workers: int = 1,
    directory_sync_policy: str = "require",
):
    return argparse.Namespace(
        plan=plan,
        output_root=output,
        local_temp_root=local,
        workers=workers,
        minimum_output_free_gib=0.0,
        minimum_local_free_gib=0.0,
        maximum_authority_database_bytes=16 * 1024 * 1024,
        maximum_authority_merge_scratch_bytes=16 * 1024 * 1024,
        directory_sync_policy=directory_sync_policy,
    )


def verify_options(plan: Path, output: Path, local: Path | None = None):
    return argparse.Namespace(
        plan=plan,
        output_root=output,
        local_temp_root=(
            local if local is not None else output.parent / "local"
        ),
        minimum_local_free_gib=0.0,
        maximum_authority_database_bytes=16 * 1024 * 1024,
        maximum_authority_merge_scratch_bytes=16 * 1024 * 1024,
    )


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="a64-campaign-fixture-") as temporary:
        root = Path(temporary)
        plan_path, plan, shard = fixture_plan(root)
        probe_argv = campaign._probe_argv(plan, shard, root / "artifact")
        reserve_index = probe_argv.index("--minimum-generated-free-gib")
        require(
            probe_argv[reserve_index + 1]
            == str(campaign.PROBE_MINIMUM_GENERATED_FREE_GIB),
            "campaign probe did not receive its explicit output reserve",
        )
        file_cap_index = probe_argv.index("--max-artifact-files")
        require(
            probe_argv[file_cap_index + 1]
            == str(plan["probe"]["max_artifact_files"]),
            "campaign probe did not receive its frozen artifact file cap",
        )
        external_probe = Path(str(plan["probe"]["path"]))
        external_probe_bytes = external_probe.read_bytes()
        external_probe.write_text("# mutated external probe\n", encoding="utf-8")
        expect_failure(
            lambda: campaign.verify_source_freeze(plan),
            "external configured probe mutation escaped source freeze",
        )
        external_probe.write_bytes(external_probe_bytes)
        campaign.verify_source_freeze(plan)
        runtime_identity = campaign.python_runtime_identity(
            Path(sysconfig.get_path("stdlib")).resolve(strict=True)
        )
        require(
            runtime_identity["kind"] == "python-runtime"
            and int(runtime_identity["file_count"]) > 0,
            "actual isolated Python runtime could not be frozen",
        )
        hostile = root / "hostile-pythonpath"
        hostile.mkdir()
        (hostile / "hashlib.py").write_text(
            "raise RuntimeError('hostile PYTHONPATH imported')\n",
            encoding="utf-8",
        )
        hostile_environment = dict(os.environ)
        hostile_environment["PYTHONPATH"] = str(hostile)
        probe_help = subprocess.run(
            [
                str(Path(sys.executable).resolve(strict=True)),
                *campaign.ISOLATED_PYTHON_FLAGS,
                str(
                    Path(campaign.__file__).with_name(
                        "probe_a64_aslref_materialization_batch.py"
                    )
                ),
                "--help",
            ],
            env=hostile_environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        require(
            probe_help.returncode == 0,
            "hostile PYTHONPATH affected the isolated campaign probe: "
            + probe_help.stderr[-1000:],
        )
        require(
            all(
                not name.upper().startswith("PYTHON")
                for name in campaign.sanitized_probe_environment()
            ),
            "campaign probe environment retained a Python control variable",
        )
        bool_mutations = (
            lambda value: value.__setitem__("guest_page_size", True),
            lambda value: value.__setitem__("shard_contract_cap", True),
            lambda value: value["source_freeze"][0].__setitem__("size", True),
            lambda value: value["authoritative_input"].__setitem__(
                "eligible_aggregate_count", True
            ),
            lambda value: value["probe"].__setitem__("timeout_seconds", True),
            lambda value: value["shards"][0]["input"].__setitem__("size", True),
            lambda value: value["shards"][0].__setitem__("contract_count", True),
            lambda value: value.__setitem__("total_contracts", True),
        )
        for mutate in bool_mutations:
            changed_plan = copy.deepcopy(plan)
            mutate(changed_plan)
            expect_failure(
                lambda changed_plan=changed_plan: validate_plan(changed_plan),
                "boolean plan scalar was accepted as an integer",
            )
        for name, replacement in (
            ("guest_page_size", 4096.0),
            ("shard_contract_cap", 1024.0),
        ):
            changed_plan = copy.deepcopy(plan)
            changed_plan[name] = replacement
            if name == "guest_page_size":
                changed_plan["authoritative_input"][name] = replacement
            expect_failure(
                lambda changed_plan=changed_plan: validate_plan(changed_plan),
                f"floating-point plan scalar {name} was accepted",
            )
        missing_tool_freeze = copy.deepcopy(plan)
        probe_path = str(missing_tool_freeze["probe"]["path"])
        missing_tool_freeze["source_freeze"] = [
            entry
            for entry in missing_tool_freeze["source_freeze"]
            if entry["path"] != probe_path
        ]
        expect_failure(
            lambda: validate_plan(missing_tool_freeze),
            "configured probe without an exact source freeze was accepted",
        )
        noncanonical_tsv = root / "noncanonical.tsv"
        noncanonical_tsv.write_bytes(b"a\tb\r\n1\t2\r\n")
        expect_failure(
            lambda: campaign.read_tsv(noncanonical_tsv, ["a", "b"]),
            "noncanonical TSV line endings were accepted",
        )
        plan_sha256 = hashlib.sha256(plan_path.read_bytes()).hexdigest()
        output = root / "published"
        local = root / "local"
        output.mkdir()
        local.mkdir()
        (output / "shards").mkdir()
        (output / ".staging").mkdir()
        artifact = root / "artifact"
        populate_probe_artifact(artifact, shard)
        success = publish_shard(
            artifact,
            output,
            plan_sha256,
            shard,
            1.25,
            123456,
            "require",
            4096,
        )
        final = output / "shards" / str(shard["shard_id"])
        require(
            verify_published_shard(final, plan_sha256, shard, 4096) == success,
            "published shard did not verify",
        )
        require(
            publish_shard(
                artifact,
                output,
                plan_sha256,
                shard,
                9.0,
                999999,
                "require",
                4096,
            )
            == success,
            "existing successful shard was republished",
        )

        copy_source = root / "copy-fsync-source"
        copy_source.mkdir()
        (copy_source / "one.bin").write_bytes(b"one")
        (copy_source / "nested").mkdir()
        (copy_source / "nested" / "two.bin").write_bytes(b"two")
        copy_destination = root / "copy-fsync-destination"
        original_os_fsync = campaign.os.fsync
        copied_file_fsyncs = 0

        def fsync_spy(descriptor: int) -> None:
            nonlocal copied_file_fsyncs
            copied_file_fsyncs += 1
            original_os_fsync(descriptor)

        campaign.os.fsync = fsync_spy
        try:
            campaign._copy_artifact(copy_source, copy_destination)
        finally:
            campaign.os.fsync = original_os_fsync
        require(
            copied_file_fsyncs == 2,
            "artifact copy did not fsync every copied payload file",
        )

        ordered_output = root / "ordered-publication"
        ordered_output.mkdir()
        (ordered_output / "shards").mkdir()
        (ordered_output / ".staging").mkdir()
        ordered_artifact = root / "ordered-artifact"
        populate_probe_artifact(ordered_artifact, shard)
        original_copy = campaign._copy_artifact
        original_atomic = campaign.atomic_write
        original_rename = campaign.os.rename
        copied_and_synced = False

        def ordered_copy(source: Path, destination: Path) -> None:
            nonlocal copied_and_synced
            original_copy(source, destination)
            copied_and_synced = True

        def ordered_atomic(path: Path, payload: bytes, policy: str) -> str:
            if path.name == "SUCCESS.json":
                require(
                    copied_and_synced,
                    "SUCCESS was written before copied payload fsyncs completed",
                )
            return original_atomic(path, payload, policy)

        def ordered_rename(source, destination) -> None:
            if str(source).endswith(".partial"):
                require(
                    copied_and_synced,
                    "shard was renamed before copied payload fsyncs completed",
                )
            original_rename(source, destination)

        campaign._copy_artifact = ordered_copy
        campaign.atomic_write = ordered_atomic
        campaign.os.rename = ordered_rename
        try:
            publish_shard(
                ordered_artifact,
                ordered_output,
                plan_sha256,
                shard,
                1.0,
                123456,
                "require",
                4096,
            )
        finally:
            campaign._copy_artifact = original_copy
            campaign.atomic_write = original_atomic
            campaign.os.rename = original_rename

        original_authority_check = campaign.verify_authoritative_plan
        campaign.verify_authoritative_plan = lambda _plan, **_kwargs: None
        first_report = run_campaign(run_options(plan_path, output, local))
        second_report = run_campaign(run_options(plan_path, output, local))
        require(first_report == second_report, "completed resume changed its report")
        require(
            verify_campaign(verify_options(plan_path, output)) == first_report,
            "completed campaign did not verify",
        )
        expect_failure(
            lambda: run_campaign(
                run_options(
                    plan_path,
                    output,
                    local,
                    directory_sync_policy="record-unsupported",
                )
            ),
            "resume under a different directory-sync policy was accepted",
        )

        extra = final / "unrelated.txt"
        extra.write_text("not in manifest\n", encoding="utf-8")
        expect_failure(
            lambda: verify_published_shard(final, plan_sha256, shard, 4096),
            "unrelated artifact file survived resume verification",
        )
        extra.unlink()

        payload = final / "payload.txt"
        payload_bytes = payload.read_bytes()
        payload.unlink()
        os.symlink("artifact-manifest.json", payload)
        expect_failure(
            lambda: verify_published_shard(final, plan_sha256, shard, 4096),
            "artifact symlink substitution survived resume verification",
        )
        payload.unlink()
        payload.write_bytes(payload_bytes)

        empty = final / "unexpected-empty-directory"
        empty.mkdir()
        expect_failure(
            lambda: verify_published_shard(final, plan_sha256, shard, 4096),
            "unexpected empty artifact directory survived exact-set verification",
        )
        empty.rmdir()

        success_path = final / "SUCCESS.json"
        success_bytes = success_path.read_bytes()
        for name, replacement in (
            ("artifact_bytes", True),
            ("artifact_file_count", -1),
            ("elapsed_seconds", 0),
            ("peak_rss_kib", -1),
            ("directory_sync_policy", "unknown-policy"),
        ):
            changed_success = dict(campaign.read_canonical_json(success_path))
            changed_success[name] = replacement
            write_canonical(success_path, changed_success)
            expect_failure(
                lambda: verify_published_shard(
                    final, plan_sha256, shard, 4096
                ),
                f"invalid SUCCESS scalar/policy {name} was accepted",
            )
            success_path.write_bytes(success_bytes)

        expect_failure(
            lambda: canonical_json({"nonfinite": float("nan")}),
            "canonical JSON accepted NaN",
        )
        expect_failure(
            lambda: canonical_json({"nonfinite": float("inf")}),
            "canonical JSON accepted Infinity",
        )
        nonfinite_success = success_bytes.decode("utf-8").replace(
            '"elapsed_seconds":1.25', '"elapsed_seconds":NaN'
        )
        require(
            nonfinite_success.encode("utf-8") != success_bytes,
            "SUCCESS fixture elapsed token was not found",
        )
        success_path.write_text(nonfinite_success, encoding="utf-8")
        expect_failure(
            lambda: verify_published_shard(final, plan_sha256, shard, 4096),
            "non-standard NaN SUCCESS JSON was accepted",
        )
        success_path.write_bytes(success_bytes)

        result_path = final / "aslref-result.json"
        result_bytes = result_path.read_bytes()
        manifest_bytes = (final / "artifact-manifest.json").read_bytes()
        changed_result = json.loads(result_bytes)
        changed_result["passed"] = False
        result_path.write_text(
            json.dumps(changed_result, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        reseal_published_shard(final)
        expect_failure(
            lambda: verify_published_shard(final, plan_sha256, shard, 4096),
            "mutated and fully resealed ASLRef result was accepted",
        )
        result_path.write_bytes(result_bytes)
        (final / "artifact-manifest.json").write_bytes(manifest_bytes)
        success_path.write_bytes(success_bytes)

        report_file = final / "batch-report.json"
        report_bytes = report_file.read_bytes()
        probe_stdout = final / "campaign-probe.stdout"
        probe_stdout_bytes = probe_stdout.read_bytes()
        changed_report = dict(campaign.read_canonical_json(report_file))
        changed_workflow = dict(changed_report["campaign_workflow"])
        changed_results = list(changed_workflow["input_results"])
        changed_results[0] = {
            "input_contract_id": "input-contract-" + "f" * 64,
            "official_asl_result": "official-asl-passed",
        }
        changed_workflow["input_results"] = changed_results
        changed_report["campaign_workflow"] = changed_workflow
        write_canonical(report_file, changed_report)
        probe_stdout.write_text(
            canonical_json(changed_report) + "\n", encoding="utf-8"
        )
        reseal_published_shard(final)
        expect_failure(
            lambda: verify_published_shard(final, plan_sha256, shard, 4096),
            "same-count replacement in fully resealed per-input results was accepted",
        )
        report_file.write_bytes(report_bytes)
        probe_stdout.write_bytes(probe_stdout_bytes)
        (final / "artifact-manifest.json").write_bytes(manifest_bytes)
        success_path.write_bytes(success_bytes)

        held = output / "shards" / "held"
        os.rename(final, held)
        expect_failure(
            lambda: verify_campaign(verify_options(plan_path, output)),
            "missing planned shard directory was accepted",
        )
        os.rename(held, final)
        extra_shard = output / "shards" / "shard-99999999"
        extra_shard.mkdir()
        expect_failure(
            lambda: verify_campaign(verify_options(plan_path, output)),
            "unrelated shard directory was accepted",
        )
        extra_shard.rmdir()

        stale = output / ".staging" / (
            "shard-00000000." + "a" * 32 + ".partial"
        )
        stale.mkdir()
        (stale / "partial").write_text("interrupted", encoding="utf-8")
        expect_failure(
            lambda: verify_campaign(verify_options(plan_path, output)),
            "nonempty interrupted staging was accepted as complete",
        )
        require(
            run_campaign(run_options(plan_path, output, local)) == first_report
            and not any((output / ".staging").iterdir()),
            "resume did not clear its bounded tool-owned staging directory",
        )

        report_path = output / "campaign-report.json"
        report_bytes = report_path.read_bytes()
        changed = dict(first_report)
        changed["failures"] = 1
        write_canonical(report_path, changed)
        expect_failure(
            lambda: verify_campaign(verify_options(plan_path, output)),
            "campaign report failure-count mutation was accepted",
        )
        report_path.write_bytes(report_bytes)

        for name, replacement in (
            ("workers", True),
            ("preexisting_shard_count", True),
            ("invocation_elapsed_seconds", 0),
            ("published_artifact_bytes", -1),
        ):
            changed = dict(first_report)
            changed[name] = replacement
            write_canonical(report_path, changed)
            expect_failure(
                lambda: verify_campaign(verify_options(plan_path, output)),
                f"invalid campaign report scalar {name} was accepted",
            )
            report_path.write_bytes(report_bytes)

        changed = dict(first_report)
        changed["preexisting_shard_ids_sha256"] = "f" * 64
        write_canonical(report_path, changed)
        expect_failure(
            lambda: verify_campaign(verify_options(plan_path, output)),
            "unbound invocation-accounting shard digest was accepted",
        )
        report_path.write_bytes(report_bytes)

        noise = output / "unrelated-root-entry"
        noise.write_text("noise", encoding="utf-8")
        expect_failure(
            lambda: verify_campaign(verify_options(plan_path, output)),
            "unrelated campaign-root entry was accepted",
        )
        noise.unlink()

        lock_path = output / "campaign.lock"
        lock_mode = lock_path.stat().st_mode & 0o777
        lock_path.chmod(0o400)
        try:
            require(
                verify_campaign(verify_options(plan_path, output))
                == first_report,
                "read-only campaign lock prevented verification",
            )
        finally:
            lock_path.chmod(lock_mode)

        missing_lock = output / "campaign.lock.saved"
        os.rename(lock_path, missing_lock)
        expect_failure(
            lambda: verify_campaign(verify_options(plan_path, output)),
            "completed verification recreated/accepted a missing campaign lock",
        )
        require(not lock_path.exists(), "verification recreated a missing lock")
        os.rename(missing_lock, lock_path)

        with campaign_lock(output, "require"):
            expect_failure(
                lambda: run_campaign(run_options(plan_path, output, local)),
                "concurrent campaign publisher acquired the same output root",
            )
        expect_failure(
            lambda: run_campaign(run_options(plan_path, output, local, workers=5)),
            "five-worker campaign passed without concurrency evidence",
        )
        expect_failure(
            lambda: require_root(Path("relative-output"), "output root"),
            "relative output root was accepted",
        )
        campaign.verify_authoritative_plan = original_authority_check

        unsupported_output = root / "published-unsupported-sync"
        unsupported_output.mkdir()
        (unsupported_output / "shards").mkdir()
        (unsupported_output / ".staging").mkdir()
        unsupported_artifact = root / "artifact-unsupported-sync"
        populate_probe_artifact(unsupported_artifact, shard)
        original_fsync = campaign._fsync_directory

        def unsupported_sync(_path: Path, policy: str) -> str:
            return (
                "supported"
                if policy == "require"
                else "unsupported-errno-45"
            )

        campaign._fsync_directory = unsupported_sync
        try:
            unsupported_success = publish_shard(
                unsupported_artifact,
                unsupported_output,
                plan_sha256,
                shard,
                1.25,
                123456,
                "record-unsupported",
                4096,
            )
        finally:
            campaign._fsync_directory = original_fsync
        require(
            unsupported_success["directory_sync_outcomes"]
            == {
                "artifact_stage": "unsupported-errno-45",
                "campaign_root": "unsupported-errno-45",
                "shards_parent": "unsupported-errno-45",
            },
            "unsupported directory-sync outcome was not preserved",
        )
        require(
            verify_published_shard(
                unsupported_output / "shards" / str(shard["shard_id"]),
                plan_sha256,
                shard,
                4096,
            )
            == unsupported_success,
            "recorded unsupported directory-sync shard did not verify",
        )
        expect_failure(
            lambda: publish_shard(
                unsupported_artifact,
                unsupported_output,
                plan_sha256,
                shard,
                1.25,
                123456,
                "require",
                4096,
            ),
            "require resume accepted a previously unsupported durability record",
        )

    print("A64 ASLRef campaign publication/resume fixtures passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
