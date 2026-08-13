#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pin one canonical 16 KiB kernel artifact across Apple products."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import json
from pathlib import Path


CANONICAL_BUILD = "/Volumes/Kernel/build-16k"
CANONICAL_IMAGE = f"{CANONICAL_BUILD}/vmlinux"
OBSOLETE_BUILD = "build-ios-device-16k"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def read(path: Path) -> str:
    return path.resolve().read_text(encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--presets", type=Path, required=True)
    parser.add_argument("--kernel-cmake", type=Path, required=True)
    parser.add_argument("--runtime-cmake", type=Path, required=True)
    parser.add_argument("--simulator-script", type=Path, required=True)
    parser.add_argument("--macos-app", type=Path, required=True)
    parser.add_argument("--toolchain-env", type=Path, required=True)
    arguments = parser.parse_args()

    presets_source = read(arguments.presets)
    presets = json.loads(presets_source)
    configurations = {
        preset["name"]: preset
        for preset in presets["configurePresets"]
    }
    require(
        configurations["macos-integration"]["cacheVariables"]
        ["UML_KERNEL_BUILD_DIR"] == CANONICAL_BUILD,
        "macOS integration does not build and embed the canonical 16 KiB kernel",
    )
    require(
        "UML_KERNEL_JOBS"
        not in configurations["macos-integration"]["cacheVariables"],
        "macOS integration overrides the all-host-CPU kernel build default",
    )
    for name in ("kernarium-ios-device", "kernarium-ios-simulator"):
        require(
            configurations[name]["cacheVariables"]
            ["UML_KERNARIUM_KERNEL_IMAGE"] == CANONICAL_IMAGE,
            f"{name} does not embed the canonical macOS 16 KiB kernel image",
        )

    kernel_cmake = read(arguments.kernel_cmake)
    require(
        'set(UML_KERNEL_IMAGE "${UML_KERNEL_BUILD_DIR}/vmlinux" CACHE INTERNAL'
        in kernel_cmake,
        "the macOS embedded image is not derived from its selected Kbuild tree",
    )
    require(
        '"Derived Linux UML kernel ELF embedded in the transitional macOS CLI" FORCE)'
        in kernel_cmake,
        "the derived macOS kernel input can be overridden independently",
    )
    require(
        'set(UML_KERNEL_JOBS "" CACHE STRING' in kernel_cmake
        and 'NOT UML_KERNEL_JOBS STREQUAL ""' in kernel_cmake,
        "CMake does not leave kernel parallelism on the online-CPU default",
    )

    toolchain_env = read(arguments.toolchain_env)
    require(
        "getconf _NPROCESSORS_ONLN" in toolchain_env
        and "sysctl -n hw.logicalcpu" in toolchain_env,
        "kernel toolchain defaults do not discover every online host CPU",
    )
    require(
        '${UML_KERNEL_JOBS:=2}' not in toolchain_env,
        "kernel toolchain retains the former two-job default",
    )

    runtime_cmake = read(arguments.runtime_cmake)
    require(
        f'    "{CANONICAL_IMAGE}"\n' in runtime_cmake,
        "KernariumRuntime does not default to the canonical kernel image",
    )
    for contract in (
        "LINKER:-sectcreate,__TEXT,__uml_elf,${UML_KERNARIUM_KERNEL_IMAGE}",
        'LINK_DEPENDS "${UML_KERNARIUM_KERNEL_IMAGE}"',
    ):
        require(contract in runtime_cmake, f"runtime carrier lacks {contract}")

    simulator_script = read(arguments.simulator_script)
    require(
        "UML_KERNARIUM_KERNEL_IMAGE:-" + CANONICAL_IMAGE in simulator_script,
        "the Simulator launcher does not default to the canonical kernel image",
    )

    macos_app = read(arguments.macos_app)
    for declaration in (
        'macos_default_guest_memory{"mem=8G"}',
        'macos_default_guest_cpus{"ncpus=10"}',
    ):
        require(declaration in macos_app, f"macOS app lacks {declaration}")
    for prefix, value in (
        ("mem=", "macos_default_guest_memory"),
        ("ncpus=", "macos_default_guest_cpus"),
    ):
        require(
            f'prepared.kernel, "{prefix}", {value}' in macos_app,
            f"macOS app does not apply {value} only when {prefix} is absent",
        )

    for path, source in (
        (arguments.presets, presets_source),
        (arguments.runtime_cmake, runtime_cmake),
        (arguments.simulator_script, simulator_script),
        (arguments.macos_app, macos_app),
        (arguments.toolchain_env, toolchain_env),
    ):
        require(
            OBSOLETE_BUILD not in source,
            f"obsolete platform-specific kernel build remains in {path}",
        )

    print("shared Apple kernel image policy verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
