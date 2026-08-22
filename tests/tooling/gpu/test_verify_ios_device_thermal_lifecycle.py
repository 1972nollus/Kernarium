#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest


INTEGRATION_GPU = Path(__file__).resolve().parents[2] / "integration/gpu"
sys.path.insert(0, str(INTEGRATION_GPU))
import run_vk_gl_cts as cts  # noqa: E402
import verify_ios_device_thermal_lifecycle as thermal  # noqa: E402


DEVICE = "3B4DCE30-3D26-4546-999B-9BFD96047108"
APP = {
    "path": "/qualification/Kernarium.app",
    "files": {
        "Kernarium": {"size": 42, "sha256": "1" * 64},
    },
}
ROOTFS = {
    "path": "/qualification/rootfs-gl-gles.ext4",
    "filesystem_type": "ext4",
    "sha256": "2" * 64,
}


def write_json(path: Path, value: object) -> None:
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def console(
    start: str = "nominal",
    finish: str = "nominal",
    changes: tuple[str, ...] = (),
) -> str:
    lines = [
        f"Kernarium thermal state: phase=start state={start} uptime=10.000"
    ]
    lines.extend(
        f"Kernarium thermal state: phase=change state={state} "
        f"uptime={11 + index}.000"
        for index, state in enumerate(changes)
    )
    lines.extend(
        (
            "Kernarium thermal state: phase=finish "
            f"state={finish} uptime=20.000",
            "Darwin UML stopped: result=0 exitcode=0",
            "Kernarium: guest exited with status 0",
            "Kernarium: runtime returned 0",
        )
    )
    return "\n".join(lines) + "\n"


def make_campaign(
    root: Path,
    api: str,
    transcript: str | None = None,
    memory_mib: int = 2048,
) -> Path:
    campaign = root / api
    shard = f"{api}-matrix-cfg00-000of001-v1"
    attempt = campaign / "shards" / shard / "attempt"
    adapter_root = attempt / "adapter-output"
    run = adapter_root / f"run-{api}"
    run.mkdir(parents=True)
    runner_log = attempt / "runner.log"
    runner_log.write_text("adapter passed\n", encoding="utf-8")
    (run / "console.log").write_text(
        transcript if transcript is not None else console(),
        encoding="utf-8",
    )
    thermal_probe = run / "thermal-preflight-001.log"
    thermal_probe.write_text(
        "Kernarium thermal probe: state=nominal uptime=9.000\n",
        encoding="utf-8",
    )
    write_json(
        run / "run.json",
        {
            "adapter": "ios-device-kernarium",
            "backend": "angle-metal",
            "campaign": shard,
            "page_profile": "16k",
            "guest_memory_mib": memory_mib,
            "qualification": True,
            "runtime_return_code": 0,
            "rootfs_sha256": ROOTFS["sha256"],
            "app": APP["path"],
            "device": {
                "identifier": DEVICE,
                "transport": "wired",
            },
            "thermal_preflights": [
                {
                    "state": "nominal",
                    "uptime": 9.0,
                    "devicectl_return_code": 130,
                    "log": thermal_probe.name,
                    "log_sha256": hashlib.sha256(
                        thermal_probe.read_bytes()
                    ).hexdigest(),
                }
            ],
        },
    )
    write_json(
        attempt / "attempt.json",
        {
            "schema_version": 1,
            "shard": shard,
            "started_at": "2026-08-22T00:00:00+00:00",
            "finished_at": "2026-08-22T00:01:00+00:00",
            "command": ["python3", "adapter.py"],
            "return_code": 0,
            "runner_log_sha256": hashlib.sha256(
                runner_log.read_bytes()
            ).hexdigest(),
            "adapter_run": str(run.relative_to(campaign)),
        },
    )
    write_json(
        campaign / "campaign.json",
        {
            "schema_version": 1,
            "kind": thermal.CAMPAIGN_KIND,
            "state": "passed",
            "failures": [],
            "spec": {
                "api": api,
                "app": APP,
                "rootfs": ROOTFS,
                "execution": {
                    "adapter": "ios-device-kernarium",
                    "backend": "angle-metal",
                    "device": DEVICE,
                    "engine": "gadgets",
                    "guest_memory_mib": memory_mib,
                    "page_profile": "16k",
                    "platform": "device",
                },
                "expected": {"shard_count": 1},
                "signed_matrix": {"shards": [shard]},
            },
            "progress": {
                "completed_shards": 1,
                "failed_shards": 0,
                "pending_shards": 0,
                "total_shards": 1,
            },
            "aggregate": {
                "complete": True,
                "unwaived_runner_success": True,
                "shard_count": 1,
            },
        },
    )
    return campaign


class DeviceThermalLifecycleTests(unittest.TestCase):
    def test_accepts_complete_matching_gles_matrices(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            gles2 = make_campaign(
                root,
                "gles2",
                console(changes=("fair", "nominal")),
            )
            gles3 = make_campaign(root, "gles3")
            report = thermal.verify_campaigns(
                [gles2, gles3], minimum_launches=2
            )
            self.assertTrue(report["result"]["passed"])
            self.assertEqual(report["result"]["launch_count"], 2)
            self.assertEqual(
                report["result"]["state_totals"],
                {"fair": 1, "nominal": 5},
            )
            self.assertEqual(
                {campaign["api"] for campaign in report["campaigns"]},
                {"gles2", "gles3"},
            )

    def test_rejects_unsafe_thermal_lifecycles(self) -> None:
        failures = {
            "missing": "Darwin UML stopped: result=0 exitcode=0\n",
            "malformed": (
                "Kernarium thermal state: phase=start state=nominal\n"
            ),
            "phase": console().replace("phase=finish", "phase=start"),
        }
        for label, transcript in failures.items():
            with self.subTest(label=label), self.assertRaises(cts.CampaignError):
                thermal.parse_thermal_samples(transcript, label)

        observed = thermal.parse_thermal_samples(
            console(
                start="fair",
                changes=("serious", "critical"),
                finish="critical",
            ),
            "passing shard",
        )
        self.assertEqual(observed["maximum_state"], "critical")
        self.assertEqual(observed["start_state"], "fair")

    def test_rejects_incomplete_or_mixed_campaign_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            gles2 = make_campaign(root, "gles2")
            gles3 = make_campaign(root, "gles3")
            campaign_path = gles2 / "campaign.json"
            document = json.loads(campaign_path.read_text(encoding="utf-8"))
            document["state"] = "running"
            write_json(campaign_path, document)
            with self.assertRaisesRegex(cts.CampaignError, "passed cleanly"):
                thermal.verify_campaigns(
                    [gles2, gles3], minimum_launches=2
                )

            document["state"] = "passed"
            write_json(campaign_path, document)
            runner_log = (
                gles3
                / "shards/gles3-matrix-cfg00-000of001-v1/attempt/runner.log"
            )
            runner_log.write_text("tampered\n", encoding="utf-8")
            with self.assertRaisesRegex(
                cts.CampaignError, "invalid attempt evidence"
            ):
                thermal.verify_campaigns(
                    [gles2, gles3], minimum_launches=2
                )

    def test_cli_pins_device_memory_steps_and_minimum_launches(self) -> None:
        options = thermal.parse_arguments(
            [
                "--campaign-dir",
                "/tmp/gles2",
                "--campaign-dir",
                "/tmp/gles3",
                "--guest-memory-mib",
                "1984",
                "--minimum-launches",
                "100",
            ]
        )
        self.assertEqual(options.guest_memory_mib, 1984)
        self.assertEqual(options.minimum_launches, 100)


if __name__ == "__main__":
    unittest.main()
