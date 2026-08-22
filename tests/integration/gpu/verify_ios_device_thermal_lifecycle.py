#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify thermal and repeated-lifecycle evidence from iOS device CTS matrices."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import sys
from typing import Any, Sequence

import kernarium_vk_gl_cts_common as common
import run_vk_gl_cts as cts


CAMPAIGN_KIND = "uml-vk-gl-cts-signed-apple-matrix"
REPORT_KIND = "uml-ios-device-thermal-lifecycle-verification"
REQUIRED_APIS = frozenset(("gles2", "gles3"))
THERMAL_PREFIX = "Kernarium thermal state:"
THERMAL_RECORD = re.compile(
    r"Kernarium thermal state: "
    r"phase=(start|change|finish) "
    r"state=(nominal|fair|serious|critical|unknown) "
    r"uptime=([0-9]+\.[0-9]{3})"
)
THERMAL_PROBE_PREFIX = "Kernarium thermal probe:"
THERMAL_PROBE_RECORD = re.compile(
    r"Kernarium thermal probe: "
    r"state=(nominal|fair|serious|critical|unknown) "
    r"uptime=([0-9]+\.[0-9]{3})"
)
STATE_SEVERITY = {
    "nominal": 0,
    "fair": 1,
    "serious": 2,
    "critical": 3,
    "unknown": 4,
}


def fail(message: str) -> None:
    raise cts.CampaignError(message)


def regular_file(path: Path, label: str) -> None:
    if path.is_symlink() or not path.is_file():
        fail(f"{label} is not a regular file: {path}")


def load_json(path: Path, label: str) -> tuple[dict[str, Any], str]:
    regular_file(path, label)
    try:
        payload = path.read_bytes()
        document = json.loads(
            payload.decode("utf-8"),
            object_pairs_hook=cts.reject_duplicate_keys,
        )
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        fail(f"cannot read {label} {path}: {error}")
    if not isinstance(document, dict):
        fail(f"{label} is not a JSON object: {path}")
    return document, hashlib.sha256(payload).hexdigest()


def read_console(path: Path) -> tuple[str, str]:
    regular_file(path, "device matrix console")
    try:
        payload = path.read_bytes()
        transcript = payload.decode("utf-8", errors="strict")
    except (OSError, UnicodeError) as error:
        fail(f"cannot read device matrix console {path}: {error}")
    return transcript, hashlib.sha256(payload).hexdigest()


def parse_thermal_samples(transcript: str, label: str) -> dict[str, Any]:
    normalized = common.normalize_mobile_transcript(transcript)
    marker_lines = [
        line[line.index(THERMAL_PREFIX) :].strip()
        for line in normalized.splitlines()
        if THERMAL_PREFIX in line
    ]
    samples: list[tuple[str, str, float]] = []
    for line in marker_lines:
        match = THERMAL_RECORD.fullmatch(line)
        if match is None:
            fail(f"{label} contains a malformed thermal record: {line!r}")
        uptime = float(match.group(3))
        if not math.isfinite(uptime):
            fail(f"{label} contains a non-finite thermal uptime")
        samples.append((match.group(1), match.group(2), uptime))
    if not samples:
        fail(f"{label} contains no thermal records")
    phases = [sample[0] for sample in samples]
    if (
        phases[0] != "start"
        or phases[-1] != "finish"
        or phases.count("start") != 1
        or phases.count("finish") != 1
        or any(phase != "change" for phase in phases[1:-1])
    ):
        fail(f"{label} has an invalid thermal phase lifecycle: {phases!r}")
    uptimes = [sample[2] for sample in samples]
    if any(right < left for left, right in zip(uptimes, uptimes[1:])):
        fail(f"{label} has non-monotonic thermal uptimes")
    if uptimes[-1] <= uptimes[0]:
        fail(f"{label} has a non-positive managed-run duration")
    states = [sample[1] for sample in samples]
    maximum_state = max(states, key=STATE_SEVERITY.__getitem__)
    return {
        "start_state": states[0],
        "finish_state": states[-1],
        "maximum_state": maximum_state,
        "change_count": len(samples) - 2,
        "sample_count": len(samples),
        "duration_seconds": round(uptimes[-1] - uptimes[0], 3),
        "state_totals": dict(sorted(Counter(states).items())),
    }


