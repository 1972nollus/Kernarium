#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Bounded pilot and honest full-resume accounting fixtures."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import copy
import hashlib
import os
import shutil
import sys
import tempfile
import threading
import time
from pathlib import Path

import run_a64_aslref_campaign as campaign
from test_a64_aslref_campaign import (
    expect_failure,
    fixture_plan,
    require,
    run_options,
    write_canonical,
)


def full_plan(root: Path) -> tuple[Path, dict[str, object]]:
    _base_path, plan, _base_shard = fixture_plan(root)
    shards = []
    for index in range(5):
        source = root / f"pilot-shard-{index:08d}.inputs.jsonl.gz"
        source.write_bytes(f"pilot-input-{index}\n".encode())
        identity = campaign.file_identity(source)
        shards.append(
            {
                "shard_id": f"shard-{index:08d}",
                "input": {
                    "path": identity["path"],
                    "size": identity["size"],
                    "sha256": identity["sha256"],
                },
                "contract_count": campaign.MAX_SHARD_CONTRACTS,
                "input_contract_ids_sha256": hashlib.sha256(
                    f"pilot-contracts-{index}".encode()
                ).hexdigest(),
            }
        )
    plan["shards"] = shards
    plan["total_contracts"] = 5 * campaign.MAX_SHARD_CONTRACTS
    plan["authoritative_input"]["input_contract_count"] = plan[
        "total_contracts"
    ]
    campaign.validate_plan(plan)
    path = root / "full-campaign-plan.json"
    write_canonical(path, plan)
    return path, plan


