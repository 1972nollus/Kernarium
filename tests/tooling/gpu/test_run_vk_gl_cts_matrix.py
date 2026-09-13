#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import contextlib
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


INTEGRATION_GPU = Path(__file__).resolve().parents[2] / "integration/gpu"
sys.path.insert(0, str(INTEGRATION_GPU))
import run_vk_gl_cts as cts  # noqa: E402
import run_vk_gl_cts_matrix as matrix  # noqa: E402


def qpa(cases: tuple[str, ...], status: str = "Pass") -> bytes:
    rows = ["#beginSession\n"]
    for case in cases:
        rows.extend(
            (
                f"#beginTestCaseResult {case}\n",
                '<?xml version="1.0" encoding="UTF-8"?>\n',
                f'<TestCaseResult Version="0.3.4" CasePath="{case}" '
                'CaseType="SelfValidate">\n',
                f' <Result StatusCode="{status}">{status}</Result>\n',
                "</TestCaseResult>\n",
                "#endTestCaseResult\n",
            )
        )
    rows.append("#endSession\n")
    return "".join(rows).encode()


def definition(
    directory: Path,
    cases: tuple[str, ...] = (
        "KHR-GLES2.case0",
        "KHR-GLES2.case1",
        "KHR-GLES2.case2",
        "KHR-GLES2.case3",
        "KHR-GLES2.case4",
    ),
    fraction_count: int = 2,
) -> matrix.CampaignDefinition:
    leaf_payload = "".join(f"{case}\n" for case in cases).encode()
    leaf = cts.LeafIdentity(
        "gles2", "guest/gles2-khr-main.txt", len(cases),
        hashlib.sha256(leaf_payload).hexdigest(),
    )
    first = cts.MustpassConfiguration(
        "gles2", leaf, "khr-main", True, "unspecified", 64, 64, 1,
        "disable", None, ("--deqp-screen-rotation=unspecified",),
    )
    other = cts.MustpassConfiguration(
        "gles2", leaf, "khr-main", False, "unspecified", 64, 64, 1,
        "disable", None, ("--deqp-screen-rotation=unspecified",),
    )
    receipt_path = directory / "receipt.json"
    receipt_path.write_text("{}\n", encoding="utf-8")
    inventory_path = directory / "config-run.json"
    inventory_path.write_text("{}\n", encoding="utf-8")
    return matrix.CampaignDefinition(
        api="gles2",
        page_profile="16k",
        fraction_count=fraction_count,
        shard_timeout=7200,
        guest_timeout=7800,
        leaf=leaf,
        configs=(
            cts.EglConfig(1, ("window", "pixmap", "pbuffer")),
            cts.EglConfig(16, ("window", "pixmap", "pbuffer")),
        ),
        config_digest="1" * 64,
        first_configuration=first,
        other_configuration=other,
        oracle=cts.RunnerOracle(
            "oracle.json",
            123,
            "2" * 64,
            (
                cts.EglConfig(1, ("window",)),
                cts.EglConfig(2, ("pbuffer",)),
                cts.EglConfig(3, ("pixmap",)),
            ),
            (
                "Pass",
                "NotSupported",
                "QualityWarning",
                "CompatibilityWarning",
            ),
        ),
        config_inventory_run=inventory_path,
        config_inventory_run_sha256=cts.sha256_file(inventory_path),
        rootfs=directory / "rootfs.ext4",
        rootfs_contract=cts.RootfsContract(
            "ext4", 8 * 1024 * 1024 * 1024, "3" * 64, "4" * 64,
            "688cb152-ce4b-4ac4-9b5d-3def792791f3", "/receipt", "5" * 64,
        ),
        rootfs_receipt_path=receipt_path,
        rootfs_receipt_sha256=cts.sha256_file(receipt_path),
        rootfs_receipt={"schema_version": 1},
        emulator=directory / "uml-darwin",
        emulator_size=456,
        emulator_sha256="6" * 64,
        git={"revision": "7" * 40, "dirty_paths": []},
        driver_sha256="8" * 64,
        orchestrator_sha256="9" * 64,
        mustpass_xml_path="guest/mustpass.xml",
        mustpass_xml_size=4284,
        mustpass_xml_sha256="a" * 64,
        cts_release="1.4.5.5",
    )