def validate_thermal_preflights(
    run: Path,
    metadata: dict[str, Any],
    shard: str,
) -> list[dict[str, Any]]:
    records = metadata.get("thermal_preflights")
    if not isinstance(records, list) or not records:
        fail(f"device matrix shard {shard} omits its thermal preflight")
    validated: list[dict[str, Any]] = []
    for index, record in enumerate(records, start=1):
        expected_log = f"thermal-preflight-{index:03}.log"
        if (
            not isinstance(record, dict)
            or set(record)
            != {
                "state",
                "uptime",
                "devicectl_return_code",
                "log",
                "log_sha256",
            }
            or record.get("log") != expected_log
            or record.get("state") not in STATE_SEVERITY
            or not isinstance(record.get("uptime"), (int, float))
            or not isinstance(record.get("devicectl_return_code"), int)
        ):
            fail(f"device matrix shard {shard} has invalid thermal preflight")
        log_path = run / expected_log
        transcript, log_sha256 = read_console(log_path)
        matches = list(THERMAL_PROBE_RECORD.finditer(transcript))
        if (
            transcript.count(THERMAL_PROBE_PREFIX) != 1
            or len(matches) != 1
            or record["state"] != matches[0].group(1)
            or record["uptime"] != float(matches[0].group(2))
            or record["log_sha256"] != log_sha256
        ):
            fail(f"device matrix shard {shard} thermal preflight changed")
        validated.append(record)
    if validated[-1]["state"] != "nominal":
        fail(f"device matrix shard {shard} did not cool to nominal")
    return validated


def safe_adapter_run(
    campaign: Path,
    shard: str,
    attempt: Path,
    value: Any,
) -> Path:
    if not isinstance(value, str) or not value:
        fail(f"device matrix shard {shard} omits its adapter run")
    relative = Path(value)
    if relative.is_absolute() or ".." in relative.parts:
        fail(f"device matrix shard {shard} has an unsafe adapter-run path")
    expected_root = attempt / "adapter-output"
    run = campaign / relative
    if run.parent != expected_root or run.is_symlink() or not run.is_dir():
        fail(f"device matrix shard {shard} names an invalid adapter run")
    try:
        entries = list(expected_root.iterdir())
    except OSError as error:
        fail(f"cannot inspect adapter output for {shard}: {error}")
    if entries != [run] and set(entries) != {run}:
        fail(f"device matrix shard {shard} has extra adapter runs")
    return run


def validate_attempt(
    campaign: Path,
    shard: str,
    execution: dict[str, Any],
    app: dict[str, Any],
    rootfs: dict[str, Any],
    expected_memory_mib: int,
) -> dict[str, Any]:
    attempt = campaign / "shards" / shard / "attempt"
    if attempt.is_symlink() or not attempt.is_dir():
        fail(f"device matrix shard {shard} has no regular attempt directory")
    attempt_path = attempt / "attempt.json"
    runner_log = attempt / "runner.log"
    metadata, attempt_sha256 = load_json(
        attempt_path, "device matrix attempt metadata"
    )
    regular_file(runner_log, "device matrix attempt log")
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
    if (
        set(metadata) != expected_keys
        or metadata.get("schema_version") != 1
        or metadata.get("shard") != shard
        or metadata.get("return_code") != 0
        or metadata.get("runner_log_sha256") != cts.sha256_file(runner_log)
    ):
        fail(f"device matrix shard {shard} has invalid attempt evidence")
    run = safe_adapter_run(
        campaign, shard, attempt, metadata.get("adapter_run")
    )
    run_path = run / "run.json"
    run_metadata, run_sha256 = load_json(run_path, "device matrix run metadata")
    device = run_metadata.get("device")
    required = {
        "adapter": "ios-device-kernarium",
        "backend": "angle-metal",
        "campaign": shard,
        "page_profile": execution.get("page_profile"),
        "guest_memory_mib": expected_memory_mib,
        "qualification": True,
        "runtime_return_code": 0,
        "rootfs_sha256": rootfs.get("sha256"),
        "app": app.get("path"),
    }
    if any(run_metadata.get(key) != value for key, value in required.items()):
        fail(f"device matrix shard {shard} changed its signed run identity")
    if (
        not isinstance(device, dict)
        or device.get("identifier") != execution.get("device")
        or device.get("transport") != "wired"
    ):
        fail(f"device matrix shard {shard} did not run on the wired device")
    thermal_preflights = validate_thermal_preflights(run, run_metadata, shard)
    console_path = run / "console.log"
    transcript, console_sha256 = read_console(console_path)
    common.validate_clean_shutdown(transcript)
    thermal = parse_thermal_samples(transcript, f"device matrix shard {shard}")
    return {
        "shard": shard,
        "attempt": str(attempt.relative_to(campaign)),
        "attempt_sha256": attempt_sha256,
        "runner_log_sha256": metadata["runner_log_sha256"],
        "run": str(run.relative_to(campaign)),
        "run_sha256": run_sha256,
        "console_sha256": console_sha256,
        "thermal_preflights": thermal_preflights,
        "thermal": thermal,
    }


