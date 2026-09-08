#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import csv
from contextlib import contextmanager
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


REPO_ROOT = Path(__file__).resolve().parents[3]
METADATA = REPO_ROOT / "tools/rootfs/vulkan"
VERIFY = METADATA / "verify-inputs.py"
VERIFY_RECEIPT = METADATA / "verify-receipt.py"
RECEIPT = REPO_ROOT / "tests/integration/gpu/vulkan_cts_receipt.json"
SEALER_PATH = METADATA / "seal-in-vm.py"

SEALER_SPEC = importlib.util.spec_from_file_location(
    "vulkan_rootfs_sealer", SEALER_PATH
)
assert SEALER_SPEC is not None and SEALER_SPEC.loader is not None
sealer = importlib.util.module_from_spec(SEALER_SPEC)
sys.modules[SEALER_SPEC.name] = sealer
SEALER_SPEC.loader.exec_module(sealer)

VERIFY_SPEC = importlib.util.spec_from_file_location("vulkan_source_inputs", VERIFY)
assert VERIFY_SPEC is not None and VERIFY_SPEC.loader is not None
source_verifier = importlib.util.module_from_spec(VERIFY_SPEC)
VERIFY_SPEC.loader.exec_module(source_verifier)


def source_rows(root: Path = METADATA) -> dict[str, dict[str, str]]:
    lines = [
        line
        for line in (root / "sources.tsv").read_text(encoding="utf-8").splitlines()
        if line and not line.startswith("#")
    ]
    return {
        row["component"]: row
        for row in csv.DictReader(lines, delimiter="\t")
    }


@contextmanager
def mesa_install_fixture(build_name: str = "mesa", wrap_mode: str = "nodownload"):
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary).resolve()
        source = root / "sources/mesa"
        build = root / "build" / build_name
        stage = root / "stage/mesa"
        info_root = build / "meson-info"
        source.mkdir(parents=True)
        info_root.mkdir(parents=True)
        protocol = build.parent / "venus-protocol"
        protocol.mkdir()
        info = {"error": False, "directories": {
            "source": str(source), "build": str(build), "info": str(info_root),
        }}
        (info_root / "meson-info.json").write_text(json.dumps(info))
        options = {**sealer.EXPECTED_MESA_BUILD_OPTIONS, "wrap_mode": wrap_mode,
                   "venus-protocol-dir": str(protocol)}
        (info_root / "intro-buildoptions.json").write_text(json.dumps([
            {"name": name, "value": value} for name, value in options.items()
        ]))
        introspection = {"installed": {}, "install_plan": {"targets": {}, "data": {}}}
        runtime = {}
        commands = []
        compile_updates = {}

        def installed_bytes(relative):
            payload = runtime[relative].read_bytes()
            # Stand in for Meson's install-time ELF RPATH transformation.
            return b"installed:" + payload if relative in sealer.MESA_ELF_STAGE_FILES else payload

        for relative in sealer.EXPECTED_MESA_STAGE_FILES:
            category = "data" if relative.endswith(".conf") else "targets"
            origin = (source if category == "data" else build) / relative
            origin.parent.mkdir(parents=True, exist_ok=True)
            origin.write_bytes(relative.encode())
            runtime[relative] = origin
            introspection["installed"][str(origin)] = "/" + relative
            introspection["install_plan"][category][str(origin)] = {"tag": "runtime"}
            target = stage / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(installed_bytes(relative))
        for relative, target in sealer.EXPECTED_MESA_STAGE_SYMLINKS.items():
            (stage / relative).symlink_to(target)

        def run(arguments, **_kwargs):
            command = [str(value) for value in arguments]
            commands.append(command)
            if command[:2] == ["meson", "compile"]:
                assert command == ["meson", "compile", "-C", str(build), "-j", "4"]
                for path, payload in compile_updates.items():
                    path.write_bytes(payload)
            else:
                assert command[:8] == ["meson", "install", "-C", str(build),
                                       "--no-rebuild", "--tags", "runtime", "--destdir"]
                canonical = Path(command[8])
                for relative in runtime:
                    target = canonical / relative
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(installed_bytes(relative))
                for relative, target in sealer.EXPECTED_MESA_STAGE_SYMLINKS.items():
                    (canonical / relative).symlink_to(target)

        def output(arguments):
            command = [str(value) for value in arguments]
            commands.append(command)
            assert command == ["meson", "introspect", "--installed", "--install-plan", str(build)]
            return json.dumps(introspection)

        with patch.object(sealer, "run", side_effect=run), \
                patch.object(sealer, "output", side_effect=output):
            yield source, build, stage, info, introspection, runtime, commands, compile_updates


