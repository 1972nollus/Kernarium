#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import base64
import contextlib
import gzip
import hashlib
import io
import json
from pathlib import Path
import socket
import subprocess
import struct
import sys
import tempfile
import unittest
from unittest import mock


INTEGRATION_GPU = Path(__file__).resolve().parents[2] / "integration/gpu"
sys.path.insert(0, str(INTEGRATION_GPU))
import run_vk_gl_cts as cts  # noqa: E402
import run_ios_device_vk_gl_cts as device_cts  # noqa: E402
import run_ios_simulator_vk_gl_cts as ios_cts  # noqa: E402


REPO_ROOT = Path(__file__).resolve().parents[3]
DERIVATIVE_RECEIPT = INTEGRATION_GPU / "vk_gl_cts_derivative_rootfs.json"


def erofs_contract() -> cts.RootfsContract:
    identity = cts.load_identity()
    glcts = cts.cts_binary_identity(
        identity, "/opt/vk-gl-cts/openglcts/glcts"
    )
    return cts.RootfsContract(
        filesystem_type="erofs",
        size=identity["rootfs"]["size"],
        sha256=identity["rootfs"]["sha256"],
        glcts_sha256=glcts["sha256"],
    )


def derivative_receipt(identity: dict[str, object]) -> dict[str, object]:
    exclusions = ["/opt/geekbench", "/opt/ltp", "/opt/vk-gl-cts/vulkan"]
    return {
        "schema_version": 1,
        "artifact": "uml-gl-gles-rootfs",
        "image": {
            "filesystem": "ext4",
            "filesystem_uuid": "688cb152-ce4b-4ac4-9b5d-3def792791f3",
            "size": 8 * 1024 * 1024 * 1024,
            "sha256": "1" * 64,
            "read_only": True,
        },
        "base_rootfs": {
            "size": identity["rootfs"]["size"],
            "sha256": identity["rootfs"]["sha256"],
        },
        "clone": {
            "excluded_paths": exclusions,
            "excluded_paths_sha256": cts.exclusion_digest(exclusions),
        },
        "guest_receipt": {
            "path": "/usr/share/uml-rootfs/gl-gles-cts-build.env",
            "sha256": "2" * 64,
        },
        "cts": {
            "glcts_path": "/opt/vk-gl-cts/openglcts/glcts",
            "glcts_size": 123456,
            "glcts_sha256": "3" * 64,
            "glcts_build_id": "abcd1234",
        },
    }


def qpa_result(case: str, status: str = "Pass") -> str:
    return f"""#beginTestCaseResult {case}
<?xml version="1.0" encoding="UTF-8"?>
<TestCaseResult Version="0.3.4" CasePath="{case}">
 <Result StatusCode="{status}">result</Result>
</TestCaseResult>
#endTestCaseResult
"""


def qpa(*results: str) -> str:
    return "#sessionInfo releaseName 1.4.5.5\n#beginSession\n" + "".join(
        results
    ) + "#endSession\n"


def config_qpa(
    api: str,
    rows: tuple[str, ...] = (
        "EGL(1): window, pixmap, pbuffer",
        "EGL(2): pbuffer, pixmap",
    ),
    status: str = "Pass",
) -> str:
    case = cts.CONFIG_CASES[api]
    messages = "".join(f"  <Text>{row}</Text>\n" for row in rows)
    result = f"""#beginTestCaseResult {case}
<?xml version="1.0" encoding="UTF-8"?>
<TestCaseResult Version="0.3.4" CasePath="{case}" CaseType="SelfValidate">
 <Section Name="Configs" Description="Configs">
{messages} </Section>
 <Result StatusCode="{status}">{status}</Result>
</TestCaseResult>
#endTestCaseResult
"""
    return qpa(result)


def write_config_inventory_evidence(
    directory: Path,
    api: str,
    contract: cts.RootfsContract,
    page_profile: str = "16k",
    display: str = "vnc",
) -> Path:
    payload = config_qpa(api).encode()
    configs = cts.parse_egl_config_qpa(payload.decode(), api)
    artifacts = directory / "artifacts"
    artifacts.mkdir(parents=True)
    (artifacts / "results.qpa").write_bytes(payload)
    metadata = {
        "schema_version": 1,
        "adapter": "macos",
        "backend": "angle-metal",
        "mode": "config-inventory",
        "api": api,
        "page_profile": page_profile,
        "display": display,
        "rootfs_type": contract.filesystem_type,
        "rootfs_sha256": contract.sha256,
        "glcts_sha256": contract.glcts_sha256,
        "emulator_return_code": 0,
        "completed_cases": 1,
        "status_totals": {"Pass": 1},
        "qpa_sha256": hashlib.sha256(payload).hexdigest(),
        "egl_config_sha256": cts.egl_config_digest(configs),
        "egl_configs": [
            {
                "id": config.identifier,
                "surface_types": list(config.surface_types),
                "selected_surface_type": config.selected_surface_type,
            }
            for config in configs
        ],
    }
    run = directory / "run.json"
    run.write_text(json.dumps(metadata), encoding="utf-8")
    return run


def framed_artifact(name: str, payload: bytes) -> str:
    compressed = gzip.compress(payload, mtime=0)
    digest = hashlib.sha256(compressed).hexdigest()
    encoded = base64.b64encode(compressed).decode("ascii")
    chunks = [encoded[offset : offset + 16] for offset in range(0, len(encoded), 16)]
    lines = [
        f"UML_CTS_ARTIFACT_BEGIN|{name}|gzip+base64|{len(compressed)}|{digest}"
    ]
    lines.extend(
        f"UML_CTS_ARTIFACT_CHUNK|{name}|{index}|{chunk}"
        for index, chunk in enumerate(chunks)
    )
    lines.append(f"UML_CTS_ARTIFACT_END|{name}|{len(chunks)}")
    return "\n".join(lines) + "\n"


def gpu_capability_report() -> str:
    lines: list[str] = []
    extensions = ["GL_EXT_alpha", "GL_EXT_beta"]
    for major, minor in ((3, 0), (3, 1), (3, 2)):
        encode = lambda value: value.encode().hex()
        lines.append(
            "UML_GPU_HOST_CONTEXT|"
            f"requested={major}.{minor}|created=1|egl_error=12288|"
            f"actual={major}.{minor}|vendor_hex={encode('vendor')}|"
            f"renderer_hex={encode('renderer')}|"
            f"version_hex={encode(f'OpenGL ES {major}.{minor}')}|"
            f"glsl_hex={encode('GLSL ES')}|extensions={len(extensions)}"
        )
        lines.extend(
            "UML_GPU_HOST_EXTENSION|"
            f"requested={major}.{minor}|name_hex={extension.encode().hex()}"
            for extension in extensions
        )
    capset_v1 = bytearray(308)
    struct.pack_into("<I", capset_v1, 0, 1)
    capset_v2 = bytearray(1408)
    struct.pack_into("<I", capset_v2, 0, 2)
    capset_v2[696:705] = b"renderer\0"
    for identifier, version, payload in (
        (1, 1, bytes(capset_v1)),
        (2, 2, bytes(capset_v2)),
    ):
        lines.extend(
            (
                "UML_GPU_CAPSET_BEGIN|"
                f"id={identifier}|version={version}|size={len(payload)}|chunks=1",
                "UML_GPU_CAPSET_CHUNK|"
                f"id={identifier}|index=0|hex={payload.hex()}",
                f"UML_GPU_CAPSET_END|id={identifier}|chunks=1",
            )
        )
    return "\n".join(lines) + "\n"