def validate_campaign(
    campaign: Path,
    expected_memory_mib: int,
) -> dict[str, Any]:
    campaign = campaign.absolute()
    if campaign.is_symlink() or not campaign.is_dir():
        fail(f"device matrix campaign is not a regular directory: {campaign}")
    campaign_path = campaign / "campaign.json"
    document, campaign_sha256 = load_json(
        campaign_path, "device matrix campaign metadata"
    )
    spec = document.get("spec")
    progress = document.get("progress")
    aggregate = document.get("aggregate")
    if (
        document.get("schema_version") != 1
        or document.get("kind") != CAMPAIGN_KIND
        or document.get("state") != "passed"
        or document.get("failures") != []
        or not isinstance(spec, dict)
        or not isinstance(progress, dict)
        or not isinstance(aggregate, dict)
        or aggregate.get("complete") is not True
        or aggregate.get("unwaived_runner_success") is not True
    ):
        fail(f"device matrix campaign has not passed cleanly: {campaign}")
    execution = spec.get("execution")
    app = spec.get("app")
    rootfs = spec.get("rootfs")
    signed_matrix = spec.get("signed_matrix")
    expected = spec.get("expected")
    if not all(
        isinstance(value, dict)
        for value in (execution, app, rootfs, signed_matrix, expected)
    ):
        fail(f"device matrix campaign has an incomplete specification: {campaign}")
    assert isinstance(execution, dict)
    assert isinstance(app, dict)
    assert isinstance(rootfs, dict)
    assert isinstance(signed_matrix, dict)
    assert isinstance(expected, dict)
    api = spec.get("api")
    if (
        api not in REQUIRED_APIS
        or execution.get("platform") != "device"
        or execution.get("adapter") != "ios-device-kernarium"
        or execution.get("backend") != "angle-metal"
        or execution.get("engine") != "gadgets"
        or execution.get("guest_memory_mib") != expected_memory_mib
        or execution.get("page_profile") != "16k"
    ):
        fail(f"device matrix campaign has the wrong execution policy: {campaign}")
    shards = signed_matrix.get("shards")
    if (
        not isinstance(shards, list)
        or not shards
        or any(not isinstance(shard, str) or not shard for shard in shards)
        or len(shards) != len(set(shards))
    ):
        fail(f"device matrix campaign has an invalid shard inventory: {campaign}")
    shard_count = len(shards)
    expected_progress = {
        "completed_shards": shard_count,
        "failed_shards": 0,
        "pending_shards": 0,
        "total_shards": shard_count,
    }
    if (
        progress != expected_progress
        or expected.get("shard_count") != shard_count
        or aggregate.get("shard_count") != shard_count
    ):
        fail(f"device matrix campaign has incomplete shard accounting: {campaign}")
    attempts = [
        validate_attempt(
            campaign,
            shard,
            execution,
            app,
            rootfs,
            expected_memory_mib,
        )
        for shard in shards
    ]
    return {
        "path": str(campaign),
        "campaign_sha256": campaign_sha256,
        "api": api,
        "device": execution["device"],
        "page_profile": execution["page_profile"],
        "guest_memory_mib": execution["guest_memory_mib"],
        "app": app,
        "rootfs": rootfs,
        "launch_count": shard_count,
        "attempts": attempts,
    }