def fake_success(
    plan_sha256: str,
    shard: dict[str, object],
    policy: str,
) -> dict[str, object]:
    digest = hashlib.sha256(str(shard["shard_id"]).encode()).hexdigest()
    return {
        "schema": campaign.SUCCESS_SCHEMA,
        "campaign_plan_sha256": plan_sha256,
        "shard_id": shard["shard_id"],
        "contract_count": shard["contract_count"],
        "input_contract_ids_sha256": shard["input_contract_ids_sha256"],
        "artifact_manifest_sha256": digest,
        "artifact_file_count": 1,
        "artifact_bytes": 1,
        "elapsed_seconds": 1.0,
        "peak_rss_kib": 1,
        "directory_sync_policy": policy,
        "directory_sync_outcomes": {
            "artifact_stage": "supported",
            "campaign_root": "supported",
            "shards_parent": "supported",
        },
        "probe_report_sha256": digest,
        "probe_result_sha256": digest,
        "input_results_sha256": digest,
    }


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="a64-campaign-pilot-") as temporary:
        root = Path(temporary).resolve(strict=True)
        plan_path, plan = full_plan(root)
        output = root / "output"
        local = root / "local"
        output.mkdir()
        local.mkdir()

        original_authority = campaign.verify_authoritative_plan
        original_run_one = campaign.run_one_shard
        original_verify_shard = campaign.verify_published_shard
        original_capacity = campaign.require_campaign_capacity
        original_free_space = campaign.require_free_space
        original_disk_usage = campaign.shutil.disk_usage
        original_wait = campaign.concurrent.futures.wait

        def fake_run_one(
            _plan,
            plan_sha256,
            shard,
            output_root,
            _local_temp_root,
            policy,
            _cancel_event=None,
        ):
            final = output_root / "shards" / str(shard["shard_id"])
            if not final.exists():
                final.mkdir()
                write_canonical(
                    final / "SUCCESS.json",
                    fake_success(plan_sha256, shard, policy),
                )
            return campaign.read_canonical_json(final / "SUCCESS.json")

        def fake_verify(directory, plan_sha256, shard, _guest_page_size):
            success = campaign.read_canonical_json(directory / "SUCCESS.json")
            if (
                success["campaign_plan_sha256"] != plan_sha256
                or success["shard_id"] != shard["shard_id"]
                or success["contract_count"] != shard["contract_count"]
            ):
                raise ValueError("fake pilot shard binding differs")
            return success

        campaign.verify_authoritative_plan = lambda _plan, **_kwargs: None
        campaign.run_one_shard = fake_run_one
        campaign.verify_published_shard = fake_verify
        try:
            unsafe_layout = root / "unsafe-layout-output"
            unsafe_layout.mkdir()
            (unsafe_layout / "shards").write_text(
                "not a directory\n", encoding="utf-8"
            )
            expect_failure(
                lambda: campaign.run_campaign(
                    run_options(plan_path, unsafe_layout, local, workers=1)
                ),
                "campaign accepted an unsafe reserved output target",
            )
            require(
                not (unsafe_layout / "campaign.lock").exists(),
                "campaign locked/mutated an unsafe output before preflight",
            )

            source_tree = Path(str(plan["probe"]["architecture_root"]))
            nested_output = source_tree / "nested-output"
            nested_local = source_tree / "nested-local"
            nested_output.mkdir()
            nested_local.mkdir()
            expect_failure(
                lambda: campaign.run_campaign(
                    run_options(plan_path, nested_output, local, workers=1)
                ),
                "campaign output inside a frozen source tree was accepted",
            )
            expect_failure(
                lambda: campaign.run_campaign(
                    run_options(plan_path, output, nested_local, workers=1)
                ),
                "campaign local root inside a frozen source tree was accepted",
            )
            require(
                not (nested_output / "campaign.lock").exists(),
                "source-tree output was mutated before isolation rejection",
            )

            boundary_required = 2 * 1024 * 1024 * 1024 + 12345
            campaign.shutil.disk_usage = lambda _path: type(
                "DiskUsage", (), {"free": boundary_required}
            )()
            campaign.require_free_space(
                output, 2.0, "boundary", headroom_bytes=12345
            )
            campaign.shutil.disk_usage = lambda _path: type(
                "DiskUsage", (), {"free": boundary_required - 1}
            )()
            expect_failure(
                lambda: campaign.require_free_space(
                    output, 2.0, "boundary", headroom_bytes=12345
                ),
                "campaign free-space boundary accepted one byte too little",
            )
            campaign.shutil.disk_usage = original_disk_usage

            exact_process = root / "exact-process"
            exact_process.mkdir()
            exact_returncode = campaign._run_process_with_capped_logs(
                [
                    sys.executable,
                    "-B",
                    "-c",
                    "import sys; sys.stdout.write('x' * 1024)",
                ],
                exact_process / "probe.stdout",
                exact_process / "probe.stderr",
                {},
                10,
                1024,
                None,
            )
            require(
                exact_returncode == 0
                and sum(
                    path.stat().st_size
                    for path in exact_process.iterdir()
                    if path.is_file()
                )
                == 1024,
                "campaign probe rejected its exact combined log boundary",
            )

            capped_process = root / "capped-process"
            capped_process.mkdir()
            capped_stdout = capped_process / "probe.stdout"
            capped_stderr = capped_process / "probe.stderr"
            expect_failure(
                lambda: campaign._run_process_with_capped_logs(
                    [
                        sys.executable,
                        "-B",
                        "-c",
                        "import sys; sys.stdout.write('x' * 8192)",
                    ],
                    capped_stdout,
                    capped_stderr,
                    {},
                    10,
                    1024,
                    None,
                ),
                "campaign probe capture accepted excess log bytes",
            )
            require(
                sum(
                    path.stat().st_size
                    for path in (capped_stdout, capped_stderr)
                    if path.exists()
                ) <= 1024,
                "campaign probe capture wrote beyond its combined hard cap",
            )
            early_process = root / "early-process"
            early_process.mkdir()
            detached_pid_path = early_process / "early-leader-child.pid"
            detached_started = time.monotonic()
            expect_failure(
                lambda: campaign._run_process_with_capped_logs(
                    [
                        sys.executable,
                        "-B",
                        "-c",
                        (
                            "import subprocess,sys;"
                            "p=subprocess.Popen(['/bin/sleep','60']);"
                            "open(sys.argv[1],'w').write(str(p.pid))"
                        ),
                        str(detached_pid_path),
                    ],
                    early_process / "early-leader.stdout",
                    early_process / "early-leader.stderr",
                    {},
                    10,
                    4096,
                    None,
                ),
                "campaign accepted an early leader with live descendants",
            )
            detached_elapsed = time.monotonic() - detached_started
            detached_pid = int(
                detached_pid_path.read_text(encoding="utf-8")
            )
            detached_deadline = time.monotonic() + 2
            detached_alive = True
            while detached_alive and time.monotonic() < detached_deadline:
                try:
                    os.kill(detached_pid, 0)
                except ProcessLookupError:
                    detached_alive = False
                    break
                time.sleep(0.025)
            require(
                detached_elapsed < 3 and not detached_alive,
                "early-exiting probe leader left a pipe-owning child alive",
            )

            timeout_process = root / "timeout-process"
            timeout_process.mkdir()
            timeout_escape = root / "timeout-descendant-escaped"
            timeout_started = time.monotonic()
            expect_failure(
                lambda: campaign._run_process_with_capped_logs(
                    [
                        sys.executable,
                        "-B",
                        "-c",
                        (
                            "import subprocess,sys,time;"
                            "subprocess.Popen([sys.executable,'-B','-c',"
                            "sys.argv[1]]);time.sleep(60)"
                        ),
                        (
                            "import time;from pathlib import Path;"
                            f"time.sleep(2);Path({str(timeout_escape)!r})"
                            ".write_text('escaped')"
                        ),
                    ],
                    timeout_process / "probe.stdout",
                    timeout_process / "probe.stderr",
                    {},
                    1,
                    4096,
                    None,
                ),
                "campaign probe and descendant escaped the timeout",
            )
            require(
                time.monotonic() - timeout_started < 3,
                "campaign probe timeout exceeded its bounded grace",
            )
            time.sleep(1.25)
            require(
                not timeout_escape.exists(),
                "campaign timeout left a descendant able to mutate later",
            )

            observed_headroom: list[tuple[str, int]] = []

            def record_headroom(
                _path, _minimum, label, *, headroom_bytes=0
            ):
                observed_headroom.append((label, headroom_bytes))

            campaign.require_free_space = record_headroom
            campaign.require_campaign_capacity(
                plan, output, local, 20.0, 5.0, 3
            )
            campaign.require_free_space = original_free_space
            expected_output_headroom = 3 * campaign.campaign_shard_disk_headroom(
                plan, "output"
            )
            expected_local_headroom = 3 * campaign.campaign_shard_disk_headroom(
                plan, "local"
            )
            require(
                observed_headroom
                == [
                    ("output", expected_output_headroom),
                    ("local", expected_local_headroom),
                ],
                "campaign capacity did not reserve per-active-worker headroom",
            )

            expect_failure(
                lambda: campaign.pilot_campaign(
                    run_options(plan_path, output, local, workers=3)
                ),
                "pilot accepted a non-four-worker invocation",
            )

            short_plan = copy.deepcopy(plan)
            short_plan["shards"][0]["contract_count"] = 1023
            short_plan["total_contracts"] -= 1
            short_plan["authoritative_input"]["input_contract_count"] -= 1
            campaign.validate_plan(short_plan)
            short_path = root / "short-plan.json"
            write_canonical(short_path, short_plan)
            expect_failure(
                lambda: campaign.pilot_campaign(
                    run_options(short_path, output, local, workers=4)
                ),
                "pilot accepted a short first shard",
            )

            interrupted_output = root / "interrupted-output"
            interrupted_local = root / "interrupted-local"
            interrupted_output.mkdir()
            interrupted_local.mkdir()
            capacity_checks: list[int] = []

            def interrupt_after_first_completion(
                checked_plan,
                checked_output,
                checked_local,
                minimum_output,
                minimum_local,
                active_shards,
            ):
                original_capacity(
                    checked_plan,
                    checked_output,
                    checked_local,
                    minimum_output,
                    minimum_local,
                    active_shards,
                )
                capacity_checks.append(active_shards)
                if capacity_checks == [0, 1, 0]:
                    raise ValueError("simulated exhausted campaign reserve")

            campaign.require_campaign_capacity = interrupt_after_first_completion
            expect_failure(
                lambda: campaign.run_campaign(
                    run_options(
                        plan_path,
                        interrupted_output,
                        interrupted_local,
                        workers=1,
                    )
                ),
                "campaign ignored a post-completion reserve failure",
            )
            campaign.require_campaign_capacity = original_capacity
            interrupted_success = sorted(
                (interrupted_output / "shards").glob("*/SUCCESS.json")
            )
            require(
                capacity_checks == [0, 1, 0]
                and len(interrupted_success) == 1
                and not (interrupted_output / "campaign-report.json").exists(),
                "reserve interruption did not stop before reporting success",
            )
            interrupted_before = interrupted_success[0].read_bytes()
            resumed = campaign.run_campaign(
                run_options(
                    plan_path,
                    interrupted_output,
                    interrupted_local,
                    workers=1,
                )
            )
            require(
                resumed["preexisting_shard_count"] == 1
                and resumed["published_this_run_shard_count"] == 4
                and interrupted_success[0].read_bytes() == interrupted_before,
                "low-space resume did not preserve the completed shard exactly",
            )

            batch_output = root / "batch-failure-output"
            batch_local = root / "batch-failure-local"
            batch_output.mkdir()
            batch_local.mkdir()
            batch_started: list[str] = []

            def same_batch_run(
                run_plan,
                plan_sha256,
                shard,
                output_root,
                local_root,
                policy,
                cancel_event=None,
            ):
                name = str(shard["shard_id"])
                batch_started.append(name)
                if name == "shard-00000001":
                    raise ValueError("simulated same-batch failure")
                return fake_run_one(
                    run_plan,
                    plan_sha256,
                    shard,
                    output_root,
                    local_root,
                    policy,
                    cancel_event,
                )

            def complete_batch(futures, return_when=None):
                return original_wait(
                    futures,
                    return_when=campaign.concurrent.futures.ALL_COMPLETED,
                )

            campaign.run_one_shard = same_batch_run
            campaign.concurrent.futures.wait = complete_batch
            expect_failure(
                lambda: campaign.run_campaign(
                    run_options(
                        plan_path, batch_output, batch_local, workers=2
                    )
                ),
                "campaign refilled work after a same-batch shard failure",
            )
            campaign.concurrent.futures.wait = original_wait
            campaign.run_one_shard = fake_run_one
            require(
                sorted(batch_started)
                == ["shard-00000000", "shard-00000001"]
                and not (batch_output / "campaign-report.json").exists(),
                "same-batch failure scheduled a later shard or reported success",
            )
            batch_success = batch_output / "shards/shard-00000000/SUCCESS.json"
            batch_before = batch_success.read_bytes()
            batch_resume = campaign.run_campaign(
                run_options(plan_path, batch_output, batch_local, workers=2)
            )
            require(
                batch_resume["preexisting_shard_count"] == 1
                and batch_success.read_bytes() == batch_before,
                "same-batch failure resume changed its durable success",
            )

            inflight_output = root / "inflight-output"
            inflight_local = root / "inflight-local"
            inflight_output.mkdir()
            inflight_local.mkdir()
            child_pid_path = root / "inflight-child.pid"
            inflight_started: list[str] = []
            rendezvous = threading.Barrier(2)

            def inflight_run(
                run_plan,
                plan_sha256,
                shard,
                output_root,
                local_root,
                policy,
                cancel_event=None,
            ):
                name = str(shard["shard_id"])
                inflight_started.append(name)
                rendezvous.wait(timeout=5)
                if name == "shard-00000000":
                    deadline = time.monotonic() + 5
                    while not child_pid_path.exists():
                        if time.monotonic() >= deadline:
                            raise ValueError("in-flight child did not start")
                        time.sleep(0.01)
                    fake_run_one(
                        run_plan,
                        plan_sha256,
                        shard,
                        output_root,
                        local_root,
                        policy,
                        cancel_event,
                    )
                    raise ValueError("simulated in-flight peer failure")
                owned = Path(
                    tempfile.mkdtemp(
                        prefix=f"{name}.", suffix=".partial", dir=local_root
                    )
                )
                try:
                    code = (
                        "import subprocess,sys,time;"
                        "p=subprocess.Popen(['/bin/sleep','60']);"
                        "open(sys.argv[1],'w').write(str(p.pid));"
                        "time.sleep(60)"
                    )
                    campaign._run_process_with_capped_logs(
                        [sys.executable, "-B", "-c", code, str(child_pid_path)],
                        owned / "stdout",
                        owned / "stderr",
                        {},
                        60,
                        4096,
                        cancel_event,
                    )
                    raise ValueError("in-flight process was not cancelled")
                finally:
                    shutil.rmtree(owned)

            campaign.run_one_shard = inflight_run
            inflight_started_at = time.monotonic()
            expect_failure(
                lambda: campaign.run_campaign(
                    run_options(
                        plan_path, inflight_output, inflight_local, workers=2
                    )
                ),
                "campaign did not interrupt an in-flight peer",
            )
            inflight_elapsed = time.monotonic() - inflight_started_at
            campaign.run_one_shard = fake_run_one
            child_pid = int(child_pid_path.read_text(encoding="utf-8"))
            process_deadline = time.monotonic() + 2
            child_alive = True
            while child_alive and time.monotonic() < process_deadline:
                try:
                    os.kill(child_pid, 0)
                except ProcessLookupError:
                    child_alive = False
                    break
                time.sleep(0.025)
            inflight_success = (
                inflight_output / "shards/shard-00000000/SUCCESS.json"
            )
            inflight_before = inflight_success.read_bytes()
            require(
                sorted(inflight_started)
                == ["shard-00000000", "shard-00000001"]
                and inflight_elapsed < 5
                and not child_alive
                and not list(inflight_local.iterdir())
                and not (inflight_output / "campaign-report.json").exists(),
                "in-flight cancellation was slow or left process/temp debris",
            )
            inflight_resume = campaign.run_campaign(
                run_options(plan_path, inflight_output, inflight_local, workers=2)
            )
            require(
                inflight_resume["preexisting_shard_count"] == 1
                and inflight_success.read_bytes() == inflight_before,
                "in-flight failure resume changed its durable success",
            )

            pilot = campaign.pilot_campaign(
                run_options(plan_path, output, local, workers=4)
            )
            require(
                pilot["shards"] == 4
                and pilot["contracts"] == 4096
                and pilot["campaign_report_published"] is False
                and not (output / "campaign-report.json").exists(),
                "pilot did not remain bounded/nonpromotable",
            )
            success_paths = sorted((output / "shards").glob("*/SUCCESS.json"))
            before = {path: path.read_bytes() for path in success_paths}
            expect_failure(
                lambda: campaign.verify_campaign(
                    type(
                        "Options",
                        (),
                        {
                            "plan": plan_path,
                            "output_root": output,
                            "local_temp_root": local,
                            "minimum_local_free_gib": 0.0,
                            "maximum_authority_database_bytes": 16
                            * 1024
                            * 1024,
                            "maximum_authority_merge_scratch_bytes": 16
                            * 1024
                            * 1024,
                        },
                    )
                ),
                "ordinary verification accepted an incomplete pilot",
            )

            extra = output / "shards" / "shard-99999999"
            extra.mkdir()
            expect_failure(
                lambda: campaign.pilot_campaign(
                    run_options(plan_path, output, local, workers=4)
                ),
                "pilot accepted an extra published shard",
            )
            extra.rmdir()

            (output / "campaign-report.json").write_text("{}\n", encoding="utf-8")
            expect_failure(
                lambda: campaign.pilot_campaign(
                    run_options(plan_path, output, local, workers=4)
                ),
                "pilot accepted/created a campaign report",
            )
            (output / "campaign-report.json").unlink()

            campaign.pilot_campaign(
                run_options(plan_path, output, local, workers=4)
            )
            require(
                all(path.read_bytes() == payload for path, payload in before.items()),
                "pilot resume changed a previously published SUCCESS artifact",
            )

            report = campaign.run_campaign(
                run_options(plan_path, output, local, workers=4)
            )
            require(
                report["preexisting_shard_count"] == 4
                and report["preexisting_contract_count"] == 4096
                and report["published_this_run_shard_count"] == 1
                and report["published_this_run_contract_count"] == 1024,
                "full resume did not account for pilot-preexisting contracts",
            )
            require(
                report["published_this_run_contracts_per_second"]
                == round(1024 / report["invocation_elapsed_seconds"], 6),
                "full resume credited pilot contracts to its invocation throughput",
            )
            require(
                all(path.read_bytes() == payload for path, payload in before.items()),
                "full resume changed a pilot SUCCESS artifact",
            )
        finally:
            campaign.verify_authoritative_plan = original_authority
            campaign.run_one_shard = original_run_one
            campaign.verify_published_shard = original_verify_shard
            campaign.require_campaign_capacity = original_capacity
            campaign.require_free_space = original_free_space
            campaign.shutil.disk_usage = original_disk_usage
            campaign.concurrent.futures.wait = original_wait

    print("A64 ASLRef bounded pilot/full-resume fixtures passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
