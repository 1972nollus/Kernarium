#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import copy
import contextlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from xml.etree import ElementTree

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "integration/gpu"))
import opengl_cts_plan as plan  # noqa: E402
import run_vk_gl_cts as common  # noqa: E402
import test_run_vk_gl_cts as fixtures  # noqa: E402


def fixture() -> dict[str, bytes]:
    files = {}
    for collection, directory, package_name, _ in plan.COLLECTIONS:
        document = ElementTree.Element("Mustpass", version="main")
        package = ElementTree.SubElement(document, "TestPackage", name=package_name)
        if collection == "es-for-gl":
            rows = [("gl45-es3-main", True, 256, 256, None, "rgba8888d24s8ms0"),
                    ("gl45-es31-main", True, 256, 256, None, "rgba8888d24s8ms0")]
        elif collection == "no-context":
            rows = [(f"gl{api}-khr-main", True, 64, 64, 1, None)
                    for api in (30, 40, 43, 45)]
        elif collection == "single":
            rows = [("gl43-khr-single", True, 64, 64, 1, None),
                    ("gl46-khr-single", True, 64, 64, 1, None)]
        else:
            rows = [("gl46-main", True, 64, 64, 1, None),
                    ("gl46-main", True, 113, 47, 2, None),
                    ("gl46-main", True, 64, -1, 3, "rgba8888d24s8"),
                    ("gl46-main", True, -1, 64, 3, "rgba8888d24s8"),
                    ("gl46-main", False, 64, 64, 1, None),
                    ("gl46-main", False, 113, 47, 2, None),
                    ("gl42-compat-main", True, 64, 64, 1, None)]
        for stem, first, width, height, seed, config in rows:
            command = ["--deqp-screen-rotation=unspecified",
                       f"--deqp-surface-width={width}",
                       f"--deqp-surface-height={height}", "--deqp-watchdog=disable"]
            if seed is not None:
                command.append(f"--deqp-base-seed={seed}")
            if config:
                command.append(f"--deqp-gl-config-name={config}")
                if collection != "es-for-gl":
                    command.append("--deqp-surface-type=fbo")
            name = stem.split("-", 1)[1].removeprefix("compat-")
            ElementTree.SubElement(package, "Configuration", {
                "caseListFile": stem + ".txt", "commandLine": " ".join(command),
                "name": name, "os": "any", "useForFirstEGLConfig": str(first),
            })
            count = 1 if stem in {"gl30-khr-main", "gl40-khr-main"} else 10
            files[f"{plan.ROOT}/{directory}/main/{stem}.txt"] = "".join(
                f"KHR-{stem.replace('-', '.')}.case{index}\n" for index in range(count)
            ).encode()
        files[f"{plan.ROOT}/{directory}/main/mustpass.xml"] = ElementTree.tostring(document)
    files[f"{plan.ROOT}/khronos_mustpass/main/gl46-gtf-main.txt"] = b"GTF-GL46.obsolete\n"
    return files