def verify_campaigns(
    campaigns: Sequence[Path],
    expected_memory_mib: int = common.DEVICE_DEFAULT_GUEST_MEMORY_MIB,
    minimum_launches: int = 100,
) -> dict[str, Any]:
    if minimum_launches <= 0:
        fail("minimum lifecycle launch count must be positive")
    resolved = [campaign.absolute() for campaign in campaigns]
    if len(resolved) != len(set(resolved)):
        fail("device thermal qualification repeats a campaign directory")
    reports = [
        validate_campaign(campaign, expected_memory_mib)
        for campaign in resolved
    ]
    apis = [report["api"] for report in reports]
    if set(apis) != REQUIRED_APIS or len(apis) != len(REQUIRED_APIS):
        fail("device thermal qualification requires one GLES2 and one GLES3 matrix")
    first = reports[0]
    for report in reports[1:]:
        for key in ("device", "page_profile", "guest_memory_mib", "app", "rootfs"):
            if report[key] != first[key]:
                fail(f"device thermal campaigns disagree on {key}")
    attempts = [attempt for report in reports for attempt in report["attempts"]]
    if len(attempts) < minimum_launches:
        fail(
            f"device thermal qualification has {len(attempts)} launches; "
            f"at least {minimum_launches} are required"
        )
    state_totals: Counter[str] = Counter()
    for attempt in attempts:
        state_totals.update(attempt["thermal"]["state_totals"])
    durations = [attempt["thermal"]["duration_seconds"] for attempt in attempts]
    return {
        "schema_version": 1,
        "kind": REPORT_KIND,
        "created_at": datetime.now(timezone.utc).isoformat(),
        "verifier": {
            "path": str(Path(__file__).resolve()),
            "sha256": cts.sha256_file(Path(__file__).resolve()),
        },
        "policy": {
            "required_apis": sorted(REQUIRED_APIS),
            "minimum_launches": minimum_launches,
            "guest_memory_mib": expected_memory_mib,
            "page_profile": "16k",
            "required_preflight_state": "nominal",
            "thermal_state_changes_affect_result": False,
            "clean_shutdown_required": True,
        },
        "result": {
            "passed": True,
            "launch_count": len(attempts),
            "sample_count": sum(state_totals.values()),
            "state_totals": dict(sorted(state_totals.items())),
            "minimum_duration_seconds": min(durations),
            "maximum_duration_seconds": max(durations),
            "total_duration_seconds": round(sum(durations), 3),
        },
        "campaigns": reports,
    }


def positive_integer(value: str) -> int:
    try:
        result = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("value must be an integer") from error
    if result <= 0:
        raise argparse.ArgumentTypeError("value must be positive")
    return result


def atomic_write_json(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.tmp-{os.getpid()}")
    temporary.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    os.replace(temporary, path)


def parse_arguments(arguments: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "verify the complete connected-iPad GLES2/GLES3 thermal and "
            "repeated-lifecycle evidence"
        )
    )
    parser.add_argument(
        "--campaign-dir",
        action="append",
        type=Path,
        required=True,
        help="passed physical-device matrix directory; specify once per API",
    )
    parser.add_argument(
        "--guest-memory-mib",
        type=common.device_guest_memory_mib,
        default=common.DEVICE_DEFAULT_GUEST_MEMORY_MIB,
    )
    parser.add_argument(
        "--minimum-launches",
        type=positive_integer,
        default=100,
    )
    parser.add_argument("--report", type=Path)
    return parser.parse_args(arguments)


def main(arguments: Sequence[str] | None = None) -> int:
    options = parse_arguments(arguments)
    report = verify_campaigns(
        options.campaign_dir,
        options.guest_memory_mib,
        options.minimum_launches,
    )
    if options.report is not None:
        atomic_write_json(options.report, report)
        print(f"Thermal/lifecycle report: {options.report.resolve()}")
    result = report["result"]
    print(
        "Verified connected-iPad thermal/lifecycle qualification: "
        f"{result['launch_count']} clean launches, "
        f"states={result['state_totals']}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (cts.CampaignError, OSError) as error:
        print(
            f"verify_ios_device_thermal_lifecycle.py: {error}",
            file=sys.stderr,
        )
        raise SystemExit(1) from error
