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
    manifest = root / "aggregate-manifest.tsv"
    manifest.write_text("fixture manifest\n", encoding="utf-8")
    tooling_root = root / "probe-tools"
    tooling_root.mkdir()
    external_probe = tooling_root / "external-probe.py"
    external_probe.write_text("# external probe\n", encoding="utf-8")
    architecture_root = root / "architecture"
    closure_root = root / "closure-captures"
    for directory in (architecture_root, closure_root):
        directory.mkdir()
        (directory / "fixture.txt").write_text(
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
        "input": {"path": input_identity["path"]},
        "contract_count": count,
        "input_contract_ids_sha256": input_digest,
    }
    absolute = str(external_probe.resolve())
    plan = {
        "schema": PLAN_SCHEMA,
        "campaign_id": "publication-fixture",
        "guest_page_size": 4096,
        "shard_contract_cap": MAX_SHARD_CONTRACTS,
        "total_contracts": 1,
        "authoritative_input": {
            "schema": "uml.a64.aslref-authoritative-input/v1",
            "guest_page_size": 4096,
            "aggregate_manifest": file_identity(manifest),
            "materialization_index": file_identity(manifest),
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
        "semantic_coverage_promotion": "requires-exact-join",
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


def run_options(
    plan: Path,
    output: Path,
    local: Path,
    workers: int = 1,
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
            "campaign probe did not receive its configured artifact file cap",
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
            lambda value: value["authoritative_input"].__setitem__(
                "eligible_aggregate_count", True
            ),
            lambda value: value["probe"].__setitem__("timeout_seconds", True),
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
        noncanonical_tsv = root / "noncanonical.tsv"
        noncanonical_tsv.write_bytes(b"a\tb\r\n1\t2\r\n")
        expect_failure(
            lambda: campaign.read_tsv(noncanonical_tsv, ["a", "b"]),
            "noncanonical TSV line endings were accepted",
        )
        campaign_id = str(plan["campaign_id"])
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
            campaign_id,
            shard,
            1.25,
            123456,
            4096,
        )
        final = output / "shards" / str(shard["shard_id"])
        require(
            verify_published_shard(final, campaign_id, shard, 4096) == success,
            "published shard did not verify",
        )
        require(
            publish_shard(
                artifact,
                output,
                campaign_id,
                shard,
                9.0,
                999999,
                4096,
            )
            == success,
            "existing successful shard was republished",
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
        copied = False

        def ordered_copy(source: Path, destination: Path) -> None:
            nonlocal copied
            original_copy(source, destination)
            copied = True

        def ordered_atomic(path: Path, payload: bytes) -> None:
            if path.name == "SUCCESS.json":
                require(
                    copied,
                    "SUCCESS was written before copied payloads completed",
                )
            original_atomic(path, payload)

        def ordered_rename(source, destination) -> None:
            if str(source).endswith(".partial"):
                require(
                    copied,
                    "shard was renamed before copied payloads completed",
                )
            original_rename(source, destination)

        campaign._copy_artifact = ordered_copy
        campaign.atomic_write = ordered_atomic
        campaign.os.rename = ordered_rename
        try:
            publish_shard(
                ordered_artifact,
                ordered_output,
                campaign_id,
                shard,
                1.0,
                123456,
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
        extra = final / "unrelated.txt"
        extra.write_text("not in manifest\n", encoding="utf-8")
        require(
            verify_published_shard(final, campaign_id, shard, 4096)[
                "artifact_file_count"
            ] == success["artifact_file_count"] + 1,
            "ordinary extra artifact was not included in resource accounting",
        )
        extra.unlink()

        payload = final / "payload.txt"
        payload_bytes = payload.read_bytes()
        payload.unlink()
        os.symlink("batch-report.json", payload)
        expect_failure(
            lambda: verify_published_shard(final, campaign_id, shard, 4096),
            "artifact symlink substitution survived resume verification",
        )
        payload.unlink()
        payload.write_bytes(payload_bytes)

        empty = final / "unexpected-empty-directory"
        empty.mkdir()
        verify_published_shard(final, campaign_id, shard, 4096)
        empty.rmdir()

        success_path = final / "SUCCESS.json"
        success_bytes = success_path.read_bytes()
        for name, replacement in (
            ("elapsed_seconds", 0),
            ("peak_rss_kib", -1),
        ):
            changed_success = dict(campaign.read_canonical_json(success_path))
            changed_success[name] = replacement
            write_canonical(success_path, changed_success)
            expect_failure(
                lambda: verify_published_shard(
                    final, campaign_id, shard, 4096
                ),
                f"invalid SUCCESS scalar {name} was accepted",
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
            lambda: verify_published_shard(final, campaign_id, shard, 4096),
            "non-standard NaN SUCCESS JSON was accepted",
        )
        success_path.write_bytes(success_bytes)

        result_path = final / "aslref-result.json"
        result_bytes = result_path.read_bytes()
        changed_result = json.loads(result_bytes)
        changed_result["passed"] = False
        result_path.write_text(
            json.dumps(changed_result, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        expect_failure(
            lambda: verify_published_shard(final, campaign_id, shard, 4096),
            "semantically invalid ASLRef result was accepted after metadata refresh",
        )
        result_path.write_bytes(result_bytes)
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
        expect_failure(
            lambda: verify_published_shard(final, campaign_id, shard, 4096),
            "same-count invalid per-input replacement was accepted after metadata refresh",
        )
        report_file.write_bytes(report_bytes)
        probe_stdout.write_bytes(probe_stdout_bytes)
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

        with campaign_lock(output):
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

    print("A64 ASLRef campaign publication/resume fixtures passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