@contextmanager
def cts_build_fixture(include_opengl_cts: bool = False):
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary).resolve()
        source, build = root / "sources/VK-GL-CTS", root / "build/cts"
        source.mkdir(parents=True)
        build.mkdir(parents=True)
        cache = build / "CMakeCache.txt"
        cache.write_text("\n".join((
            f"CMAKE_HOME_DIRECTORY:INTERNAL={source}",
            f"CMAKE_CACHEFILE_DIR:INTERNAL={build}",
            "DE_CPU:UNINITIALIZED=DE_CPU_ARM_64",
            "DEQP_TARGET:STRING=default",
            "CMAKE_BUILD_TYPE:STRING=Release",
            "SELECTED_BUILD_TARGETS:STRING=" + ("deqp-vk;glcts" if include_opengl_cts else "deqp-vk"),
        )) + "\n")
        release = "vulkan-cts-1.4.6.2-2-g" + "a" * 40
        binaries = [build / "external/vulkancts/modules/vulkan/deqp-vk"]
        if include_opengl_cts:
            binaries.append(build / "external/openglcts/modules/glcts")
        for binary in binaries:
            binary.parent.mkdir(parents=True)
            binary.write_text("other binary string\n" + release + "\n")
        commands = []

        def output(arguments):
            commands.append(arguments)
            assert arguments[:3] == ("strings", "--all", "--")
            return Path(arguments[3]).read_text()

        def git(repository, *arguments):
            commands.append(("git", repository, *arguments))
            assert repository == source
            assert arguments == ("describe", "--tags", "--long", "--abbrev=40",
                                 "--match", "vulkan-cts-*", "HEAD")
            return release

        with patch.object(sealer, "output", side_effect=output), \
                patch.object(sealer, "git", side_effect=git):
            yield source, build, cache, binaries, release, commands


@contextmanager
def protocol_build_fixture():
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary).resolve()
        source, sdk, build = root / "source", root / "sdk", root / "build/protocol"
        source.mkdir()
        (sdk / "registry").mkdir(parents=True)
        (sdk / "registry/vk.xml").write_text("pinned registry")
        info_root = build / "meson-info"
        info_root.mkdir(parents=True)
        info = {"error": False, "directories": {
            "source": str(source), "build": str(build), "info": str(info_root),
        }}
        options = {"wrap_mode": "nodownload", "vulkan-headers-dir": str(sdk)}
        (info_root / "meson-info.json").write_text(json.dumps(info))
        (info_root / "intro-buildoptions.json").write_text(json.dumps([
            {"name": name, "value": value} for name, value in options.items()
        ]))
        names = ("vn_protocol_driver.h", "vn_protocol_driver_command_buffer.h")
        for name in names:
            (build / name).write_text(name)
        commands = []

        def run(arguments, **_kwargs):
            command = [str(value) for value in arguments]
            commands.append(command)
            if command[:2] == ["meson", "setup"]:
                assert command[3:] == [str(source), "--wrap-mode=nofallback",
                                       f"-Dvulkan-headers-dir={sdk}"]
                for name in names:
                    (Path(command[2]) / name).write_text(name)
            else:
                assert command[:3] == ["meson", "compile", "-C"]
                assert command[4:] == ["-j", "4"]

        with patch.object(sealer, "run", side_effect=run):
            yield source, sdk, build, info, options, names, commands


