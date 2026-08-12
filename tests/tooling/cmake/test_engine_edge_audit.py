#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise concrete engine link and include boundaries."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import concurrent.futures
import os
import re
import subprocess
import tempfile
from pathlib import Path


REPRESENTATIVES = {
    "native": ("uml_arm64_jit_executor", "jit"),
    "gadget": ("uml_arm64_gadgets", "gadgets"),
    "interpreter": ("uml_arm64_interpreter_engine", "interpreter"),
}
INTERFACE_REPRESENTATIVES = {
    "native": "uml_cpu_jit_component",
    "gadget": "uml_cpu_gadget_component",
    "interpreter": "uml_cpu_interpreter_component",
}
ALL_KIND_REPRESENTATIVES = {
    **INTERFACE_REPRESENTATIVES,
    "shared": "uml_cpu_contract",
    "mixed": "uml_cpu_engine_component",
}
EDGE_KINDS = (
    "link",
    "link-transitive",
    "interface-link",
    "interface-transitive",
    "include-relative",
    "include-target-owned-source",
    "include-basename",
    "include-transitive",
)
INCLUDE_EDGE_KINDS = (
    "include-relative",
    "include-target-owned-source",
    "include-basename",
    "include-transitive",
)
CLASSIFIER_EDGE_KINDS = ("link", "include")


def run(
    command: list[str],
    *,
    cwd: Path | None = None,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=cwd,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )


def run_audit(
    cmake: Path,
    audit: Path,
    build_root: Path,
    manifest: Path,
    *,
    forbidden_fixtures: Path | None = None,
    allowed_fixtures: Path | None = None,
) -> subprocess.CompletedProcess[str]:
    command = [
        str(cmake),
        f"-DBUILD_ROOT={build_root}",
        f"-DARCHITECTURE_MANIFEST={manifest}",
    ]
    if forbidden_fixtures is not None:
        command.append(f"-DENGINE_EDGE_FIXTURES={forbidden_fixtures}")
    if allowed_fixtures is not None:
        command.append(
            f"-DENGINE_EDGE_ALLOWED_FIXTURES={allowed_fixtures}"
        )
    command.extend(("-P", str(audit)))
    return run(command)


def cmake_path(path: Path) -> str:
    return path.resolve().as_posix().replace('"', '\\"')


