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
    cmake: Path, module: Path, root: Path, body: list[str]
) -> subprocess.CompletedProcess[str]:
    source_root = root / "source"
    build_root = root / "build"
    source_root.mkdir(parents=True)
    query = build_root / ".cmake" / "api" / "v1" / "query"
    query.mkdir(parents=True)
    (query / "codemodel-v2").touch()
    module_path = module.resolve().as_posix().replace('"', '\\"')
    (source_root / "CMakeLists.txt").write_text(
        "\n".join(
            [
                "cmake_minimum_required(VERSION 3.27)",
                "project(uml_engine_kind_fixture LANGUAGES NONE)",
                f'include("{module_path}")',
                *body,
                "",
            ]
        ),
        encoding="utf-8",
    )
    return run([str(cmake), "-S", str(source_root), "-B", str(build_root)])


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
                "-> first -> second -> first",
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

    if failures:
        raise SystemExit("\n".join(failures))
    print(
        f"architecture audit accepted {accepted} and rejected {rejected} "
        "configured graphs"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
