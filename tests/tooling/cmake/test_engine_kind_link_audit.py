#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the File API architecture audit on real configured graphs."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import itertools
import re
import subprocess
import tempfile
from pathlib import Path


ENGINE_KINDS = ("native", "gadget", "shared", "mixed")


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )


def normalized(output: str) -> str:
    return re.sub(r"\s+", " ", output)


def configure(
    cmake: Path,
    module: Path,
    root: Path,
    body: list[str],
    languages: str = "NONE",
    platform: str | None = None,
    sources: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    source_root = root / "source"
    build_root = root / "build"
    source_root.mkdir(parents=True)
    for name, text in (sources or {}).items():
        path = source_root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
    query = build_root / ".cmake" / "api" / "v1" / "query"
    query.mkdir(parents=True)
    (query / "codemodel-v2").touch()
    (query / "cache-v2").touch()
    module_path = module.resolve().as_posix().replace('"', '\\"')
    (source_root / "CMakeLists.txt").write_text(
        "\n".join(
            [
                "cmake_minimum_required(VERSION 3.27)",
                f"project(uml_engine_kind_fixture LANGUAGES {languages})",
                f'include("{module_path}")',
                *body,
                "",
            ]
        ),
        encoding="utf-8",
    )
    command = [str(cmake), "-S", str(source_root), "-B", str(build_root)]
    if platform is not None:
        command.append(f"-DUML_APPLE_PLATFORM={platform}")
    return run(command)


# The x86-64 JIT's pieces, each of which only a macOS tree may configure.
JIT_SOURCES = {
    "darwin/cpu/x86_64/jit/fex_engine.cpp": "int fixture_engine;\n",
    "darwin/runtime/x86_jit_runtime.cpp": "int fixture_seam;\n",
    "darwin/runtime/jit_user.cpp": (
        "// cpu/x86_64/jit/ in a comment is not an include\n"
        '#include "cpu/x86_64/jit/fex_engine.hpp"\n'
    ),
    "darwin/cpu/x86_64/jit/fex_engine.hpp": "#pragma once\n",
    "darwin/runtime/clean.cpp": (
        "// cpu/x86_64/jit/ named in a comment\n"
        'static const char *name = "cpu/x86_64/jit/";\n'
    ),
    "main.cpp": "int main() { return 0; }\n",
}
JIT_TARGET = (
    "add_library(uml_x86_64_jit OBJECT darwin/cpu/x86_64/jit/fex_engine.cpp)"
)
JIT_SEAM = "add_library(seam OBJECT darwin/runtime/x86_jit_runtime.cpp)"
JIT_INCLUDE = (
    "add_library(includer OBJECT darwin/runtime/jit_user.cpp)\n"
    "target_include_directories(includer PRIVATE darwin)"
)
JIT_LINK = (
    "add_executable(product main.cpp)\n"
    "target_link_libraries(product PRIVATE "
    "${CMAKE_BINARY_DIR}/prefix/lib/libFEXCore.a)"
)
JIT_CLEAN = "add_library(ordinary OBJECT darwin/runtime/clean.cpp)"
JIT_PREFIX = "macOS-only x86-64 JIT in a UML_APPLE_PLATFORM="


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", type=Path, required=True)
    parser.add_argument("--audit", type=Path, required=True)
    options = parser.parse_args()
    module = options.audit.parent.parent / "cmake" / "UmlArchitecture.cmake"

    failures: list[str] = []
    accepted = rejected = 0
    with tempfile.TemporaryDirectory(prefix="uml-engine-kind-") as directory:
        root = Path(directory)

        # A CPU target must declare its engine kind at configure time.
        unclassified = configure(
            options.cmake,
            module,
            root / "unclassified",
            [
                "add_library(unclassified_cpu_target INTERFACE)",
                "uml_declare_architecture_target(unclassified_cpu_target cpu)",
            ],
        )
        if (
            unclassified.returncode == 0
            or "requires an explicit engine kind"
            not in normalized(unclassified.stdout)
        ):
            failures.append(
                "CPU target without an engine kind was accepted:\n"
                f"{unclassified.stdout}"
            )

        # One configured graph per kind pair: an engine may link its own kind
        # or shared code, shared code links only shared code, and only the
        # mixed composition may link everything.
        for source_kind, destination_kind in itertools.product(
            ENGINE_KINDS, repeat=2
        ):
            label = f"{source_kind} -> {destination_kind}"
            fixture = root / f"{source_kind}-{destination_kind}"
            configured = configure(
                options.cmake,
                module,
                fixture,
                [
                    "add_library(source_engine INTERFACE)",
                    "add_library(destination_engine INTERFACE)",
                    "uml_declare_architecture_target(source_engine cpu "
                    f"{source_kind})",
                    "uml_declare_architecture_target(destination_engine cpu "
                    f"{destination_kind})",
                    "target_link_libraries(source_engine INTERFACE "
                    "destination_engine)",
                    "uml_write_architecture_manifest()",
                ],
            )
            if configured.returncode != 0:
                failures.append(
                    f"could not configure {label}:\n{configured.stdout}"
                )
                continue
            audited = run(
                [
                    str(options.cmake),
                    f"-DBUILD_ROOT={fixture / 'build'}",
                    "-P",
                    str(options.audit),
                ]
            )
            allowed = (
                source_kind == "mixed"
                or destination_kind == "shared"
                or source_kind == destination_kind
            )
            if allowed:
                accepted += 1
                if audited.returncode != 0:
                    failures.append(
                        f"rejected allowed {label}:\n{audited.stdout}"
                    )
            else:
                rejected += 1
                if (
                    audited.returncode == 0
                    or "Forbidden cross-engine"
                    not in normalized(audited.stdout)
                ):
                    failures.append(
                        f"accepted forbidden {label}:\n{audited.stdout}"
                    )

        # The same audit rejects a declared cycle, a link against the layer
        # direction, a declared target the codemodel lacks, and a dependency
        # whose name the manifest cannot carry.
        graphs = {
            "cycle": (
                [
                    "add_library(first INTERFACE)",
                    "add_library(second INTERFACE)",
                    "uml_declare_architecture_target(first platform)",
                    "uml_declare_architecture_target(second platform)",
                    "target_link_libraries(first INTERFACE second)",
                    "target_link_libraries(second INTERFACE first)",
                ],
                "Declared architecture target dependency cycle: "
                "first -> second -> first",
            ),
            "layer": (
                [
                    "add_library(memory_contract INTERFACE)",
                    "add_library(cpu_contract INTERFACE)",
                    "uml_declare_architecture_target(memory_contract memory)",
                    "uml_declare_architecture_target(cpu_contract cpu shared)",
                    "target_link_libraries(memory_contract INTERFACE "
                    "cpu_contract)",
                ],
                "Forbidden declared link architecture edge: "
                "memory_contract (memory) -> cpu_contract (cpu)",
            ),
            "absent": (
                [
                    "add_library(imported_platform STATIC IMPORTED)",
                    "uml_declare_architecture_target(imported_platform "
                    "platform)",
                ],
                "Declared architecture target is absent from the CMake File "
                "API: imported_platform",
            ),
            "malformed": (
                [
                    "add_library(Fixture::contract INTERFACE IMPORTED)",
                    "add_library(platform_user INTERFACE)",
                    "uml_declare_architecture_target(Fixture::contract "
                    "platform)",
                    "uml_declare_architecture_target(platform_user platform)",
                    "target_link_libraries(platform_user INTERFACE "
                    "Fixture::contract)",
                ],
                "Malformed architecture dependency for platform_user: "
                "link@Fixture::contract",
            ),
        }
        for label, (body, message) in graphs.items():
            fixture = root / label
            configured = configure(
                options.cmake,
                module,
                fixture,
                [*body, "uml_write_architecture_manifest()"],
            )
            if configured.returncode != 0:
                failures.append(
                    f"could not configure {label}:\n{configured.stdout}"
                )
                continue
            audited = run(
                [
                    str(options.cmake),
                    f"-DBUILD_ROOT={fixture / 'build'}",
                    "-P",
                    str(options.audit),
                ]
            )
            rejected += 1
            if audited.returncode == 0 or message not in normalized(
                audited.stdout
            ):
                failures.append(f"accepted {label}:\n{audited.stdout}")

        # The x86-64 JIT is macOS-only: an iOS or unnamed platform rejects
        # each of its pieces, and macOS accepts them all at once.
        jit_graphs = {
            "jit-target-ios": (
                "ios", [JIT_TARGET], "ios configuration: target uml_x86_64_jit"
            ),
            "jit-target-unset": (
                None, [JIT_TARGET],
                "unset configuration: target uml_x86_64_jit",
            ),
            "jit-source-simulator": (
                "ios-simulator",
                ["add_library(engine OBJECT "
                 "darwin/cpu/x86_64/jit/fex_engine.cpp)"],
                "engine compiles darwin/cpu/x86_64/jit/fex_engine.cpp",
            ),
            "jit-seam-ios": (
                "ios", [JIT_SEAM],
                "seam compiles darwin/runtime/x86_jit_runtime.cpp",
            ),
            "jit-include-ios": (
                "ios", [JIT_INCLUDE],
                "includer compiles darwin/runtime/jit_user.cpp, which "
                "includes cpu/x86_64/jit/",
            ),
            "jit-link-ios": ("ios", [JIT_LINK], "product links FEXCore"),
            "jit-clean-ios": ("ios", [JIT_CLEAN], None),
            "jit-all-macos": (
                "macos",
                [JIT_TARGET, JIT_SEAM, JIT_INCLUDE, JIT_LINK, JIT_CLEAN],
                None,
            ),
        }
        for label, (platform, body, message) in jit_graphs.items():
            fixture = root / label
            configured = configure(
                options.cmake,
                module,
                fixture,
                [*body, "uml_write_architecture_manifest()"],
                languages="CXX",
                platform=platform,
                sources=JIT_SOURCES,
            )
            if configured.returncode != 0:
                failures.append(
                    f"could not configure {label}:\n{configured.stdout}"
                )
                continue
            audited = run(
                [
                    str(options.cmake),
                    f"-DBUILD_ROOT={fixture / 'build'}",
                    "-P",
                    str(options.audit),
                ]
            )
            output = normalized(audited.stdout)
            if message is None:
                accepted += 1
                if audited.returncode != 0:
                    failures.append(f"rejected {label}:\n{audited.stdout}")
            else:
                rejected += 1
                if (
                    audited.returncode == 0
                    or JIT_PREFIX not in output
                    or message not in output
                ):
                    failures.append(f"accepted {label}:\n{audited.stdout}")

    if failures:
        raise SystemExit("\n".join(failures))
    print(
        f"architecture audit accepted {accepted} and rejected {rejected} "
        "configured graphs"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