class VkGlCtsToolingTests(unittest.TestCase):
    def test_frozen_identity_is_complete_and_qualification_is_blocked(self) -> None:
        identity = cts.load_identity()
        leaves = cts.leaf_identities(identity)
        self.assertFalse(identity["cts"]["qualification_ready"])
        self.assertIsNone(identity["cts"]["authoritative_build_receipt"])
        self.assertEqual(identity["cts"]["inventory_scope"], "gles2-gles3")
        self.assertEqual(identity["cts"]["complete_caselist_case_count"], 6972)
        self.assertEqual(
            identity["cts"]["complete_caselist_sha256"],
            "3ec390ed2d36f0fd8e32a02718e19c5750cf9381eab002aac9b71f848ebc6162",
        )
        self.assertEqual(
            identity["cts"]["registered_modules"],
            ["KHR-GLES2", "KHR-GLES3"],
        )
        self.assertEqual(leaves["gles2"].case_count, 473)
        self.assertEqual(leaves["gles3"].case_count, 6498)
        self.assertEqual(leaves["gles31"].case_count, 4101)
        self.assertEqual(leaves["gles32-extensions"].case_count, 1097)
        self.assertEqual(leaves["gl31"].case_count, 888)
        self.assertEqual(leaves["gl46"].case_count, 18647)
        for leaf in leaves.values():
            self.assertRegex(leaf.sha256, r"^[0-9a-f]{64}$")
            self.assertTrue(leaf.path.startswith("/opt/vk-gl-cts/"))

    def test_checked_gles_mustpass_expansion_is_exact_and_typed(self) -> None:
        identity = cts.load_identity()
        payload = cts.GLES_MUSTPASS_PATH.read_bytes()
        self.assertEqual(len(payload), identity["mustpass"]["gles_xml"]["size"])
        self.assertEqual(
            hashlib.sha256(payload).hexdigest(),
            identity["mustpass"]["gles_xml"]["sha256"],
        )
        configurations = cts.load_gles_mustpass(identity)
        self.assertEqual(
            set(configurations).intersection(cts.BASELINE_GLES_APIS),
            set(cts.BASELINE_GLES_APIS),
        )
        first = cts.baseline_mustpass_configuration(
            configurations, "gles3", True
        )
        other = cts.baseline_mustpass_configuration(
            configurations, "gles3", False
        )
        self.assertEqual(first.leaf.identifier, "gles3")
        self.assertEqual(first.base_seed, 1)
        self.assertEqual((first.surface_width, first.surface_height), (64, 64))
        self.assertEqual(first.watchdog, "disable")
        self.assertEqual(first.xml_arguments, other.xml_arguments)

        selection = cts.Selection(first.leaf, (), 0, 8)
        matrix = cts.MatrixRun(
            configuration=first,
            egl_config=cts.EglConfig(7, ("pbuffer", "pixmap")),
            egl_config_index=0,
            config_inventory_sha256="1" * 64,
            selection=selection,
            mustpass_xml_path=identity["mustpass"]["gles_xml"]["path"],
            mustpass_xml_size=identity["mustpass"]["gles_xml"]["size"],
            mustpass_xml_sha256=identity["mustpass"]["gles_xml"]["sha256"],
        )
        self.assertEqual(
            cts.matrix_runner_arguments(matrix, "/run/uml-cts/selected.txt"),
            [
                "--deqp-caselist-file=/run/uml-cts/selected.txt",
                "--deqp-screen-rotation=unspecified",
                "--deqp-surface-width=64",
                "--deqp-surface-height=64",
                "--deqp-watchdog=disable",
                "--deqp-base-seed=1",
                "--deqp-surface-type=pbuffer",
                "--deqp-gl-config-id=7",
                "--deqp-gl-context-type=egl",
            ],
        )

        with tempfile.TemporaryDirectory() as temporary:
            changed = Path(temporary) / "mustpass.xml"
            changed.write_bytes(payload.replace(b"base-seed=1", b"base-seed=9", 1))
            with self.assertRaisesRegex(cts.CampaignError, "identity changed"):
                cts.load_gles_mustpass(identity, changed)

    def test_released_macos_runner_oracle_matches_expansion(self) -> None:
        identity = cts.load_identity()
        configurations = cts.load_gles_mustpass(identity)
        oracle = cts.load_runner_oracle(identity, configurations)
        payload = cts.RUNNER_ORACLE_PATH.read_bytes()
        self.assertEqual(oracle.path, "tests/integration/gpu/vk_gl_cts_runner_oracle.json")
        self.assertEqual(oracle.size, len(payload))
        self.assertEqual(oracle.sha256, hashlib.sha256(payload).hexdigest())
        self.assertEqual(
            oracle.fixture_configs,
            (
                cts.EglConfig(17, ("window", "pixmap", "pbuffer")),
                cts.EglConfig(23, ("pbuffer", "pixmap")),
                cts.EglConfig(29, ("pixmap",)),
            ),
        )
        self.assertEqual(
            oracle.successful_case_statuses,
            (
                "Pass",
                "NotSupported",
                "QualityWarning",
                "CompatibilityWarning",
            ),
        )

        with tempfile.TemporaryDirectory() as temporary:
            changed = Path(temporary) / "oracle.json"
            value = json.loads(payload)
            value["summaries"]["gles3"]["runner_arguments"][1][6] = (
                "--deqp-surface-type=window"
            )
            changed.write_text(json.dumps(value), encoding="utf-8")
            with self.assertRaisesRegex(
                cts.CampaignError, "oracle expansion changed"
            ):
                cts.load_runner_oracle(identity, configurations, changed)

    def test_derivative_receipt_is_strict_and_tied_to_the_base(self) -> None:
        identity = cts.load_identity()
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "receipt.json"
            receipt = derivative_receipt(identity)
            path.write_text(json.dumps(receipt), encoding="utf-8")
            loaded, contract = cts.load_derivative_receipt(path, identity)
            self.assertEqual(loaded, receipt)
            self.assertEqual(contract.filesystem_type, "ext4")
            self.assertEqual(contract.glcts_sha256, "3" * 64)

            receipt["base_rootfs"]["sha256"] = "4" * 64
            path.write_text(json.dumps(receipt), encoding="utf-8")
            with self.assertRaisesRegex(cts.CampaignError, "different base"):
                cts.load_derivative_receipt(path, identity)

            receipt = derivative_receipt(identity)
            receipt["clone"]["excluded_paths_sha256"] = "5" * 64
            path.write_text(json.dumps(receipt), encoding="utf-8")
            with self.assertRaisesRegex(cts.CampaignError, "exclusion"):
                cts.load_derivative_receipt(path, identity)

    def test_checked_derivative_receipt_freezes_the_minimal_clone(self) -> None:
        receipt, contract = cts.load_derivative_receipt(
            DERIVATIVE_RECEIPT, cts.load_identity()
        )
        self.assertEqual(contract.filesystem_type, "ext4")
        self.assertEqual(contract.size, 8 * 1024 * 1024 * 1024)
        self.assertEqual(
            contract.sha256,
            "f3ebadb579988892f4e33524b3101e82437199f876a7463aae092251fccec57a",
        )
        self.assertEqual(
            contract.glcts_sha256,
            "cfb17f07d4e7e78ed71a2c065670465c720d9109bb557f4316cbabd1f1c9834e",
        )
        self.assertEqual(
            receipt["clone"]["excluded_paths"],
            [
                "/home/uml/vk-gl-cts-1.4.5.5/android",
                "/home/uml/vk-gl-cts-1.4.5.5/external/vulkancts/README.md",
                "/home/uml/vk-gl-cts-1.4.5.5/external/vulkancts/data",
                "/home/uml/vk-gl-cts-1.4.5.5/external/vulkancts/doc",
                "/home/uml/vk-gl-cts-1.4.5.5/external/vulkancts/modules",
                "/home/uml/vk-gl-cts-1.4.5.5/external/vulkancts/mustpass",
                "/home/uml/vk-gl-cts-1.4.5.5/external/vulkancts/scripts",
                "/home/uml/vk-gl-cts-1.4.5.5/external/vulkancts/vkscpc",
                "/home/uml/vk-gl-cts-1.4.5.5/external/vulkancts/vkscserver",
                "/opt/geekbench",
                "/opt/hpl",
                "/opt/ltp",
                "/opt/node",
                "/opt/passmark",
                "/opt/unixbench",
                "/opt/vk-gl-cts/vulkan",
                "/usr/local/go",
            ],
        )

    def test_ext4_rootfs_contract_is_forwarded_and_checked_in_guest(self) -> None:
        identity = cts.load_identity()
        with tempfile.TemporaryDirectory() as temporary:
            receipt_path = Path(temporary) / "receipt.json"
            receipt_path.write_text(
                json.dumps(derivative_receipt(identity)), encoding="utf-8"
            )
            options = cts.parse_arguments(
                [
                    "--leaf",
                    "gles3",
                    "--api",
                    "gles3",
                    "--rootfs",
                    "/tmp/rootfs.ext4",
                    "--rootfs-type",
                    "ext4",
                    "--rootfs-receipt",
                    str(receipt_path),
                ]
            )
            command = cts.build_harness_command(
                options,
                cts.Selection(options.leaves["gles3"], (), 0, 1),
                Path("/tmp/uml-cts-test"),
            )
            rootfs_type = command.index("--rootfs-type")
            self.assertEqual(
                command[rootfs_type : rootfs_type + 2],
                ["--rootfs-type", "ext4"],
            )
            launcher = cts.build_guest_launcher(
                cts.Selection(options.leaves["gles3"], (), 0, 1),
                0,
                900,
                16384,
                options.rootfs_contract,
            )
            self.assertIn("688cb152-ce4b-4ac4-9b5d-3def792791f3", launcher)
            self.assertIn("gl-gles-cts-build.env", launcher)
            self.assertIn("3" * 64, launcher)

        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(
            SystemExit
        ):
            cts.parse_arguments(
                [
                    "--leaf",
                    "gles3",
                    "--api",
                    "gles3",
                    "--rootfs-type",
                    "ext4",
                ]
            )

    def test_complete_inventory_identity_is_exact_and_fail_closed(self) -> None:
        identity = json.loads(json.dumps(cts.load_identity()))
        digest = hashlib.sha256(b"complete caselist\n").hexdigest()
        identity["cts"]["complete_caselist_sha256"] = digest
        identity["cts"]["complete_caselist_case_count"] = 2
        payload = f"""schema_version=2
scope=gles2-gles3
complete=1
exported_package_count=2
complete_caselist_sha256={digest}
complete_caselist_case_count=2
registered_module=KHR-GLES2
registered_module=KHR-GLES3
""".encode()
        count, parsed_digest, modules, packages = cts.validate_inventory_report(
            payload, identity
        )
        self.assertEqual(count, 2)
        self.assertEqual(parsed_digest, digest)
        self.assertEqual(modules, ["KHR-GLES2", "KHR-GLES3"])
        self.assertEqual(packages, 2)

        cts.validate_inventory_report(payload, identity)
        with self.assertRaisesRegex(cts.CampaignError, "identity changed"):
            cts.validate_inventory_report(
                payload.replace(digest.encode(), b"0" * 64), identity
            )
        with self.assertRaisesRegex(cts.CampaignError, "not complete"):
            cts.validate_inventory_report(
                payload.replace(b"complete=1", b"complete=0"), identity
            )
        with self.assertRaisesRegex(cts.CampaignError, "scope is invalid"):
            cts.validate_inventory_report(
                payload.replace(b"scope=gles2-gles3", b"scope=all"), identity
            )
        with self.assertRaisesRegex(cts.CampaignError, "GLES scope"):
            cts.validate_inventory_report(
                payload.replace(
                    b"registered_module=KHR-GLES3",
                    b"registered_module=dEQP-GL45-ES3",
                ),
                identity,
            )
        with self.assertRaisesRegex(cts.CampaignError, "identity is invalid"):
            cts.validate_inventory_report(
                payload.replace(
                    b"exported_package_count=2",
                    b"exported_package_count=3",
                ),
                identity,
            )

    def test_typed_fraction_and_exact_case_reject_shell_input(self) -> None:
        self.assertEqual(cts.parse_fraction("0,8"), (0, 8))
        with self.assertRaises(Exception):
            cts.parse_fraction("8,8")
        with self.assertRaises(Exception):
            cts.parse_fraction("1/8")
        self.assertEqual(
            cts.exact_case(
                "KHR-GLES3.shaders.declarations.declarations."
                "missing_variable_name_vertex"
            ),
            "KHR-GLES3.shaders.declarations.declarations."
            "missing_variable_name_vertex",
        )
        for value in ("KHR-GLES3.*", "a,b", "$(id)", "case name", "../case"):
            with self.subTest(value=value), self.assertRaises(Exception):
                cts.exact_case(value)

    def test_apple_campaign_allow_list_is_exact_and_fail_closed(self) -> None:
        campaigns = cts.load_apple_campaigns()
        self.assertEqual(
            set(campaigns),
            {
                "gles2-identity-sentinel-v1",
                "gles3-declaration-sentinel-v1",
                "gles3-identity-sentinel-v1",
            },
        )
        campaign = campaigns["gles3-declaration-sentinel-v1"]
        self.assertEqual(campaign.api, "gles3")
        self.assertEqual(campaign.leaf.identifier, "gles3")
        self.assertEqual(campaign.selection.expected_count, 1)
        self.assertEqual(
            campaign.cases,
            (
                "KHR-GLES3.shaders.declarations.declarations."
                "missing_variable_name_vertex",
            ),
        )
        self.assertEqual(
            campaigns["gles2-identity-sentinel-v1"].selection.expected_count,
            5,
        )
        self.assertEqual(
            campaigns["gles3-identity-sentinel-v1"].selection.expected_count,
            12,
        )

        with tempfile.TemporaryDirectory() as temporary:
            invalid = Path(temporary) / "campaigns.json"
            invalid.write_text(
                json.dumps(
                    {
                        "schema_version": 2,
                        "campaigns": [
                            {
                                "id": "bad",
                                "api": "gles3",
                                "leaf": "gles3",
                                "cases": ["KHR-GLES3.*"],
                                "seed": 0,
                                "shard_timeout": 900,
                            }
                        ],
                    }
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(cts.CampaignError, "invalid Apple"):
                cts.load_apple_campaigns(path=invalid)

    def test_fraction_shards_have_an_exact_disjoint_union(self) -> None:
        cases = [f"KHR-GLES3.group.case{index}" for index in range(17)]
        shards = [cts.select_fraction(cases, index, 4) for index in range(4)]
        flattened = [case for shard in shards for case in shard]
        self.assertEqual(set(flattened), set(cases))
        self.assertEqual(len(flattened), len(cases))
        self.assertEqual(shards[0], cases[0::4])

    def test_macos_campaign_owns_its_exact_set_seed_and_timeout(self) -> None:
        options = cts.parse_arguments(
            ["--campaign", "gles3-identity-sentinel-v1", "--dry-run"]
        )
        campaign = options.campaigns[options.campaign]
        self.assertEqual(options.seed, campaign.seed)
        self.assertEqual(options.shard_timeout, campaign.shard_timeout)
        self.assertEqual(campaign.selection.expected_count, 12)
        for conflicting in (
            ["--leaf", "gles3"],
            ["--case", campaign.cases[0]],
            ["--seed", "1"],
            ["--shard-timeout", "1"],
        ):
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(
                SystemExit
            ):
                cts.parse_arguments(
                    [
                        "--campaign",
                        "gles3-identity-sentinel-v1",
                        *conflicting,
                    ]
                )

    def test_caselist_parser_rejects_duplicates_and_invalid_rows(self) -> None:
        self.assertEqual(
            cts.parse_case_list("# comment\nKHR-GLES2.info.version\n\n"),
            ["KHR-GLES2.info.version"],
        )
        with self.assertRaisesRegex(cts.CampaignError, "duplicate case"):
            cts.parse_case_list("a.b\na.b\n")
        with self.assertRaisesRegex(cts.CampaignError, "invalid case"):
            cts.parse_case_list("a.b;uname\n")
        with self.assertRaisesRegex(cts.CampaignError, "empty"):
            cts.parse_case_list("# only a comment\n")

    def test_qpa_parser_requires_complete_unique_results(self) -> None:
        first = "KHR-GLES2.info.version"
        second = "KHR-GLES2.info.renderer"
        parsed = cts.validate_qpa_union(
            qpa(qpa_result(first), qpa_result(second, "NotSupported")),
            [first, second],
        )
        self.assertEqual(parsed, {first: "Pass", second: "NotSupported"})

        terminated = qpa(
            f"#beginTestCaseResult {first}\n#terminateTestCaseResult Crash\n"
        )
        self.assertEqual(cts.parse_qpa(terminated), {first: "Crash"})

        with self.assertRaisesRegex(cts.CampaignError, "truncated"):
            cts.parse_qpa(qpa(qpa_result(first)).replace("#endSession\n", ""))
        with self.assertRaisesRegex(cts.CampaignError, "incomplete QPA"):
            cts.validate_qpa_union(qpa(qpa_result(first)), [first, second])
        with self.assertRaisesRegex(cts.CampaignError, "case order"):
            cts.validate_qpa_union(
                qpa(qpa_result(second), qpa_result(first)),
                [first, second],
            )
        with self.assertRaisesRegex(cts.CampaignError, "duplicate QPA"):
            cts.parse_qpa(qpa(qpa_result(first), qpa_result(first)))

        cts.validate_required_passes({first: "Pass"})
        with self.assertRaisesRegex(cts.CampaignError, "did not pass"):
            cts.validate_required_passes({second: "NotSupported"})
        cts.validate_official_runner_results(
            {
                first: "Pass",
                second: "NotSupported",
                "KHR-GLES2.info.vendor": "QualityWarning",
                "KHR-GLES2.info.extensions": "CompatibilityWarning",
            },
            (
                "Pass",
                "NotSupported",
                "QualityWarning",
                "CompatibilityWarning",
            ),
        )
        for status in ("Fail", "Waiver", "ResourceError", "Crash"):
            with self.subTest(status=status), self.assertRaisesRegex(
                cts.CampaignError, "official CTS runner rejects"
            ):
                cts.validate_official_runner_results(
                    {first: status},
                    (
                        "Pass",
                        "NotSupported",
                        "QualityWarning",
                        "CompatibilityWarning",
                    ),
                )

    def test_qpa_case_path_must_match_container_marker(self) -> None:
        container = "KHR-GLES2.info.version"
        payload = qpa_result("KHR-GLES2.info.renderer").replace(
            "#beginTestCaseResult KHR-GLES2.info.renderer",
            f"#beginTestCaseResult {container}",
        )
        with self.assertRaisesRegex(cts.CampaignError, "case-path mismatch"):
            cts.parse_qpa(qpa(payload))

    def test_egl_config_qpa_is_ordered_and_fail_closed(self) -> None:
        configs = cts.parse_egl_config_qpa(config_qpa("gles3"), "gles3")
        self.assertEqual(
            configs,
            (
                cts.EglConfig(1, ("window", "pixmap", "pbuffer")),
                cts.EglConfig(2, ("pbuffer", "pixmap")),
            ),
        )
        self.assertEqual(configs[0].selected_surface_type, "window")
        self.assertEqual(configs[1].selected_surface_type, "pbuffer")
        self.assertRegex(cts.egl_config_digest(configs), r"^[0-9a-f]{64}$")

        with self.assertRaisesRegex(cts.CampaignError, "duplicate config"):
            cts.parse_egl_config_qpa(
                config_qpa("gles3", ("EGL(1): window", "EGL(1): pbuffer")),
                "gles3",
            )
        with self.assertRaisesRegex(cts.CampaignError, "no usable configs"):
            cts.parse_egl_config_qpa(config_qpa("gles2", ()), "gles2")
        with self.assertRaisesRegex(cts.CampaignError, "did not pass"):
            cts.parse_egl_config_qpa(
                config_qpa("gles2", status="Fail"), "gles2"
            )
        with self.assertRaisesRegex(cts.CampaignError, "unsupported row"):
            cts.parse_egl_config_qpa(
                config_qpa("gles2", ("EGL(1): surfaceless",)), "gles2"
            )

    def test_renderer_probe_rejects_every_software_negative_control(self) -> None:
        valid = """device=/dev/dri/card0
device=/dev/dri/renderD128
x11_server=xwayland
OpenGL renderer string: virgl (Apple GPU)
"""
        cts.validate_renderer_probe(valid)
        cts.validate_renderer_probe(valid + "guest_page_size=16384\n", 16384)
        with self.assertRaisesRegex(cts.CampaignError, "4096-byte guest pages"):
            cts.validate_renderer_probe(valid + "guest_page_size=16384\n", 4096)
        for forbidden in ("llvmpipe", "softpipe", "swrast", "Zink", "Xvfb"):
            with self.subTest(forbidden=forbidden), self.assertRaisesRegex(
                cts.CampaignError, "forbidden renderer"
            ):
                cts.validate_renderer_probe(valid + forbidden)
        with self.assertRaisesRegex(cts.CampaignError, "renderD128"):
            cts.validate_renderer_probe(valid.replace("device=/dev/dri/renderD128\n", ""))
        with self.assertRaisesRegex(cts.CampaignError, "Xwayland"):
            cts.validate_renderer_probe(valid.replace("x11_server=xwayland\n", ""))

    def test_host_backend_refuses_cgl_and_requires_angle_metal(self) -> None:
        cts.validate_host_backend("Virgl ANGLE/Metal provider: hardware\n")
        with self.assertRaisesRegex(cts.CampaignError, "retired CGL"):
            cts.validate_host_backend(
                "Virgl CGL capset probe\nVirgl ANGLE/Metal provider: hardware\n"
            )
        with self.assertRaisesRegex(cts.CampaignError, "marker is missing"):
            cts.validate_host_backend("virgl started\n")

    def test_gpu_capability_and_native_frame_reports_are_fail_closed(self) -> None:
        report = gpu_capability_report()
        parsed = cts.parse_gpu_capability_report(report)
        self.assertEqual(
            [context["requested"] for context in parsed["host_contexts"]],
            [[3, 0], [3, 1], [3, 2]],
        )
        self.assertEqual(
            [capset["id"] for capset in parsed["virgl_capsets"]], [1, 2]
        )
        self.assertEqual(
            parsed["virgl_capsets"][0]["sha256"],
            hashlib.sha256(struct.pack("<I", 1) + bytes(304)).hexdigest(),
        )
        self.assertEqual(
            parsed["virgl_capsets"][1]["decoded"]["v2"]["renderer"],
            "renderer",
        )
        comparison = cts.virgl_capset.compare_reports(
            parsed["virgl_capsets"], parsed["virgl_capsets"]
        )
        self.assertTrue(comparison["equal"])
        self.assertEqual(comparison["capsets"][0]["differences"], [])
        changed = json.loads(json.dumps(parsed["virgl_capsets"]))
        changed[0]["decoded"]["v1"]["glsl_level"] = 300
        changed[0]["data_hex"] = "00" + changed[0]["data_hex"][2:]
        changed_comparison = cts.virgl_capset.compare_reports(
            parsed["virgl_capsets"], changed
        )
        self.assertFalse(changed_comparison["equal"])
        self.assertEqual(
            changed_comparison["capsets"][0]["differences"][0]["field"],
            "$.decoded.v1.glsl_level",
        )
        with self.assertRaisesRegex(cts.CampaignError, "omitted an ES 3.x"):
            cts.parse_gpu_capability_report(
                "\n".join(
                    line
                    for line in report.splitlines()
                    if "requested=3.2" not in line
                )
            )
        with self.assertRaisesRegex(cts.CampaignError, "size changed"):
            cts.parse_gpu_capability_report(
                report.replace(
                    "id=1|version=1|size=308", "id=1|version=1|size=309"
                )
            )

        final = (
            "Native display: direct virgl/ANGLE/Metal presentation\n"
            "Native display final: guest 8 presented 7 dropped 1 cpu-fallback 0\n"
        )
        self.assertEqual(
            cts.parse_native_display_final(final)["cpu_fallback_frames"], 0
        )
        with self.assertRaisesRegex(cts.CampaignError, "CPU scanout fallback"):
            cts.parse_native_display_final(
                final.replace("cpu-fallback 0", "cpu-fallback 1")
            )

    def test_kernarium_transcript_requires_a_clean_guest_shutdown(self) -> None:
        clean = """Darwin UML stopped: result=0 exitcode=0
Kernarium: guest exited with status 0
Kernarium: runtime returned 0
"""
        ios_cts.common.validate_clean_shutdown(clean)
        with self.assertRaisesRegex(cts.CampaignError, "clean shutdown markers"):
            ios_cts.common.validate_clean_shutdown(
                clean.replace("Kernarium: guest exited with status 0\n", "")
            )
        with self.assertRaisesRegex(cts.CampaignError, "memory manager"):
            ios_cts.common.validate_clean_shutdown(
                clean + "Memory manager not clean during takedown.\n"
            )
        cts.validate_guest_shutdown(clean)
        with self.assertRaisesRegex(cts.CampaignError, "clean Darwin"):
            cts.validate_guest_shutdown("")
        with self.assertRaisesRegex(cts.CampaignError, "memory manager"):
            cts.validate_guest_shutdown(
                clean + "Memory manager not clean during takedown.\n"
            )

    def test_artifact_frames_round_trip_and_tolerate_console_prefixes(self) -> None:
        transcript = "".join(
            "[  10.0] service: " + line + "\r\n"
            for line in framed_artifact("results.qpa", b"qpa payload").splitlines()
        )
        self.assertEqual(
            cts.decode_artifacts(transcript),
            {"results.qpa": b"qpa payload"},
        )

    def test_artifact_frames_ignore_echoed_launcher_source(self) -> None:
        transcript = (
            "    printf 'UML_CTS_ARTIFACT_BEGIN|%s|gzip+base64|%s|%s\\n'\n"
            + framed_artifact("results.qpa", b"qpa payload")
        )
        self.assertEqual(
            cts.decode_artifacts(transcript),
            {"results.qpa": b"qpa payload"},
        )

        with self.assertRaisesRegex(cts.CampaignError, "malformed"):
            cts.decode_artifacts(
                "[  10.0] service: UML_CTS_ARTIFACT_BROKEN|results.qpa\n"
            )

    def test_artifact_frames_reject_path_checksum_and_chunk_failures(self) -> None:
        unsafe = framed_artifact("../results.qpa", b"payload")
        with self.assertRaisesRegex(cts.CampaignError, "unsafe"):
            cts.decode_artifacts(unsafe)

        corrupt = framed_artifact("results.qpa", b"payload")
        digest = hashlib.sha256(gzip.compress(b"payload", mtime=0)).hexdigest()
        replacement = ("0" if digest[0] != "0" else "1") + digest[1:]
        corrupt = corrupt.replace(digest, replacement, 1)
        with self.assertRaisesRegex(cts.CampaignError, "checksum"):
            cts.decode_artifacts(corrupt)

        truncated = framed_artifact("results.qpa", b"payload")
        truncated = "\n".join(truncated.splitlines()[:-2]) + "\n"
        with self.assertRaisesRegex(cts.CampaignError, "missing its trailer"):
            cts.decode_artifacts(truncated)

    def test_artifact_transport_acknowledges_only_canonical_frames(self) -> None:
        frames = framed_artifact("results.qpa", b"payload").splitlines()
        acknowledgements = [
            cts.artifact_acknowledgement(frame) for frame in frames
        ]
        self.assertEqual(
            acknowledgements[0],
            "UML_CTS_ARTIFACT_ACK|results.qpa|begin",
        )
        self.assertEqual(
            acknowledgements[-1],
            "UML_CTS_ARTIFACT_ACK|results.qpa|end",
        )
        self.assertTrue(
            all(cts.ACK_ARTIFACT.fullmatch(item) for item in acknowledgements)
        )
        with self.assertRaisesRegex(cts.CampaignError, "non-frame"):
            cts.artifact_acknowledgement("UML_CTS_STATUS|rc=0")

        host, guest = socket.socketpair()
        transport = cts.AcknowledgedArtifactSocket(Path("/unused"))
        transport.connection = host
        transport.connected = True
        try:
            guest.sendall((frames[0] + "\n").encode("ascii"))
            transport.service()
            transport.service()
            self.assertEqual(
                guest.recv(256),
                b"UML_CTS_ARTIFACT_ACK|results.qpa|begin\n",
            )
            guest.close()
            transport.service()
            self.assertEqual(transport.finish(), frames[0] + "\n")
        finally:
            transport.close()
            guest.close()

    def test_artifact_writer_keeps_outputs_beneath_new_directory(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "artifacts"
            cts.write_artifacts({"summary.txt": b"cts_rc=0\n"}, destination)
            self.assertEqual(
                (destination / "summary.txt").read_bytes(),
                b"cts_rc=0\n",
            )
            with self.assertRaises(FileExistsError):
                cts.write_artifacts({"summary.txt": b"again"}, destination)

    def test_launcher_uses_only_frozen_guest_inputs_and_bounded_output(self) -> None:
        leaf = cts.leaf_identities(cts.load_identity())["gles3"]
        selection = cts.Selection(leaf, (), 1, 8)
        launcher = cts.build_guest_launcher(
            selection,
            seed=7,
            timeout=300,
            guest_page_size=16384,
            rootfs_contract=erofs_contract(),
        )
        self.assertIn(leaf.path, launcher)
        self.assertIn(leaf.sha256, launcher)
        self.assertIn("/opt/vk-gl-cts/openglcts/glcts", launcher)
        self.assertIn("--deqp-gl-context-type=egl", launcher)
        self.assertIn("--deqp-log-images=disable", launcher)
        self.assertIn("--deqp-test-oom=disable", launcher)
        self.assertIn("timeout --signal=TERM --kill-after=30 300", launcher)
        self.assertIn("runuser -u uml", launcher)
        self.assertIn('pgrep -u "$uml_uid" -x Xwayland', launcher)
        self.assertIn(r"tr '\000' '\n'", launcher)
        self.assertNotIn("\0", launcher)
        self.assertIn('DISPLAY="$xdisplay"', launcher)
        self.assertNotIn("DISPLAY=:0", launcher)
        self.assertIn("pgrep -x Xvfb", launcher)
        self.assertIn("/dev/dri/renderD128", launcher)
        self.assertIn("guest_page_size=", launcher)
        self.assertIn("does not match 16384", launcher)
        self.assertIn("UML_CTS_ARTIFACT_BEGIN", launcher)
        self.assertIn("systemctl poweroff", launcher)
        self.assertNotIn("apt ", launcher)
        self.assertNotIn("curl ", launcher)
        self.assertNotIn("MESA_GL_VERSION_OVERRIDE", launcher)
        self.assertNotIn("MESA_GLES_VERSION_OVERRIDE", launcher)
        self.assertNotIn("/dev/ubda", launcher)
        self.assertNotIn("\n+    ", launcher)
        exact_launcher = cts.build_guest_launcher(
            cts.Selection(
                leaf,
                (
                    "KHR-GLES3.shaders.declarations.declarations."
                    "missing_variable_name_vertex",
                    "KHR-GLES3.exposed_extensions.validate_extensions",
                ),
                0,
                1,
            ),
            seed=7,
            timeout=300,
            guest_page_size=16384,
            rootfs_contract=erofs_contract(),
        )
        self.assertIn(
            "exact case is absent from selected must-pass leaf",
            exact_launcher,
        )
        for generated_launcher in (launcher, exact_launcher):
            syntax = subprocess.run(
                ["sh", "-n"],
                input=generated_launcher,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(syntax.returncode, 0, syntax.stderr)

        virtio_launcher = cts.build_guest_launcher(
            selection,
            seed=7,
            timeout=300,
            guest_page_size=16384,
            rootfs_contract=erofs_contract(),
            artifact_transport="virtio-serial",
        )
        self.assertIn("artifact_transport=virtio-serial", virtio_launcher)
        self.assertIn(
            f"artifact_channel={cts.VIRTIO_SERIAL_GUEST_DEVICE}",
            virtio_launcher,
        )
        protocol = (
            REPO_ROOT / "darwin/device/console/protocol.hpp"
        ).read_text(encoding="utf-8")
        self.assertIn(f'"{cts.VIRTIO_SERIAL_PORT_NAME}"', protocol)
        self.assertNotIn('stty -F "$artifact_channel"', virtio_launcher)
        self.assertIn('exec 3<> "$artifact_channel"', virtio_launcher)
        self.assertIn("UML_CTS_ARTIFACT_ACK|$name|$index", virtio_launcher)
        self.assertIn("timeout --signal=TERM 30 head -n 1", virtio_launcher)
        syntax = subprocess.run(
            ["sh", "-n"],
            input=virtio_launcher,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(syntax.returncode, 0, syntax.stderr)
        with self.assertRaisesRegex(cts.CampaignError, "transport"):
            cts.build_guest_launcher(
                selection,
                7,
                300,
                16384,
                erofs_contract(),
                artifact_transport="virtiofs",
            )

    def test_inventory_launcher_scopes_the_real_display_to_gles(self) -> None:
        launcher = cts.build_guest_launcher(
            None, 0, 900, 16384, erofs_contract()
        )
        self.assertIn("--deqp-runmode=txt-caselist", launcher)
        self.assertIn(
            "'--deqp-caselist-export-file=/run/uml-cts/${packageName}-cases."
            "${typeExtension}'",
            launcher,
        )
        self.assertIn("--deqp-duplicate-case-name-check=enable", launcher)
        self.assertIn("--deqp-log-filename=/run/uml-cts/results.qpa", launcher)
        self.assertIn("--deqp-case=KHR-GLES2.*", launcher)
        self.assertIn("--deqp-case=KHR-GLES3.*", launcher)
        self.assertLess(
            launcher.index("--deqp-case=KHR-GLES2.*"),
            launcher.index("--deqp-case=KHR-GLES3.*"),
        )
        self.assertNotIn("dEQP-GL45", launcher)
        self.assertNotIn("--deqp-caselist-file=", launcher)
        self.assertIn(
            'package_caselist="$output/$inventory_package-cases.txt"',
            launcher,
        )
        self.assertIn("scope=gles2-gles3", launcher)
        self.assertIn('emit_artifact inventory.txt "$inventory_report"', launcher)
        self.assertIn("x11_server=xwayland", launcher)
        self.assertIn("systemctl poweroff", launcher)
        syntax = subprocess.run(
            ["sh", "-n"],
            input=launcher,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(syntax.returncode, 0, syntax.stderr)
        options = cts.parse_arguments(["--inventory", "--dry-run"])
        self.assertTrue(options.inventory)
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(
            SystemExit
        ):
            cts.parse_arguments(
                ["--inventory", "--leaf", "gles3", "--api", "gles3"]
            )

    def test_config_inventory_launcher_runs_one_exact_cts_case(self) -> None:
        launcher = cts.build_guest_launcher(
            None,
            0,
            900,
            16384,
            erofs_contract(),
            config_inventory_api="gles3",
        )
        self.assertIn("--deqp-case=CTS-Configs.es3", launcher)
        self.assertIn("--deqp-gl-context-type=egl", launcher)
        self.assertNotIn("--deqp-runmode=txt-caselist", launcher)
        self.assertIn("hierarchy_inventory_mode=0", launcher)
        self.assertIn("selected_mode=0", launcher)
        self.assertIn("emit_artifact results.qpa", launcher)
        syntax = subprocess.run(
            ["sh", "-n"],
            input=launcher,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(syntax.returncode, 0, syntax.stderr)
        options = cts.parse_arguments(
            ["--config-inventory", "gles3", "--display", "vnc", "--dry-run"]
        )
        self.assertEqual(options.config_inventory, "gles3")
        self.assertIsNone(options.matrix_run)

    def test_config_inventory_evidence_revalidates_its_qpa(self) -> None:
        contract = erofs_contract()
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            run = write_config_inventory_evidence(directory, "gles2", contract)
            configs, digest = cts.load_config_inventory_evidence(
                run, "gles2", "16k", "vnc", contract
            )
            self.assertEqual(len(configs), 2)
            self.assertEqual(digest, cts.egl_config_digest(configs))

            qpa_path = directory / "artifacts" / "results.qpa"
            qpa_path.write_bytes(qpa_path.read_bytes() + b"changed")
            with self.assertRaisesRegex(cts.CampaignError, "identity changed"):
                cts.load_config_inventory_evidence(
                    run, "gles2", "16k", "vnc", contract
                )

    def test_matrix_cli_requires_authenticated_discovery_and_exact_xml(self) -> None:
        identity = cts.load_identity()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            receipt_path = root / "receipt.json"
            receipt_path.write_text(
                json.dumps(derivative_receipt(identity)), encoding="utf-8"
            )
            _, contract = cts.load_derivative_receipt(receipt_path, identity)
            inventory_run = write_config_inventory_evidence(
                root / "inventory", "gles3", contract
            )
            common = [
                "--matrix",
                "gles3",
                "--config-inventory-run",
                str(inventory_run),
                "--egl-config-index",
                "1",
                "--fraction",
                "3,8",
                "--display",
                "vnc",
                "--rootfs",
                str(root / "rootfs.ext4"),
                "--rootfs-type",
                "ext4",
                "--rootfs-receipt",
                str(receipt_path),
                "--qualification",
                "--dry-run",
            ]
            options = cts.parse_arguments(common)
            matrix = options.matrix_run
            self.assertEqual(
                options.runner_oracle.path,
                "tests/integration/gpu/vk_gl_cts_runner_oracle.json",
            )
            self.assertRegex(options.runner_oracle.sha256, r"^[0-9a-f]{64}$")
            self.assertEqual(matrix.egl_config_index, 1)
            self.assertEqual(matrix.egl_config.identifier, 2)
            self.assertFalse(
                matrix.configuration.use_for_first_egl_config
            )
            self.assertEqual(matrix.selection.expected_count, 812)
            launcher = cts.build_guest_launcher(
                matrix.selection,
                options.seed,
                options.shard_timeout,
                16384,
                options.rootfs_contract,
                matrix=matrix,
            )
            self.assertIn("--deqp-gl-config-id=2", launcher)
            self.assertIn("--deqp-surface-type=pbuffer", launcher)
            self.assertIn("--deqp-base-seed=1", launcher)
            self.assertIn("--deqp-watchdog=disable", launcher)
            self.assertIn(
                "awk -v shard_index=3 -v shard_count=8", launcher
            )
            self.assertNotIn("awk -v index=", launcher)
            self.assertIn(identity["mustpass"]["gles_xml"]["sha256"], launcher)
            self.assertNotIn("--deqp-base-seed=0", launcher)
            syntax = subprocess.run(
                ["sh", "-n"],
                input=launcher,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(syntax.returncode, 0, syntax.stderr)
            harness = cts.build_harness_command(
                options, matrix.selection, root / "artifacts"
            )
            self.assertEqual(
                len(
                    [
                        argument
                        for argument in harness
                        if argument.startswith("--virtio-serial-socket=")
                    ]
                ),
                1,
            )

            without_qualification = [
                item for item in common if item != "--qualification"
            ]
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(
                SystemExit
            ):
                cts.parse_arguments(without_qualification)
            wrong_display = list(common)
            wrong_display[wrong_display.index("vnc")] = "native"
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(
                SystemExit
            ):
                cts.parse_arguments(wrong_display)

    def test_kernarium_generator_embeds_the_common_page_specific_launcher(self) -> None:
        generator = REPO_ROOT / "Kernarium/scripts/generate-cts-campaign-header.py"
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "KernariumCtsCampaigns.hpp"
            subprocess.run(
                [
                    sys.executable,
                    "-B",
                    str(generator),
                    "--runner",
                    str(INTEGRATION_GPU / "run_vk_gl_cts.py"),
                    "--identity",
                    str(INTEGRATION_GPU / "vk_gl_cts_identity.json"),
                    "--campaigns",
                    str(INTEGRATION_GPU / "vk_gl_cts_apple_campaigns.json"),
                    "--derivative-receipt",
                    str(DERIVATIVE_RECEIPT),
                    "--page-size",
                    "4096",
                    "--output",
                    str(output),
                ],
                check=True,
            )
            generated = output.read_text(encoding="utf-8")
            self.assertIn("gles3-declaration-sentinel-v1", generated)
            self.assertIn("gles2-identity-sentinel-v1", generated)
            self.assertIn("gles3-identity-sentinel-v1", generated)
            self.assertIn("does not match 4096", generated)
            self.assertIn("UML_CTS_ARTIFACT_BEGIN", generated)
            self.assertIn("x11_server=xwayland", generated)
            self.assertIn("uml-vk-gl-cts.service", generated)
            self.assertIn("std::string_view erofsSetup", generated)
            self.assertIn("std::string_view ext4Setup", generated)
            self.assertIn("688cb152-ce4b-4ac4-9b5d-3def792791f3", generated)
            self.assertIn(
                "cfb17f07d4e7e78ed71a2c065670465c720d9109bb557f4316cbabd1f1c9834e",
                generated,
            )
            self.assertNotIn("/dev/ubda", generated)

    def test_simulator_adapter_accepts_only_campaign_ids_and_fixed_engine(self) -> None:
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            self.assertEqual(
                ios_cts.main(
                    [
                        "--campaign",
                        "gles3-declaration-sentinel-v1",
                        "--page-profile",
                        "4k",
                        "--macos-run",
                        "/tmp/macos-run.json",
                        "--dry-run",
                    ]
                ),
                0,
            )
        dry_run = json.loads(stdout.getvalue())
        self.assertEqual(dry_run["page_profile"], "4k")
        self.assertEqual(
            dry_run["macos_run"], str(Path("/tmp/macos-run.json").resolve())
        )
        self.assertEqual(
            dry_run["launch_environment"],
            {
                "SIMCTL_CHILD_KERNARIUM_CTS_CAMPAIGN":
                    "gles3-declaration-sentinel-v1",
                "SIMCTL_CHILD_KERNARIUM_EXECUTION_ENGINE": "gadgets",
                "SIMCTL_CHILD_KERNARIUM_KERNEL_PAGE_SIZE": "4096",
                "SIMCTL_CHILD_KERNARIUM_ROOTFS": "rootfs.erofs",
                "SIMCTL_CHILD_UML_GPU_CAPABILITY_REPORT": "1",
                "SIMCTL_CHILD_UML_GPU_FRAME_STATS": "1",
            },
        )
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit):
            ios_cts.parse_arguments(
                ["--campaign", "gles3-declaration-sentinel-v1", "--case", "x"]
            )

    def test_simulator_adapter_selects_the_authenticated_ext4_root(self) -> None:
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            self.assertEqual(
                ios_cts.main(
                    [
                        "--campaign",
                        "gles3-declaration-sentinel-v1",
                        "--macos-run",
                        "/tmp/macos-run.json",
                        "--rootfs-type",
                        "ext4",
                        "--rootfs-receipt",
                        str(DERIVATIVE_RECEIPT),
                        "--dry-run",
                    ]
                ),
                0,
            )
        dry_run = json.loads(stdout.getvalue())
        self.assertEqual(dry_run["rootfs_type"], "ext4")
        self.assertEqual(
            dry_run["rootfs_sha256"],
            "f3ebadb579988892f4e33524b3101e82437199f876a7463aae092251fccec57a",
        )
        self.assertEqual(
            dry_run["launch_environment"][
                "SIMCTL_CHILD_KERNARIUM_ROOTFS"
            ],
            "rootfs.ext4",
        )

    def test_mobile_adapters_reject_an_unchecked_ext4_receipt(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            receipt = Path(temporary) / "derivative.json"
            receipt.write_text(
                json.dumps(derivative_receipt(cts.load_identity())),
                encoding="utf-8",
            )
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit):
                ios_cts.parse_arguments(
                    [
                        "--campaign",
                        "gles3-declaration-sentinel-v1",
                        "--macos-run",
                        "/tmp/macos-run.json",
                        "--rootfs-type",
                        "ext4",
                        "--rootfs-receipt",
                        str(receipt),
                    ]
                )
        self.assertIn("does not match the checked derivative", stderr.getvalue())

    def test_simulator_adapter_requires_an_already_booted_device(self) -> None:
        device = "00000000-0000-0000-0000-000000000000"
        with mock.patch.object(
            ios_cts.subprocess,
            "run",
            return_value=subprocess.CompletedProcess([], 0, device + "\n", ""),
        ):
            ios_cts.require_booted_device(device)
        with mock.patch.object(
            ios_cts.subprocess,
            "run",
            return_value=subprocess.CompletedProcess([], 149, "", "Shutdown"),
        ), self.assertRaisesRegex(cts.CampaignError, "not booted"):
            ios_cts.require_booted_device(device)

    def test_simulator_adapter_revalidates_macos_campaign_evidence(self) -> None:
        campaign = cts.load_apple_campaigns()[
            "gles3-declaration-sentinel-v1"
        ]
        with tempfile.TemporaryDirectory() as temporary:
            run_directory = Path(temporary)
            artifacts = run_directory / "artifacts"
            artifacts.mkdir()
            selected = (campaign.cases[0] + "\n").encode()
            qpa_payload = qpa(qpa_result(campaign.cases[0])).encode()
            renderer_payload = b"renderer evidence\n"
            (artifacts / "selected.txt").write_bytes(selected)
            (artifacts / "results.qpa").write_bytes(qpa_payload)
            (artifacts / "renderer.txt").write_bytes(renderer_payload)
            capability = cts.parse_gpu_capability_report(
                gpu_capability_report()
            )
            native_display = {
                "guest_frames": 8,
                "presented_frames": 7,
                "dropped_frames": 1,
                "cpu_fallback_frames": 0,
            }
            metadata = {
                "adapter": "macos",
                "backend": "angle-metal",
                "campaign": campaign.identifier,
                "display": "native",
                "page_profile": "16k",
                "rootfs_type": "erofs",
                "rootfs_sha256": erofs_contract().sha256,
                "engine": "gadgets",
                "api": campaign.api,
                "leaf": campaign.leaf.identifier,
                "leaf_sha256": campaign.leaf.sha256,
                "expected_cases": 1,
                "seed": campaign.seed,
                "emulator_return_code": 0,
                "completed_cases": 1,
                "status_totals": {"Pass": 1},
                **capability,
                "native_display_final": native_display,
                "selected_caselist_sha256": hashlib.sha256(
                    selected
                ).hexdigest(),
                "renderer_sha256": hashlib.sha256(
                    renderer_payload
                ).hexdigest(),
                "qpa_sha256": hashlib.sha256(qpa_payload).hexdigest(),
            }
            (run_directory / "console.log").write_text(
                gpu_capability_report()
                + "Native display: direct virgl/ANGLE/Metal presentation\n"
                + "Native display final: guest 8 presented 7 dropped 1 "
                "cpu-fallback 0\n",
                encoding="utf-8",
            )
            run = run_directory / "run.json"
            run.write_text(json.dumps(metadata), encoding="utf-8")
            self.assertEqual(
                ios_cts.common.validate_macos_evidence(
                    run, campaign, "16k", erofs_contract()
                ),
                metadata,
            )

            metadata["campaign"] = "another-campaign"
            run.write_text(json.dumps(metadata), encoding="utf-8")
            with self.assertRaisesRegex(cts.CampaignError, "campaign identity"):
                ios_cts.common.validate_macos_evidence(
                    run, campaign, "16k", erofs_contract()
                )

    def test_device_adapter_requires_matching_passing_simulator_evidence(self) -> None:
        campaign = cts.load_apple_campaigns()[
            "gles3-declaration-sentinel-v1"
        ]
        with tempfile.TemporaryDirectory() as temporary:
            run_directory = Path(temporary)
            artifacts = run_directory / "artifacts"
            artifacts.mkdir()
            selected = (campaign.cases[0] + "\n").encode()
            qpa_payload = qpa(qpa_result(campaign.cases[0])).encode()
            (artifacts / "selected.txt").write_bytes(selected)
            (artifacts / "results.qpa").write_bytes(qpa_payload)
            renderer_payload = b"renderer evidence\n"
            (artifacts / "renderer.txt").write_bytes(renderer_payload)
            run = run_directory / "run.json"
            capability = cts.parse_gpu_capability_report(
                gpu_capability_report()
            )
            metadata = {
                "adapter": "ios-simulator-kernarium",
                "backend": "angle-metal",
                "campaign": campaign.identifier,
                "page_profile": "16k",
                "rootfs_type": "erofs",
                "rootfs_sha256": erofs_contract().sha256,
                "engine": "gadgets",
                "api": campaign.api,
                "leaf": campaign.leaf.identifier,
                "leaf_sha256": campaign.leaf.sha256,
                "expected_cases": 1,
                "seed": campaign.seed,
                "runtime_return_code": 0,
                "completed_cases": 1,
                "status_totals": {"Pass": 1},
                **capability,
                "native_display_final": {
                    "guest_frames": 8,
                    "presented_frames": 7,
                    "dropped_frames": 1,
                    "cpu_fallback_frames": 0,
                },
                "renderer_sha256": hashlib.sha256(
                    renderer_payload
                ).hexdigest(),
                "qpa_sha256": hashlib.sha256(qpa_payload).hexdigest(),
                "selected_caselist_sha256": hashlib.sha256(
                    selected
                ).hexdigest(),
                "macos_run": "/tmp/macos-run.json",
                "macos_host_contexts_equal": True,
                "macos_virgl_capsets_equal": True,
                "macos_virgl_capset_comparison":
                    cts.virgl_capset.compare_reports(
                        capability["virgl_capsets"],
                        capability["virgl_capsets"],
                    ),
            }
            (run_directory / "console.log").write_text(
                gpu_capability_report()
                + "Native display: direct virgl/ANGLE/Metal presentation\n"
                + "Native display final: guest 8 presented 7 dropped 1 "
                "cpu-fallback 0\n",
                encoding="utf-8",
            )
            run.write_text(json.dumps(metadata), encoding="utf-8")
            self.assertEqual(
                device_cts.validate_simulator_evidence(
                    run, campaign, "16k", erofs_contract()
                ),
                metadata,
            )

            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                self.assertEqual(
                    device_cts.main(
                        [
                            "--campaign",
                            campaign.identifier,
                            "--device",
                            "device-id",
                            "--simulator-run",
                            str(run),
                            "--dry-run",
                        ]
                    ),
                    0,
                )
            dry_run = json.loads(stdout.getvalue())
            self.assertEqual(
                dry_run["launch_environment"][
                    "DEVICECTL_CHILD_KERNARIUM_EXECUTION_ENGINE"
                ],
                "gadgets",
            )

            metadata["status_totals"] = {"NotSupported": 1}
            run.write_text(json.dumps(metadata), encoding="utf-8")
            with self.assertRaisesRegex(cts.CampaignError, "matching passing"):
                device_cts.validate_simulator_evidence(
                    run, campaign, "16k", erofs_contract()
                )

            metadata["status_totals"] = {"Pass": 1}
            metadata["virgl_capsets"][0]["decoded"]["v1"][
                "glsl_level"
            ] = 999
            run.write_text(json.dumps(metadata), encoding="utf-8")
            with self.assertRaisesRegex(cts.CampaignError, "changed after capture"):
                device_cts.validate_simulator_evidence(
                    run, campaign, "16k", erofs_contract()
                )

    def test_cli_blocks_qualification_and_interpreter(self) -> None:
        stderr = io.StringIO()
        old_stderr = sys.stderr
        try:
            sys.stderr = stderr
            with self.assertRaises(SystemExit):
                cts.parse_arguments(
                    [
                        "--leaf",
                        "gles3",
                        "--api",
                        "gles3",
                        "--qualification",
                    ]
                )
            self.assertIn("No authoritative receipt", stderr.getvalue())
            with self.assertRaises(SystemExit):
                cts.parse_arguments(
                    [
                        "--leaf",
                        "gles3",
                        "--api",
                        "gles3",
                        "--engine",
                        "interpreter",
                    ]
                )
        finally:
            sys.stderr = old_stderr

    def test_page_profile_selects_the_matching_default_emulator(self) -> None:
        options = cts.parse_arguments(
            ["--leaf", "gles3", "--api", "gles3", "--page-profile", "4k"]
        )
        self.assertIn("macos-integration-4k", str(options.emulator))
        options = cts.parse_arguments(
            ["--leaf", "gles3", "--api", "gles3", "--page-profile", "16k"]
        )
        self.assertEqual(options.emulator, cts.DEFAULT_EMULATOR)

    def test_native_harness_does_not_pass_vnc_only_options(self) -> None:
        options = cts.parse_arguments(
            [
                "--leaf",
                "gles3",
                "--api",
                "gles3",
                "--display",
                "native",
            ]
        )
        leaf = options.leaves["gles3"]
        command = cts.build_harness_command(
            options,
            cts.Selection(leaf, (), 0, 1),
            Path("/tmp/uml-cts-test"),
        )
        self.assertIn("--gpu-display=native", command)
        self.assertFalse(any(item.startswith("--gpu-vnc-") for item in command))
        self.assertEqual(
            command[:3],
            ["env", "UML_GPU_CAPABILITY_REPORT=1", "UML_GPU_FRAME_STATS=1"],
        )


if __name__ == "__main__":
    unittest.main()