def write_fixture(
    root: Path,
    architecture_module: Path,
    source_kind: str,
    destination_kind: str,
    edge_kind: str,
) -> tuple[Path, Path]:
    if edge_kind in ("interface-link", "interface-transitive"):
        source_target = INTERFACE_REPRESENTATIVES[source_kind]
        destination_target = INTERFACE_REPRESENTATIVES[destination_kind]
        source_root = root / "source"
        build_root = root / "build"
        source_root.mkdir(parents=True)
        (source_root / "anchor.cpp").write_text(
            "int fixture_anchor_value() { return 1; }\n", encoding="utf-8"
        )
        cmake_lists = [
            "cmake_minimum_required(VERSION 3.27)",
            "project(uml_engine_interface_fixture LANGUAGES CXX)",
            f'include("{cmake_path(architecture_module)}")',
            f"add_library({destination_target} INTERFACE)",
            f"add_library({source_target} INTERFACE)",
            f"uml_declare_architecture_target({destination_target} cpu "
            f"{destination_kind})",
            f"uml_declare_architecture_target({source_target} cpu "
            f"{source_kind})",
        ]
        if edge_kind == "interface-link":
            cmake_lists.append(
                f"target_link_libraries({source_target} INTERFACE "
                f"{destination_target})"
            )
        else:
            cmake_lists.extend(
                [
                    "add_library(neutral_engine_carrier INTERFACE)",
                    "uml_declare_architecture_target(neutral_engine_carrier "
                    "cpu shared)",
                    f"target_link_libraries({source_target} INTERFACE "
                    "neutral_engine_carrier)",
                    "target_link_libraries(neutral_engine_carrier INTERFACE "
                    f"{destination_target})",
                ]
            )
        cmake_lists.extend(
            [
                "add_library(fixture_anchor STATIC anchor.cpp)",
                "uml_write_architecture_manifest()",
                "",
            ]
        )
        (source_root / "CMakeLists.txt").write_text(
            "\n".join(cmake_lists), encoding="utf-8"
        )
        query = build_root / ".cmake" / "api" / "v1" / "query"
        query.mkdir(parents=True)
        (query / "codemodel-v2").touch()
        return source_root, build_root

    source_target, source_directory = REPRESENTATIVES[source_kind]
    destination_target, destination_directory = REPRESENTATIVES[destination_kind]
    source_root = root / "source"
    build_root = root / "build"
    source_domain = source_root / "darwin" / "cpu" / "arm64" / source_directory
    if edge_kind == "include-target-owned-source":
        # Engine ownership comes from the configured target, not from a
        # directory-name convention. Keep this source outside every concrete
        # engine directory to prove the File API audit follows that metadata.
        source_domain = source_root / "darwin" / "cpu" / "arm64" / "relocated"
    destination_domain = (
        source_root / "darwin" / "cpu" / "arm64" / destination_directory
    )
    source_domain.mkdir(parents=True)
    destination_domain.mkdir(parents=True)

    destination_type = "STATIC"
    edge_commands: list[str] = []
    include_line = ""
    if edge_kind == "link":
        edge_commands.append(
            f"target_link_libraries({source_target} PRIVATE {destination_target})"
        )
    elif edge_kind == "link-transitive":
        edge_commands.extend(
            [
                "add_library(foreign_link_carrier INTERFACE)",
                "uml_declare_architecture_target(foreign_link_carrier "
                "cpu shared)",
                "target_link_libraries(foreign_link_carrier INTERFACE "
                f"{destination_target})",
                f"target_link_libraries({source_target} PRIVATE "
                "foreign_link_carrier)",
            ]
        )
    elif edge_kind == "include-relative":
        include_line = f'#include "../{destination_directory}/owned.hpp"\n'
    elif edge_kind == "include-target-owned-source":
        include_line = f'#include "../{destination_directory}/owned.hpp"\n'
    elif edge_kind == "include-basename":
        include_line = '#include "owned.hpp"\n'
        edge_commands.append(
            f"target_include_directories({source_target} PRIVATE "
            f'"${{CMAKE_CURRENT_SOURCE_DIR}}/darwin/cpu/arm64/'
            f'{destination_directory}")'
        )
    elif edge_kind == "include-transitive":
        include_line = '#include "owned.hpp"\n'
        edge_commands.extend(
            [
                "add_library(foreign_include_carrier INTERFACE)",
                "uml_declare_architecture_target(foreign_include_carrier "
                "cpu shared)",
                "target_include_directories(foreign_include_carrier INTERFACE "
                f'"${{CMAKE_CURRENT_SOURCE_DIR}}/darwin/cpu/arm64/'
                f'{destination_directory}")',
                f"target_link_libraries({source_target} PRIVATE "
                "foreign_include_carrier)",
            ]
        )
    else:  # pragma: no cover - the constant table above is closed.
        raise ValueError(edge_kind)

    (source_domain / "source.cpp").write_text(
        include_line + "int fixture_source_value() { return owned_value; }\n"
        if include_line
        else "int fixture_source_value() { return 1; }\n",
        encoding="utf-8",
    )
    (destination_domain / "destination.cpp").write_text(
        "int fixture_destination_value() { return 2; }\n",
        encoding="utf-8",
    )
    (destination_domain / "owned.hpp").write_text(
        "#pragma once\ninline constexpr int owned_value = 3;\n",
        encoding="utf-8",
    )

    source_file = (
        "darwin/cpu/arm64/relocated/source.cpp"
        if edge_kind == "include-target-owned-source"
        else f"darwin/cpu/arm64/{source_directory}/source.cpp"
    )
    destination_file = (
        f"darwin/cpu/arm64/{destination_directory}/destination.cpp"
    )
    cmake_lists = [
        "cmake_minimum_required(VERSION 3.27)",
        "project(uml_engine_edge_fixture LANGUAGES CXX)",
        f'include("{cmake_path(architecture_module)}")',
        f"add_library({destination_target} {destination_type} {destination_file})",
        f"add_library({source_target} STATIC {source_file})",
        f"uml_declare_architecture_target({destination_target} cpu "
        f"{destination_kind})",
        f"uml_declare_architecture_target({source_target} cpu "
        f"{source_kind})",
        *edge_commands,
        "uml_write_architecture_manifest()",
        "",
    ]
    (source_root / "CMakeLists.txt").write_text(
        "\n".join(cmake_lists), encoding="utf-8"
    )
    query = build_root / ".cmake" / "api" / "v1" / "query"
    query.mkdir(parents=True)
    (query / "codemodel-v2").touch()
    return source_root, build_root


def check_missing_engine_metadata(cmake: Path, module: Path, root: Path) -> None:
    source_root = root / "missing-engine-metadata" / "source"
    build_root = root / "missing-engine-metadata" / "build"
    source_root.mkdir(parents=True)
    (source_root / "CMakeLists.txt").write_text(
        "\n".join(
            [
                "cmake_minimum_required(VERSION 3.27)",
                "project(uml_missing_engine_metadata LANGUAGES CXX)",
                f'include("{cmake_path(module)}")',
                "add_library(unclassified_cpu_target INTERFACE)",
                "uml_declare_architecture_target(unclassified_cpu_target cpu)",
                "",
            ]
        ),
        encoding="utf-8",
    )
    configured = run(
        [str(cmake), "-S", str(source_root), "-B", str(build_root)]
    )
    normalized_output = re.sub(r"\s+", " ", configured.stdout)
    if (
        configured.returncode == 0
        or "requires an explicit engine kind" not in normalized_output
    ):
        raise SystemExit(
            "CPU target without engine ownership metadata was accepted:\n"
            f"{configured.stdout}"
        )