class VulkanGuestRootfsTests(unittest.TestCase):
    def test_protocol_build_recomputes_all_headers_from_authenticated_inputs(self) -> None:
        with protocol_build_fixture() as fixture:
            source, sdk, build, _, _, names, commands = fixture
            generation = sealer.verify_protocol_build(source.parent, source, sdk, build)
            self.assertEqual(generation["registry_sha256"], sealer.sha256(sdk / "registry/vk.xml"))
            self.assertEqual(generation["driver_headers"], {
                name: sealer.sha256(build / name) for name in names
            })
            self.assertEqual([command[1] for command in commands], ["compile", "setup", "compile"])

    def test_protocol_build_rejects_foreign_configuration_before_compile(self) -> None:
        for field in ("source", "build", "info", "sdk", "wrap_mode"):
            with self.subTest(field=field), protocol_build_fixture() as fixture:
                source, sdk, build, info, options, _, commands = fixture
                if field in info["directories"]:
                    info["directories"][field] = "/"
                else:
                    options["vulkan-headers-dir" if field == "sdk" else field] = "/"
                (build / "meson-info/meson-info.json").write_text(json.dumps(info))
                (build / "meson-info/intro-buildoptions.json").write_text(json.dumps([
                    {"name": name, "value": value} for name, value in options.items()
                ]))
                with self.assertRaises(sealer.SealError):
                    sealer.verify_protocol_build(source.parent, source, sdk, build)
                self.assertEqual(commands, [])

    def test_protocol_build_rejects_modified_or_extra_generated_headers(self) -> None:
        for change in ("modified", "extra"):
            with self.subTest(change=change), protocol_build_fixture() as fixture:
                source, sdk, build, _, _, names, _ = fixture
                name = names[-1] if change == "modified" else "vn_protocol_driver_extra.h"
                (build / name).write_text("unverified generated bytes")
                with self.assertRaisesRegex(sealer.SealError, "header.* differs"):
                    sealer.verify_protocol_build(source.parent, source, sdk, build)

    def test_protocol_build_rejects_escaping_symlink_before_compile(self) -> None:
        with protocol_build_fixture() as fixture, tempfile.TemporaryDirectory() as outside:
            source, sdk, build, _, _, _, commands = fixture
            escaped = build.parent / "escaped"
            escaped.symlink_to(Path(outside).resolve(), target_is_directory=True)
            with self.assertRaisesRegex(sealer.SealError, "protocol build root is outside"):
                sealer.verify_protocol_build(source.parent, source, sdk, escaped)
            self.assertEqual(commands, [])

    def test_generated_patch_ownership_rejects_imports_and_repeated_binaries(self) -> None:
        def entry(path, destination=None):
            return f"diff --git a/{path} b/{path if destination is None else destination}\n"

        source_verifier.verify_generated_ownership(
            entry("src/kosmickrisp/libkk/libkk.spv") + entry("src/poly/cl/libpoly.spv"), "Mesa")
        source_verifier.verify_generated_ownership(entry("src/old.c", "src/new.c"), "Mesa")
        for path in ("libkk.spv", "test_spirv.hpp", "test_spirv.h"):
            with self.subTest(path=path), self.assertRaisesRegex(
                    source_verifier.VerificationError, "repeats a generated file"):
                source_verifier.verify_generated_ownership(entry(path) * 2, "Mesa")
        for payload in (entry("old.spv", "new.spv") + entry("old.spv"),
                        entry("old.spv", "new.spv") + entry("new.spv")):
            with self.subTest(payload=payload), self.assertRaisesRegex(
                    source_verifier.VerificationError, "repeats a generated file"):
                source_verifier.verify_generated_ownership(payload, "Mesa")
        for path, prefix in (("src/virtio/venus-protocol/vn_protocol_driver.h", "src/virtio/venus-protocol/"),
                             ("xmls/vk.xml", "xmls/vk.xml"),
                             ("include/vulkan/vulkan_core.h", "include/vulkan/")):
            for source, destination in ((path, path), (path, "src/renamed.h"),
                                        ("src/renamed.h", path)):
                with self.subTest(source=source, destination=destination), self.assertRaisesRegex(
                        source_verifier.VerificationError, "build-only generated input"):
                    source_verifier.verify_generated_ownership(
                        entry(source, destination), "protocol", (prefix,))
        for header in ('diff --git "a/xmls/vk.xml" "b/xmls/vk.xml"',
                       'diff --git "a/path with spaces" b/plain',
                       'diff --git a/plain "b/path with spaces"',
                       'diff --git a/path with spaces b/path with spaces',
                       r'diff --git "a/path\\name" "b/path\\name"',
                       "diff --git a/plain b/plain trailing", "diff --cc plain",
                       "diff --combined plain", "diff --git a/plain"):
            with self.subTest(header=header), self.assertRaisesRegex(
                    source_verifier.VerificationError, "unsupported or quoted diff header"):
                source_verifier.verify_generated_ownership(header + "\n", "protocol", ("xmls/vk.xml",))

    def test_cts_build_reads_the_release_without_rebuilding(self) -> None:
        for combined in (False, True):
            with self.subTest(combined=combined), cts_build_fixture(combined) as fixture:
                source, build, _, binaries, release, commands = fixture
                self.assertEqual(sealer.verify_cts_build(
                    source, build, include_opengl_cts=combined,
                ), release)
                self.assertEqual(len(commands), 1 + len(binaries))

    def test_cts_build_rejects_foreign_source_and_build_before_readback(self) -> None:
        for key in ("CMAKE_HOME_DIRECTORY:INTERNAL", "CMAKE_CACHEFILE_DIR:INTERNAL"):
            for replacement in ("relative/path", "/"):
                with self.subTest(key=key, replacement=replacement), cts_build_fixture() as fixture:
                    source, build, cache, _, _, commands = fixture
                    lines = cache.read_text().splitlines()
                    cache.write_text("\n".join(
                        key + "=" + replacement if line.startswith(key + "=") else line
                        for line in lines
                    ))
                    with self.assertRaisesRegex(sealer.SealError, "source/build"):
                        sealer.verify_cts_build(source, build, include_opengl_cts=False)
                    self.assertEqual(commands, [])

    def test_cts_build_requires_exact_unambiguous_cache_options(self) -> None:
        for replacement in (
            "# DEQP_TARGET:STRING=default",
            "DEQP_TARGET:STRING=default-extra",
            "DEQP_TARGET:STRING=default\nDEQP_TARGET:STRING=default",
            "DEQP_TARGET:STRING=other",
        ):
            with self.subTest(replacement=replacement), cts_build_fixture() as fixture:
                source, build, cache, _, _, commands = fixture
                cache.write_text(cache.read_text().replace("DEQP_TARGET:STRING=default", replacement))
                with self.assertRaises(sealer.SealError):
                    sealer.verify_cts_build(source, build, include_opengl_cts=False)
                self.assertEqual(commands, [])

    def test_cts_build_rejects_stale_or_ambiguous_binary_releases(self) -> None:
        with cts_build_fixture(True) as fixture:
            source, build, _, binaries, release, _ = fixture
            for binary in binaries:
                for invalid in ("missing\n", release + "-dirty\n",
                                release.replace("a" * 40, "b" * 40) + "\n",
                                release + "\n" + release.replace("-2-g", "-1-g") + "\n"):
                    with self.subTest(binary=binary.name, invalid=invalid):
                        binary.write_text(invalid)
                        with self.assertRaisesRegex(sealer.SealError, "release"):
                            sealer.verify_cts_build(source, build, include_opengl_cts=True)
                        binary.write_text(release + "\n")

    def test_pinned_sources_and_generated_series_verify(self) -> None:
        result = subprocess.run(
            (str(VERIFY),),
            cwd=REPO_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("verified Vulkan guest source locks", result.stdout)

        rows = source_rows()
        self.assertEqual(
            set(rows),
            {
                "mesa",
                "venus-protocol",
                "vulkan-headers",
                "vulkan-loader",
                "vk-gl-cts",
            },
        )
        self.assertEqual(rows["mesa"]["release_ref"], "mesa-26.2.1")
        self.assertEqual(
            rows["mesa"]["base_commit"],
            "da14d65e4499e66468094be52bff9ea0915a695e",
        )
        self.assertEqual(
            rows["vk-gl-cts"]["release_ref"], "vulkan-cts-1.4.6.2"
        )
        self.assertEqual(
            rows["vk-gl-cts"]["base_commit"],
            "f6a29701220f34dd1407513bfe80d74ca7b392ce",
        )
        self.assertEqual(
            rows["vk-gl-cts"]["series"],
            "patches/vk-gl-cts/series.tsv",
        )
        self.assertEqual(rows["venus-protocol"]["series"], "patches/venus-protocol/series.tsv")
        self.assertEqual(
            rows["vulkan-headers"]["release_ref"],
            "vulkan-sdk-1.4.357.0",
        )
        self.assertEqual(
            rows["vulkan-headers"]["base_commit"],
            "e3b1eec08173d6b825cd3ac88c885a63b621504a",
        )
        self.assertEqual(
            rows["vulkan-loader"]["release_ref"],
            "vulkan-sdk-1.4.357.0",
        )
        self.assertEqual(
            rows["vulkan-loader"]["base_commit"],
            "5f157b62e333c63260d05d81bf66faa216ab0fb8",
        )

    def test_patch_tamper_is_rejected_before_build(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            copied = Path(temporary) / "vulkan"
            shutil.copytree(METADATA, copied)
            patch = next((copied / "patches/mesa").glob("*.patch"))
            patch.write_bytes(patch.read_bytes() + b"\n")
            result = subprocess.run(
                (
                    str(VERIFY),
                    "--metadata-root",
                    str(copied),
                    "--outer-root",
                    str(REPO_ROOT),
                ),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("patch digest changed", result.stderr)

    def test_downstream_scope_is_explicit_and_not_conformance(self) -> None:
        readme = (METADATA / "README.md").read_text(encoding="utf-8")
        self.assertIn("intentionally downstream", readme)
        self.assertIn("KosmicKrisp", readme)
        self.assertIn("ordinary Vulkan ICD", " ".join(readme.split()))
        self.assertIn("Khronos conformance claim", readme)

    def test_protocol_header_check_covers_every_generated_header(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            generated = root / "build"
            canonical = root / "canonical"
            generated.mkdir()
            canonical.mkdir(parents=True)
            names = ("vn_protocol_driver.h", "vn_protocol_driver_device.h",
                     "vn_protocol_driver_info.h", "vn_protocol_driver_command_buffer.h")
            for name in names:
                (generated / name).write_text(name)
                (canonical / name).write_text(name)
            sealer.verify_protocol_headers(generated, canonical)
            (generated / names[-1]).write_text("stale command codec")
            with self.assertRaisesRegex(sealer.SealError, "header differs"):
                sealer.verify_protocol_headers(generated, canonical)
            (generated / names[-1]).unlink()
            with self.assertRaisesRegex(sealer.SealError, "inventory differs"):
                sealer.verify_protocol_headers(generated, canonical)

    def test_protocol_header_check_rejects_symlinks_and_empty_sets(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            generated = root / "build"
            canonical = root / "canonical"
            generated.mkdir()
            canonical.mkdir(parents=True)
            with self.assertRaisesRegex(sealer.SealError, "inventory differs"):
                sealer.verify_protocol_headers(generated, canonical)
            name = "vn_protocol_driver.h"
            (generated / name).write_text("codec")
            (canonical / name).symlink_to(generated / name)
            with self.assertRaisesRegex(sealer.SealError, "header differs"):
                sealer.verify_protocol_headers(generated, canonical)

    def test_mesa_binding_accepts_canonical_install_and_build_override(self) -> None:
        for name, wrap_mode in (("mesa", "nodownload"), ("mesa-custom", "nofallback")):
            with self.subTest(build=name), mesa_install_fixture(name, wrap_mode) as fixture:
                source, build, stage, _, _, runtime, commands, _ = fixture
                argv = sealer.verify_mesa_build_stage(source, build, stage, build.parent / "venus-protocol")
                self.assertEqual(argv[:5], ["meson", "setup", str(build), str(source),
                                            f"--wrap-mode={wrap_mode}"])
                self.assertEqual([command[1] for command in commands],
                                 ["compile", "introspect", "install"])
                library = "usr/lib/aarch64-linux-gnu/libvulkan_virtio.so"
                self.assertNotEqual(runtime[library].read_bytes(), (stage / library).read_bytes())

    def test_mesa_binding_rejects_wrong_source_build_or_info_directory(self) -> None:
        for name in ("source", "build", "info"):
            with self.subTest(directory=name), mesa_install_fixture() as fixture:
                source, build, stage, info, _, _, commands, _ = fixture
                other = build.parent / "other"
                other.mkdir()
                info["directories"][name] = str(other)
                (build / "meson-info/meson-info.json").write_text(json.dumps(info))
                with self.assertRaisesRegex(sealer.SealError, f"Mesa {name} directory differs"):
                    sealer.verify_mesa_build_stage(source, build, stage, build.parent / "venus-protocol")
                self.assertEqual(commands, [])

    def test_mesa_binding_rejects_online_build_before_compile(self) -> None:
        with mesa_install_fixture() as fixture:
            source, build, stage, _, _, _, commands, _ = fixture
            options = {**sealer.EXPECTED_MESA_BUILD_OPTIONS, "wrap_mode": "default",
                       "venus-protocol-dir": str(build.parent / "venus-protocol")}
            (build / "meson-info/intro-buildoptions.json").write_text(json.dumps([
                {"name": name, "value": value} for name, value in options.items()
            ]))
            with self.assertRaisesRegex(sealer.SealError, "offline wrap mode"):
                sealer.verify_mesa_build_stage(source, build, stage, build.parent / "venus-protocol")
            self.assertEqual(commands, [])

    def test_mesa_binding_rejects_stale_runtime_stage(self) -> None:
        with mesa_install_fixture() as fixture:
            source, build, stage, _, _, _, _, _ = fixture
            (stage / "usr/lib/aarch64-linux-gnu/libvulkan_virtio.so").write_bytes(b"old driver")
            with self.assertRaisesRegex(sealer.SealError, "staged runtime differs"):
                sealer.verify_mesa_build_stage(source, build, stage, build.parent / "venus-protocol")

    def test_mesa_binding_rejects_missing_or_foreign_protocol_before_compile(self) -> None:
        for value in (None, "relative", "/"):
            with self.subTest(value=value), mesa_install_fixture() as fixture:
                source, build, stage, _, _, _, commands, _ = fixture
                path = build / "meson-info/intro-buildoptions.json"
                options = json.loads(path.read_text())
                for option in options:
                    if option["name"] == "venus-protocol-dir":
                        option["value"] = value
                path.write_text(json.dumps(options))
                with self.assertRaisesRegex(sealer.SealError, "Mesa protocol directory"):
                    sealer.verify_mesa_build_stage(source, build, stage, build.parent / "venus-protocol")
                self.assertEqual(commands, [])

    def test_mesa_binding_retains_exact_stage_inventory(self) -> None:
        for change in ("extra-file", "wrong-symlink"):
            with self.subTest(change=change), mesa_install_fixture() as fixture:
                source, build, stage, _, _, _, _, _ = fixture
                if change == "extra-file":
                    (stage / "unrequested").write_bytes(b"extra")
                else:
                    link = stage / "usr/lib/aarch64-linux-gnu/libgbm.so.1"
                    link.unlink()
                    link.symlink_to("unrelated.so")
                with self.assertRaisesRegex(sealer.SealError, "install .* inventory changed"):
                    sealer.verify_mesa_build_stage(source, build, stage, build.parent / "venus-protocol")

    def test_mesa_binding_synchronizes_same_path_topic_updates(self) -> None:
        with mesa_install_fixture() as fixture:
            source, build, stage, _, _, runtime, commands, updates = fixture
            library = "usr/lib/aarch64-linux-gnu/libvulkan_virtio.so"
            updates[runtime[library]] = b"newly compiled topic"
            with self.assertRaisesRegex(sealer.SealError, "staged runtime differs"):
                sealer.verify_mesa_build_stage(source, build, stage, build.parent / "venus-protocol")
            self.assertEqual(runtime[library].read_bytes(), b"newly compiled topic")
            self.assertEqual([command[1] for command in commands],
                             ["compile", "introspect", "install"])

    def test_mesa_binding_rejects_incomplete_runtime_mapping(self) -> None:
        with mesa_install_fixture() as fixture:
            source, build, stage, _, introspection, runtime, _, _ = fixture
            library = str(runtime["usr/lib/aarch64-linux-gnu/libvulkan_virtio.so"])
            del introspection["install_plan"]["targets"][library]
            with self.assertRaisesRegex(sealer.SealError, "runtime install mapping changed"):
                sealer.verify_mesa_build_stage(source, build, stage, build.parent / "venus-protocol")

    def test_mesa_binding_rejects_foreign_runtime_output(self) -> None:
        with mesa_install_fixture() as fixture:
            source, build, stage, _, introspection, runtime, _, _ = fixture
            library = str(runtime["usr/lib/aarch64-linux-gnu/libvulkan_virtio.so"])
            foreign = build.parent / "foreign.so"
            foreign.write_bytes(b"foreign")
            introspection["installed"][str(foreign)] = introspection["installed"].pop(library)
            introspection["install_plan"]["targets"][str(foreign)] = \
                introspection["install_plan"]["targets"].pop(library)
            with self.assertRaisesRegex(sealer.SealError, "outside its authenticated source/build"):
                sealer.verify_mesa_build_stage(source, build, stage, build.parent / "venus-protocol")

    def test_vm_sealer_allows_only_the_release_verified_video_sample(self) -> None:
        sealer = (METADATA / "seal-in-vm.py").read_text(encoding="utf-8")
        self.assertIn("LTRPSPS_A_Qualcomm_1.bit", sealer)
        self.assertIn(
            "8f8e50fa408b7e96e76e6936b380fc34c208271efb8c283b15a42cb1f8c78ae7",
            sealer,
        )
        self.assertIn("contains an unverified fetched or modified resource", sealer)

    def test_vm_sealer_has_a_distinct_receipt_bound_zink_gl_mode(self) -> None:
        sealer = (METADATA / "seal-in-vm.py").read_text(encoding="utf-8")
        self.assertIn("--include-opengl-cts", sealer)
        self.assertIn("rootfs-vulkan-zink-gl.ext4", sealer)
        self.assertIn("3b59fbc9-4b35-4a2b-890d-286a77b04afd", sealer)
        self.assertIn("external/openglcts/modules", sealer)
        self.assertIn("copy_generated_runtime_directory", sealer)
        self.assertIn("external/graphicsfuzz/data/gles3", sealer)
        self.assertNotIn(
            'copy_directory(\n                opengl_build / directory', sealer
        )
        self.assertIn("opengl_manifest(stage, cts_tree)", sealer)
        self.assertIn('opengl_cts["mustpass"]["documents"]', sealer)
        self.assertIn('opengl_cts["mustpass"]["leaves"]', sealer)
        self.assertNotIn("gl46-gtf-main.txt", sealer)
        self.assertIn("uml-vulkan-zink-gl-rootfs", sealer)

    def test_gl_sealer_derives_only_xml_referenced_scope_and_checks_runner(self) -> None:
        import test_opengl_cts_plan as fixtures

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            stage, source = root / "stage", root / "source"
            files = fixtures.fixture()
            for name, payload in files.items():
                path = stage / name.lstrip("/")
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(payload)
            runner = sealer.opengl_cts_plan.RUNNER_SOURCE
            runner_path = source / runner["path"]
            runner_path.parent.mkdir(parents=True)
            runner_path.write_bytes(b"x" * runner["size"])
            with self.assertRaisesRegex(sealer.SealError, "runner scope source"):
                sealer.opengl_manifest(stage, source)
            with patch.object(sealer, "sha256", return_value=runner["sha256"]):
                manifest = sealer.opengl_manifest(stage, source)
                self.assertEqual(manifest["leaf_count"], 8)
                self.assertEqual(len(manifest["configurations"]), 13)
                self.assertFalse(any("gtf" in leaf["path"] for leaf in manifest["leaves"]))
                missing = stage / manifest["leaves"][0]["path"].lstrip("/")
                missing.unlink()
                with self.assertRaises(FileNotFoundError):
                    sealer.opengl_manifest(stage, source)

    def test_gl_runtime_copy_excludes_collocated_build_products(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            generated = root / "generated"
            source = root / "source"
            overlay = root / "overlay"
            image_root = root / "image"
            for directory in (generated, source, overlay, image_root):
                directory.mkdir()
            (source / "shader.test").write_text("base\n", encoding="utf-8")
            (overlay / "shader.test").write_text("overlay\n", encoding="utf-8")
            (overlay / "extra.test").write_text("extra\n", encoding="utf-8")
            (generated / "shader.test").write_text("overlay\n", encoding="utf-8")
            (generated / "extra.test").write_text("extra\n", encoding="utf-8")
            (generated / "libglcts-es3.a").write_text("build\n", encoding="utf-8")

            sealer.copy_generated_runtime_directory(
                generated,
                (source, overlay),
                image_root,
                "/opt/vk-gl-cts/openglcts/gles3",
            )

            installed = image_root / "opt/vk-gl-cts/openglcts/gles3"
            self.assertEqual(
                sorted(path.name for path in installed.iterdir()),
                ["extra.test", "shader.test"],
            )
            self.assertEqual(
                (installed / "shader.test").read_text(encoding="utf-8"),
                "overlay\n",
            )

    def test_checked_receipt_and_native_arm64_cts_verify(self) -> None:
        result = subprocess.run(
            (str(VERIFY_RECEIPT),),
            cwd=REPO_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        receipt = json.loads(RECEIPT.read_text(encoding="utf-8"))
        self.assertEqual(receipt["schema_version"], 2)
        self.assertFalse(receipt["claim_scope"]["formal_conformance"])
        self.assertTrue(receipt["claim_scope"]["downstream_guest_mesa"])
        self.assertEqual(receipt["cts"]["reported_cpu"], "DE_CPU_ARM_64")
        self.assertEqual(receipt["mustpass"]["leaf_count"], 98)
        self.assertEqual(receipt["mustpass"]["case_count"], 3_247_552)

    def test_checked_receipt_rejects_claim_expansion(self) -> None:
        receipt = json.loads(RECEIPT.read_text(encoding="utf-8"))
        receipt["claim_scope"]["formal_conformance"] = True
        with tempfile.TemporaryDirectory() as temporary:
            changed = Path(temporary) / "receipt.json"
            changed.write_text(json.dumps(receipt), encoding="utf-8")
            result = subprocess.run(
                (str(VERIFY_RECEIPT), "--receipt", str(changed)),
                cwd=REPO_ROOT,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("claim scope changed", result.stderr)


if __name__ == "__main__":
    unittest.main()