class OpenGLPlanTests(unittest.TestCase):
    def setUp(self) -> None:
        self.files = fixture()
        self.manifest = plan.build_manifest(self.files.__getitem__)
        self.configs = [{"id": 7, "surface_types": ["pixmap", "pbuffer", "window"]},
                        {"id": 3, "surface_types": ["pixmap", "pbuffer"]},
                        {"id": 9, "surface_types": ["pixmap"]}]

    def test_only_xml_referenced_supported_public_leaves_define_scope(self) -> None:
        self.assertEqual(plan.validate_manifest(self.manifest), self.manifest)
        self.assertEqual(self.manifest["leaf_count"], 8)
        self.assertEqual(len(self.manifest["configurations"]), 13)
        self.assertEqual(self.manifest["case_count"], 62)
        self.assertEqual(self.manifest["unique_case_count"], 62)
        paths = [row["path"] for row in self.manifest["leaves"]]
        self.assertFalse(any("gtf" in path or "compat" in path for path in paths))
        self.assertFalse(any("gl43-khr-single" in path for path in paths))

    def test_runtime_order_controls_first_and_other_config_roles(self) -> None:
        runs = plan.expand_runs(self.manifest, self.configs)
        self.assertEqual(len(runs), 15)
        self.assertEqual([row["egl_config_id"] for row in runs], [7] * 11 + [3] * 2 + [9] * 2)
        self.assertEqual([row["configuration_index"] for row in runs], list(range(11)) + [11, 12, 11, 12])
        self.assertEqual([row["surface_type"] for row in runs[7:]],
                         ["window", "window", "fbo", "fbo", "pbuffer", "pbuffer", "pixmap", "pixmap"])

    def test_upstream_es_for_gl_implicit_seed_and_fbo_are_preserved(self) -> None:
        run = plan.expand_runs(self.manifest, self.configs)[0]
        arguments = plan.runner_arguments(self.manifest, run, "/run/selected.txt")
        for argument in ("--deqp-base-seed=-1", "--deqp-surface-type=fbo",
                         "--deqp-gl-config-name=rgba8888d24s8ms0",
                         "--deqp-gl-config-id=7", "--deqp-gl-context-type=egl"):
            self.assertIn(argument, arguments)

    def test_main_xml_sizes_seeds_and_negative_fbo_dimension_are_preserved(self) -> None:
        runs = plan.expand_runs(self.manifest, self.configs)
        values = [(64, 64, 1), (113, 47, 2), (64, -1, 3), (-1, 64, 3)]
        for run, (width, height, seed) in zip(runs[7:11], values):
            with self.subTest(configuration=run["configuration_index"]):
                arguments = plan.runner_arguments(self.manifest, run, "/selected")
                self.assertIn(f"--deqp-surface-width={width}", arguments)
                self.assertIn(f"--deqp-surface-height={height}", arguments)
                self.assertIn(f"--deqp-base-seed={seed}", arguments)

    def test_xml_payload_hash_and_path_are_checked(self) -> None:
        for key, value in (("text", "<Mustpass/>"), ("sha256", "0" * 64),
                           ("size", 1), ("path", "/unexpected")):
            with self.subTest(key=key):
                bad = copy.deepcopy(self.manifest)
                bad["documents"][0][key] = value
                with self.assertRaises(plan.PlanError):
                    plan.validate_manifest(bad)

    def test_derived_configuration_records_cannot_be_relabelled(self) -> None:
        bad = copy.deepcopy(self.manifest)
        bad["configurations"][7]["base_seed"] = 0
        with self.assertRaisesRegex(plan.PlanError, "differ from authenticated XML"):
            plan.validate_manifest(bad)

    def test_unsafe_and_obsolete_xml_leaf_references_are_rejected(self) -> None:
        path = f"{plan.ROOT}/khronos_mustpass/main/mustpass.xml"
        for replacement in (b"../gl46-main.txt", b"gl46-gtf-main.txt"):
            with self.subTest(replacement=replacement):
                files = dict(self.files)
                files[path] = files[path].replace(b"gl46-main.txt", replacement)
                with self.assertRaises(plan.PlanError):
                    plan.build_manifest(files.__getitem__)

    def test_omitted_first_or_other_roles_are_rejected(self) -> None:
        path = f"{plan.ROOT}/khronos_mustpass/main/mustpass.xml"
        for old, new in ((b'"True"', b'"False"'), (b'"False"', b'"True"')):
            with self.subTest(old=old):
                files = dict(self.files)
                files[path] = files[path].replace(old, new)
                with self.assertRaisesRegex(plan.PlanError, "lacks first/other"):
                    plan.build_manifest(files.__getitem__)

    def test_unsafe_numeric_and_duplicate_arguments_fail_closed(self) -> None:
        arguments = self.manifest["configurations"][7]["xml_arguments"]
        for extra in ("--deqp-base-seed=0", "--unexpected=1"):
            with self.subTest(extra=extra), self.assertRaises(plan.PlanError):
                plan.command_values(arguments + [extra], "main")
        for old, new in (("--deqp-surface-width=64", "--deqp-surface-width=0"),
                         ("--deqp-base-seed=1", "--deqp-base-seed=-1")):
            with self.subTest(new=new), self.assertRaises(plan.PlanError):
                plan.command_values([new if value == old else value for value in arguments], "main")

    def test_invalid_or_duplicate_runtime_configs_are_rejected(self) -> None:
        for configs in ([], self.configs + [self.configs[0]],
                        [{"id": True, "surface_types": ["window"]}],
                        [{"id": 1, "surface_types": []}],
                        [{"id": 1, "surface_types": ["window", "window"]}]):
            with self.subTest(configs=configs), self.assertRaises(plan.PlanError):
                plan.expand_runs(self.manifest, configs)

    def test_leaf_canonical_encoding_is_required_for_exact_reconstruction(self) -> None:
        for payload in (b"KHR.test", b"KHR.test\r\n", b"KHR.test\n\n",
                        b"KHR.test\nKHR.test\n", b"KHR.test\x00\n"):
            with self.subTest(payload=payload), self.assertRaises(plan.PlanError):
                plan.case_list(payload)

    def receipt(self) -> dict:
        receipt = fixtures.zink_gl_derivative_receipt(common.load_identity())
        receipt["schema_version"] = 4
        receipt["opengl_cts"]["release"] = "vulkan-cts-1.4.6.2"
        receipt["opengl_cts"]["mustpass"] = self.manifest
        return receipt

    def test_current_receipt_derives_scope_and_rejects_source_or_xml_drift(self) -> None:
        receipt = self.receipt()
        checked, _ = common.load_zink_gl_derivative_receipt(receipt, common.load_identity())
        self.assertEqual(checked["opengl_cts"]["mustpass"], self.manifest)
        for section, key, value in (("runner_source", "sha256", "0" * 64),
                                    ("configurations", "base_seed", 99)):
            bad = copy.deepcopy(receipt)
            target = bad["opengl_cts"]["mustpass"][section]
            (target[0] if isinstance(target, list) else target)[key] = value
            with self.subTest(section=section), self.assertRaises(common.CampaignError):
                common.load_zink_gl_derivative_receipt(bad, common.load_identity())

    def test_typed_matrix_matches_all_xml_roles_and_guest_identity_checks(self) -> None:
        receipt = self.receipt()
        configs = tuple(common.EglConfig(row["id"], tuple(row["surface_types"])) for row in self.configs)
        for run in plan.expand_runs(self.manifest, self.configs):
            with self.subTest(run=run["index"]):
                matrix = common.opengl_matrix_run(
                    receipt, configs, "1" * 64, run["egl_config_index"],
                    run["configuration_index"], (0, 8),
                )
                self.assertEqual(
                    sorted(common.matrix_runner_arguments(matrix, "/run/selected.txt")),
                    sorted(plan.runner_arguments(self.manifest, run, "/run/selected.txt")),
                )
                _, contract = common.load_zink_gl_derivative_receipt(receipt, common.load_identity())
                script = common.build_guest_launcher(
                    matrix.selection, 0, 60, 16384, contract,
                    matrix=matrix, renderer_backend="zink",
                )
                self.assertIn(matrix.mustpass_xml_sha256, script)
                self.assertIn(matrix.selection.leaf.sha256, script)
                self.assertEqual(subprocess.run(["/bin/sh", "-n"], input=script,
                                                text=True, capture_output=True).returncode, 0)

    def test_legacy_receipt_and_wrong_egl_xml_role_cannot_run_full_matrix(self) -> None:
        configs = (common.EglConfig(1, ("window",)), common.EglConfig(2, ("pbuffer",)))
        legacy = fixtures.zink_gl_derivative_receipt(common.load_identity())
        with self.assertRaisesRegex(common.CampaignError, "schema-4"):
            common.opengl_matrix_run(legacy, configs, "1" * 64, 0, 7, (0, 8))
        for config_index, xml_index in ((0, 11), (1, 0), (1, 7), (2, 11), (0, 99)):
            with self.subTest(config=config_index, xml=xml_index), self.assertRaises(common.CampaignError):
                common.opengl_matrix_run(self.receipt(), configs, "1" * 64,
                                         config_index, xml_index, (0, 8))

    def test_config_inventory_binds_provider_rootfs_order_and_transport(self) -> None:
        receipt = self.receipt()
        _, contract = common.load_zink_gl_derivative_receipt(receipt, common.load_identity())
        runtime = {"emulator_sha256": "1" * 64, "kosmickrisp_sha256": "2" * 64}
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            path = fixtures.write_config_inventory_evidence(
                directory, "gl46", contract, runtime_identity=runtime,
            )
            configs, _ = common.load_config_inventory_evidence(
                path, "gl46", "16k", "vnc", contract, runtime, renderer="zink")
            self.assertEqual([row.identifier for row in configs], [1, 2])
            original = path.read_text()
            for key, value in (("kosmickrisp_sha256", "3" * 64),
                               ("emulator_sha256", "3" * 64), ("rootfs_sha256", "3" * 64),
                               ("backend", "zink-kosmickrisp"), ("surface", [640, 480]),
                               ("egl_configs", [])):
                changed = json.loads(original)
                changed[key] = value
                path.write_text(json.dumps(changed))
                with self.subTest(key=key), self.assertRaises(common.CampaignError):
                    common.load_config_inventory_evidence(
                        path, "gl46", "16k", "vnc", contract, runtime,
                        renderer="zink")
            path.write_text(original)
            (directory / "artifacts/summary.txt").write_text("cts_rc=1\n")
            with self.assertRaisesRegex(common.CampaignError, "transport and artifacts"):
                common.load_config_inventory_evidence(
                    path, "gl46", "16k", "vnc", contract, runtime,
                    renderer="zink")

    def test_gl_discovery_is_separate_from_the_matrix_on_either_stack(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "receipt.json"
            path.write_text(json.dumps(self.receipt()))
            args = ["--renderer", "zink", "--config-inventory", "gl46",
                    "--rootfs-type", "ext4", "--rootfs-receipt", str(path)]
            self.assertEqual(common.parse_arguments(args).config_inventory, "gl46")
            # Both GL46 stacks discover their own EGL configs: guest Zink over
            # Venus, and guest Virgl over the host's Mesa Zink provider.
            virgl = common.parse_arguments(args + ["--renderer", "virgl"])
            self.assertEqual(
                (virgl.config_inventory, virgl.renderer, virgl.gl46_scope),
                ("gl46", "virgl", True),
            )
            for extra in (["--qualification"], ["--egl-config-index", "0"],
                          ["--case", "KHR-GL46.foo"]):
                with self.subTest(extra=extra), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                    common.parse_arguments(args + extra)


if __name__ == "__main__":
    unittest.main()