def evidence(
    campaign: Path,
    definition_value: matrix.CampaignDefinition,
    key: matrix.ShardKey,
    cases: tuple[str, ...] | None = None,
) -> matrix.ShardEvidence:
    if cases is None:
        canonical = tuple(
            f"KHR-GLES2.case{index}"
            for index in range(definition_value.leaf.case_count)
        )
        cases = canonical[key.fraction_index :: definition_value.fraction_count]
    run = campaign / "runs" / key.identifier(definition_value.fraction_count)
    run.mkdir(parents=True, exist_ok=True)
    return matrix.ShardEvidence(
        key,
        run,
        cases,
        tuple((case, "Pass") for case in cases),
        "b" * 64,
        "c" * 64,
        "d" * 64,
        "e" * 64,
    )


class MatrixCampaignTests(unittest.TestCase):
    def test_aggregate_reconstructs_every_config_and_fraction(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            campaign = Path(temporary)
            campaign_definition = definition(campaign)
            shards = {
                key: evidence(campaign, campaign_definition, key)
                for key in campaign_definition.all_keys()
            }
            aggregate = matrix.aggregate_matrix(
                campaign, campaign_definition, shards
            )
            self.assertTrue(aggregate["complete"])
            self.assertEqual(aggregate["egl_config_count"], 2)
            self.assertEqual(aggregate["shard_count"], 4)
            self.assertEqual(aggregate["unique_case_count"], 5)
            self.assertEqual(aggregate["case_execution_count"], 10)
            self.assertEqual(aggregate["status_totals"], {"Pass": 10})

            missing = dict(shards)
            missing.pop(matrix.ShardKey(1, 1))
            with self.assertRaisesRegex(cts.CampaignError, "incomplete"):
                matrix.aggregate_matrix(campaign, campaign_definition, missing)

    def test_aggregate_rejects_a_changed_or_duplicate_leaf_union(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            campaign = Path(temporary)
            campaign_definition = definition(campaign)
            shards = {
                key: evidence(campaign, campaign_definition, key)
                for key in campaign_definition.all_keys()
            }
            key = matrix.ShardKey(1, 0)
            changed = ("KHR-GLES2.changed",) + shards[key].cases[1:]
            shards[key] = evidence(
                campaign, campaign_definition, key, changed
            )
            with self.assertRaisesRegex(cts.CampaignError, "frozen leaf"):
                matrix.aggregate_matrix(campaign, campaign_definition, shards)

    def test_child_command_fixes_vnc_jit_and_ten_vcpus(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            campaign_definition = definition(root)
            command = matrix.child_command(
                campaign_definition,
                matrix.ShardKey(0, 1),
                root / "output",
            )
            self.assertIn("--engine", command)
            self.assertEqual(command[command.index("--engine") + 1], "jit")
            self.assertEqual(command[command.index("--display") + 1], "vnc")
            self.assertEqual(command[command.index("--cpus") + 1], "10")
            self.assertEqual(command[command.index("--fraction") + 1], "1,2")
            self.assertNotIn("interpreter", command)
            self.assertNotIn("native", command)

    def test_cli_bounds_parallel_ten_vcpu_guests_and_timeouts(self) -> None:
        required = [
            "--api", "gles2",
            "--config-inventory-run", "/tmp/config.json",
            "--campaign-dir", "/tmp/campaign",
            "--rootfs", "/tmp/rootfs.ext4",
            "--rootfs-receipt", "/tmp/receipt.json",
        ]
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(
            SystemExit
        ):
            matrix.parse_arguments([*required, "--jobs", "3"])
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(
            SystemExit
        ):
            matrix.parse_arguments(
                [
                    *required,
                    "--shard-timeout", "100",
                    "--guest-timeout", "100",
                ]
            )

    def test_validate_shard_reparses_metadata_artifacts_and_order(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            campaign_definition = definition(root)
            key = matrix.ShardKey(0, 0)
            selected = tuple(
                f"KHR-GLES2.case{index}"
                for index in range(0, campaign_definition.leaf.case_count, 2)
            )
            run = root / "run"
            artifacts = run / "artifacts"
            artifacts.mkdir(parents=True)
            selected_payload = "".join(
                f"{case}\n" for case in selected
            ).encode()
            qpa_payload = qpa(selected)
            renderer_payload = b"renderer\n"
            (artifacts / "selected.txt").write_bytes(selected_payload)
            (artifacts / "results.qpa").write_bytes(qpa_payload)
            (artifacts / "renderer.txt").write_bytes(renderer_payload)
            (artifacts / "summary.txt").write_text(
                "cts_rc=0\n", encoding="utf-8"
            )
            (run / "console.log").write_text("console\n", encoding="utf-8")
            artifact_transport_payload = qpa_payload.decode()
            (run / cts.ARTIFACT_TRANSPORT_LOG).write_text(
                artifact_transport_payload, encoding="ascii"
            )
            metadata = {
                "schema_version": 1,
                "adapter": "macos",
                "backend": "zink-kosmickrisp",
                "page_profile": "16k",
                "engine": "jit",
                "memory": "8G",
                "cpus": 10,
                "display": "vnc",
                "surface": [1280, 720],
                "context": "egl",
                "mode": "mustpass-matrix-shard",
                "rootfs_type": "ext4",
                "rootfs_sha256": "3" * 64,
                "glcts_sha256": "4" * 64,
                "emulator_sha256": "6" * 64,
                "rootfs_receipt": {"schema_version": 1},
                "qualification": True,
                "launch_environment": {"UML_GPU_CAPABILITY_REPORT": "1"},
                "api": "gles2",
                "leaf": "gles2",
                "leaf_sha256": campaign_definition.leaf.sha256,
                "fraction": [0, 2],
                "exact_cases": [],
                "expected_cases": len(selected),
                "seed": 1,
                "git": campaign_definition.git,
                "emulator_return_code": 0,
                "artifact_transport": {
                    "type": "virtio-serial-unix-socket",
                    "guest_device": cts.VIRTIO_SERIAL_GUEST_DEVICE,
                    "port_name": cts.VIRTIO_SERIAL_PORT_NAME,
                    "acknowledged_frames": True,
                },
                "matrix": matrix.expected_matrix_metadata(
                    campaign_definition, key
                ),
                "command": [
                    str(campaign_definition.emulator),
                    str(campaign_definition.rootfs),
                    "ncpus=10",
                    "--execution-engine=jit",
                    "--gpu-display=vnc",
                    "--gpu-vnc-port=0",
                    f"--virtio-serial-socket={cts.artifact_socket_path(run)}",
                ],
                "completed_cases": len(selected),
                "status_totals": {"Pass": len(selected)},
                "selected_caselist_sha256": hashlib.sha256(
                    selected_payload
                ).hexdigest(),
                "qpa_sha256": hashlib.sha256(qpa_payload).hexdigest(),
                "renderer_sha256": hashlib.sha256(
                    renderer_payload
                ).hexdigest(),
                "artifact_transport_sha256": hashlib.sha256(
                    artifact_transport_payload.encode("ascii")
                ).hexdigest(),
                "host_contexts": [],
                "virgl_capsets": [],
            }
            (run / "run.json").write_text(
                json.dumps(metadata), encoding="utf-8"
            )
            with (
                mock.patch.object(cts, "validate_renderer_probe"),
                mock.patch.object(cts, "validate_host_backend"),
                mock.patch.object(cts, "validate_guest_shutdown"),
                mock.patch.object(
                    cts,
                    "parse_gpu_capability_report",
                    return_value={"host_contexts": [], "virgl_capsets": []},
                ),
            ):
                result = matrix.validate_shard_run(
                    run, campaign_definition, key
                )
            self.assertEqual(result.cases, selected)

            metadata["emulator_sha256"] = "0" * 64
            (run / "run.json").write_text(
                json.dumps(metadata), encoding="utf-8"
            )
            with self.assertRaisesRegex(cts.CampaignError, "emulator_sha256"):
                matrix.validate_shard_run(run, campaign_definition, key)

    def test_attempt_is_single_and_return_code_is_immutable(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            campaign = Path(temporary)
            campaign_definition = definition(campaign)
            key = matrix.ShardKey(0, 0)
            attempt = matrix.attempt_directory(campaign, key, 2)
            output = attempt / "driver-output"
            run = output / "one-run"
            run.mkdir(parents=True)
            log = attempt / "runner.log"
            log.write_text("runner\n", encoding="utf-8")
            metadata = {
                "schema_version": 1,
                "shard": key.identifier(2),
                "started_at": "2026-08-21T00:00:00+00:00",
                "finished_at": "2026-08-21T00:01:00+00:00",
                "command": matrix.child_command(
                    campaign_definition, key, output
                ),
                "return_code": 0,
                "runner_log_sha256": cts.sha256_file(log),
                "driver_run": str(run.relative_to(campaign)),
            }
            (attempt / "attempt.json").write_text(
                json.dumps(metadata), encoding="utf-8"
            )
            expected = evidence(campaign, campaign_definition, key)
            with mock.patch.object(
                matrix, "validate_shard_run", return_value=expected
            ):
                self.assertEqual(
                    matrix.validate_attempt(campaign, campaign_definition, key),
                    expected,
                )
            metadata["return_code"] = 1
            (attempt / "attempt.json").write_text(
                json.dumps(metadata), encoding="utf-8"
            )
            with self.assertRaisesRegex(cts.CampaignError, "did not complete"):
                matrix.validate_attempt(campaign, campaign_definition, key)


if __name__ == "__main__":
    unittest.main()