def check_kind_direction_matrix(
    cmake: Path,
    audit: Path,
    build_root: Path,
    manifest: Path,
    root: Path,
) -> None:
    """Exercise the complete shared/concrete/mixed dependency policy once."""
    forbidden_rows: list[str] = []
    allowed_rows: list[str] = []
    for source_kind in ALL_KIND_REPRESENTATIVES:
        for destination_kind in ALL_KIND_REPRESENTATIVES:
            allowed = (
                source_kind == "mixed"
                or destination_kind == "shared"
                or source_kind == destination_kind
            )
            for edge_kind in CLASSIFIER_EDGE_KINDS:
                row = (
                    f"{ALL_KIND_REPRESENTATIVES[source_kind]}\t"
                    f"{ALL_KIND_REPRESENTATIVES[destination_kind]}\t"
                    f"{edge_kind}"
                )
                (allowed_rows if allowed else forbidden_rows).append(row)

    forbidden_path = root / "forbidden-kind-edges.tsv"
    allowed_path = root / "allowed-kind-edges.tsv"
    forbidden_path.write_text("\n".join(forbidden_rows) + "\n", encoding="utf-8")
    allowed_path.write_text("\n".join(allowed_rows) + "\n", encoding="utf-8")
    result = run_audit(
        cmake,
        audit,
        build_root,
        manifest,
        forbidden_fixtures=forbidden_path,
        allowed_fixtures=allowed_path,
    )
    if result.returncode != 0:
        raise SystemExit(f"engine-kind classifier matrix failed:\n{result.stdout}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", type=Path, required=True)
    parser.add_argument("--audit", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    options = parser.parse_args()

    architecture_module = options.audit.parent.parent / "cmake" / "UmlArchitecture.cmake"
    fixtures: list[tuple[int, str, Path, Path]] = []
    with tempfile.TemporaryDirectory(prefix="uml-engine-edge-") as directory:
        temporary_root = Path(directory)
        check_missing_engine_metadata(
            options.cmake, architecture_module, temporary_root
        )
        check_kind_direction_matrix(
            options.cmake,
            options.audit,
            options.build_root,
            options.manifest,
            temporary_root,
        )
        for source_kind in REPRESENTATIVES:
            for destination_kind in REPRESENTATIVES:
                if source_kind == destination_kind:
                    continue
                for edge_kind in EDGE_KINDS:
                    # Include resolution is the direction-sensitive mechanism
                    # this fixture exists to prove, so exercise all three
                    # include shapes in every concrete-engine direction. The
                    # exact kind-direction policy is covered above in one
                    # audit process; one real direction per remaining File
                    # API mechanism proves that the edge is discovered.
                    if (
                        edge_kind not in INCLUDE_EDGE_KINDS
                        and (source_kind, destination_kind)
                        != ("native", "gadget")
                    ):
                        continue
                    label = f"{source_kind}->{destination_kind} {edge_kind}"
                    fixture_index = len(fixtures)
                    fixture_root = temporary_root / f"fixture-{fixture_index}"
                    source_root, build_root = write_fixture(
                        fixture_root,
                        architecture_module,
                        source_kind,
                        destination_kind,
                        edge_kind,
                    )
                    fixtures.append(
                        (fixture_index, label, source_root, build_root)
                    )

        def check_fixture(
            fixture: tuple[int, str, Path, Path],
        ) -> tuple[int, str, str | None]:
            fixture_index, label, source_root, build_root = fixture
            configured = run(
                [
                    str(options.cmake),
                    "-S",
                    str(source_root),
                    "-B",
                    str(build_root),
                ]
            )
            if configured.returncode != 0:
                return (
                    fixture_index,
                    label,
                    f"could not configure real {label} fixture:\n"
                    f"{configured.stdout}",
                )
            rejected = run_audit(
                options.cmake,
                options.audit,
                build_root,
                build_root / "uml-architecture-targets.tsv",
            )
            if (
                rejected.returncode == 0
                or "Forbidden cross-engine" not in rejected.stdout
            ):
                return (
                    fixture_index,
                    label,
                    f"accepted real {label} fixture:\n{rejected.stdout}",
                )
            return fixture_index, label, None

        # Each negative owns an isolated source/build pair. Configure and
        # inspect those real graphs concurrently so adding edge kinds does not
        # make the structural gate scale linearly with CMake startup latency.
        worker_count = min(len(fixtures), max(2, min(20, os.cpu_count() or 2)))
        with concurrent.futures.ThreadPoolExecutor(
            max_workers=worker_count
        ) as executor:
            results = list(executor.map(check_fixture, fixtures))
        failures = [result for result in results if result[2] is not None]
        if failures:
            failures.sort()
            raise SystemExit("\n".join(result[2] or "" for result in failures))

    print(f"symmetric engine-edge audit rejected {len(fixtures)} real fixtures")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
