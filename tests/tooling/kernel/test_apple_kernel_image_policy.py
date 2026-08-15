#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pin the canonical 4/16 KiB kernels and selectable signed iOS runtimes."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


CANONICAL_BUILD_16K = "/Volumes/Kernel/build-16k"
CANONICAL_BUILD_4K = "/Volumes/Kernel/build-4k"
CANONICAL_IMAGE_16K = f"{CANONICAL_BUILD_16K}/vmlinux"
CANONICAL_IMAGE_4K = f"{CANONICAL_BUILD_4K}/vmlinux"
OBSOLETE_BUILD = "build-ios-device-16k"
ELF_HEADER = struct.Struct("<16sHHIQQQIHHHHHH")
PROGRAM_HEADER = struct.Struct("<IIQQQQQQ")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def read(path: Path) -> str:
    return path.resolve().read_text(encoding="utf-8")


def kernel_release(metadata: str) -> str:
    prefix = "UML_KERNEL_RELEASE_VERSION='"
    rows = [
        row[len(prefix) : -1]
        for row in metadata.splitlines()
        if row.startswith(prefix) and row.endswith("'")
    ]
    require(len(rows) == 1, "kernel metadata lacks one release identity")
    require(
        all(component.isdigit() for component in rows[0].split(".")),
        "kernel release identity is not numeric",
    )
    return rows[0]


def synthetic_elf(
    *,
    late_nonexecutable_overlap: bool,
    reverse_overlap_order: bool = False,
    overlap_writable: bool = False,
) -> bytes:
    image = bytearray(0x10000)
    programs = [
        (1, 4, 0, 0xC00000000, 0xC00000000, 0x1000, 0x1000, 0x4000),
        (
            1,
            5,
            0x4000,
            0xC00004000,
            0xC00004000,
            0x1800 if late_nonexecutable_overlap else 0x1000,
            0x1800 if late_nonexecutable_overlap else 0x1000,
            0x4000,
        ),
        (
            1,
            6 if overlap_writable else 4,
            0x6000 if late_nonexecutable_overlap else 0x8000,
            0xC00006000 if late_nonexecutable_overlap else 0xC00008000,
            0xC00006000 if late_nonexecutable_overlap else 0xC00008000,
            0x1000,
            0x1000,
            0x4000,
        ),
    ]
    if reverse_overlap_order:
        programs[1:] = reversed(programs[1:])
    identity = b"\x7fELF\x02\x01\x01" + bytes(9)
    ELF_HEADER.pack_into(
        image,
        0,
        identity,
        3,
        183,
        1,
        0xC00004000,
        ELF_HEADER.size,
        0,
        0,
        ELF_HEADER.size,
        PROGRAM_HEADER.size,
        len(programs),
        0,
        0,
        0,
    )
    for index, program in enumerate(programs):
        PROGRAM_HEADER.pack_into(
            image,
            ELF_HEADER.size + index * PROGRAM_HEADER.size,
            *program,
        )
    return bytes(image)


def verify_carrier_tool(verifier: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="kernarium-carrier-policy-") as directory:
        valid = Path(directory) / "valid-vmlinux"
        invalid = Path(directory) / "invalid-vmlinux"
        reverse_invalid = Path(directory) / "reverse-invalid-vmlinux"
        valid.write_bytes(synthetic_elf(late_nonexecutable_overlap=False))
        invalid.write_bytes(synthetic_elf(late_nonexecutable_overlap=True))
        reverse_invalid.write_bytes(
            synthetic_elf(
                late_nonexecutable_overlap=True,
                reverse_overlap_order=True,
                overlap_writable=True,
            )
        )
        accepted = subprocess.run(
            [sys.executable, "-B", str(verifier), "--image", str(valid)],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        require(accepted.returncode == 0, f"safe carrier rejected:\n{accepted.stderr}")
        unsafe_cases = (
            (
                invalid,
                "non-executable PT_LOAD follows signed executable bytes",
            ),
            (
                reverse_invalid,
                "signed executable PT_LOAD shares a host page with writable",
            ),
        )
        for unsafe, diagnostic in unsafe_cases:
            rejected = subprocess.run(
                [sys.executable, "-B", str(verifier), "--image", str(unsafe)],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            require(
                rejected.returncode != 0,
                f"mixed loader page was accepted: {unsafe.name}",
            )
            require(
                diagnostic in rejected.stderr,
                "carrier verifier rejected the hazard for the wrong reason:\n"
                f"{rejected.stderr}",
            )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--presets", type=Path, required=True)
    parser.add_argument("--kernel-cmake", type=Path, required=True)
    parser.add_argument("--runtime-cmake", type=Path, required=True)
    parser.add_argument("--runtime-source", type=Path, required=True)
    parser.add_argument("--runtime-loader", type=Path, required=True)
    parser.add_argument("--carrier-verifier", type=Path, required=True)
    parser.add_argument("--linker-patch", type=Path, required=True)
    parser.add_argument("--metadata", type=Path, required=True)
    parser.add_argument("--package-script", type=Path, required=True)
    parser.add_argument("--xcode-project", type=Path, required=True)
    parser.add_argument("--simulator-script", type=Path, required=True)
    parser.add_argument("--macos-app", type=Path, required=True)
    parser.add_argument("--toolchain-env", type=Path, required=True)
    arguments = parser.parse_args()

    presets_source = read(arguments.presets)
    presets = json.loads(presets_source)
    configurations = {
        preset["name"]: preset for preset in presets["configurePresets"]
    }
    require(
        configurations["macos-integration"]["cacheVariables"]
        ["UML_KERNEL_BUILD_DIR"]
        == CANONICAL_BUILD_16K,
        "macOS integration does not build and embed the canonical 16 KiB kernel",
    )
    require(
        configurations["macos-integration-4k"]["cacheVariables"]
        ["UML_KERNEL_BUILD_DIR"]
        == CANONICAL_BUILD_4K,
        "macOS 4 KiB integration does not use its canonical kernel",
    )
    require(
        "UML_KERNEL_JOBS"
        not in configurations["macos-integration"]["cacheVariables"],
        "macOS integration overrides the all-host-CPU kernel build default",
    )
    for platform in ("device", "simulator"):
        profile_16k = configurations[f"kernarium-ios-{platform}"]
        profile_4k = configurations[f"kernarium-ios-{platform}-4k"]
        require(
            profile_16k["cacheVariables"]["UML_KERNARIUM_KERNEL_IMAGE"]
            == CANONICAL_IMAGE_16K
            and profile_16k["cacheVariables"]["UML_GUEST_16K_PAGES"] is True,
            f"{platform} 16 KiB runtime profile is not canonical",
        )
        require(
            profile_4k["cacheVariables"]["UML_KERNARIUM_KERNEL_IMAGE"]
            == CANONICAL_IMAGE_4K
            and profile_4k["cacheVariables"]["UML_GUEST_16K_PAGES"] is False,
            f"{platform} 4 KiB runtime profile is not canonical",
        )
        require(
            profile_16k["binaryDir"] != profile_4k["binaryDir"],
            f"{platform} page profiles share one CMake build tree",
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
        "${UML_KERNEL_JOBS:=2}" not in toolchain_env,
        "kernel toolchain retains the former two-job default",
    )

    runtime_cmake = read(arguments.runtime_cmake)
    for contract in (
        CANONICAL_IMAGE_16K,
        CANONICAL_IMAGE_4K,
        "KernariumRuntime_${KERNARIUM_KERNEL_RELEASE_COMPONENT}_${KERNARIUM_KERNEL_PAGE_VARIANT}",
        'OUTPUT_NAME "${KERNARIUM_RUNTIME_FRAMEWORK_NAME}"',
        "verify-signed-kernel-carrier.py",
        "LINKER:-sectcreate,__TEXT,__uml_elf,${UML_KERNARIUM_KERNEL_IMAGE}",
        'LINK_DEPENDS "${UML_KERNARIUM_KERNEL_IMAGE}"',
    ):
        require(contract in runtime_cmake, f"runtime carrier lacks {contract}")

    runtime_source = read(arguments.runtime_source)
    loader_source = read(arguments.runtime_loader)
    require(
        "kernarium_runtime_get_api_v1" in runtime_source
        and "KERNARIUM_RUNTIME_FRAMEWORK_NAME" in runtime_source,
        "runtime does not publish its versioned identity API",
    )
    require(
        "__attribute__((constructor))" not in runtime_source,
        "deferred runtime still owns the pre-main timer constructor",
    )
    for contract in (
        "__attribute__((constructor))",
        "pthread_sigmask(SIG_BLOCK",
        "dlopen(executable.fileSystemRepresentation, RTLD_NOW | RTLD_GLOBAL)",
        'dlsym(handle, "kernarium_runtime_get_api_v1")',
        "selectedPageSize == guestPageSize",
    ):
        require(contract in loader_source, f"application runtime loader lacks {contract}")

    linker_patch = read(arguments.linker_patch)
    require(
        "diff --git a/arch/um/kernel/dyn.lds.S" in linker_patch
        and linker_patch.count("+  . = ALIGN(0x4000);") >= 2
        and "following dynamic" in linker_patch,
        "the architecture topic does not align the early signed-text boundaries",
    )
    verify_carrier_tool(arguments.carrier_verifier)

    metadata = read(arguments.metadata)
    release = kernel_release(metadata)
    release_component = release.replace(".", "_")
    runtime_4k = f"KernariumRuntime_{release_component}_4K"
    runtime_16k = f"KernariumRuntime_{release_component}_16K"
    package_script = read(arguments.package_script)
    for contract in (
        "kernarium-runtime-ios-device-4k",
        "kernarium-runtime-ios-device-16k",
        "kernarium-runtime-ios-simulator-4k",
        "kernarium-runtime-ios-simulator-16k",
        'create_xcframework "$RUNTIME_4K"',
        'create_xcframework "$RUNTIME_16K"',
    ):
        require(contract in package_script, f"runtime packaging lacks {contract}")

    xcode_project = read(arguments.xcode_project)
    for runtime in (runtime_4k, runtime_16k):
        require(
            f"{runtime}.xcframework in Embed Frameworks" in xcode_project,
            f"Xcode does not embed {runtime}",
        )
    require(
        "KernariumRuntime.xcframework" not in xcode_project,
        "Xcode retains the directly linked unversioned runtime",
    )
    frameworks_phase = xcode_project.split(
        "/* Begin PBXFrameworksBuildPhase section */", 1
    )[1].split("/* End PBXFrameworksBuildPhase section */", 1)[0]
    require(
        "KernariumRuntime_" not in frameworks_phase,
        "a selectable runtime is linked for process launch instead of only embedded",
    )

    simulator_script = read(arguments.simulator_script)
    for contract in (
        "UML_KERNARIUM_KERNEL_IMAGE_16K:-" + CANONICAL_IMAGE_16K,
        "UML_KERNARIUM_KERNEL_IMAGE_4K:-" + CANONICAL_IMAGE_4K,
        "cmake --build --preset kernarium-ios-simulator",
        "cmake --build --preset kernarium-ios-simulator-4k",
    ):
        require(contract in simulator_script, f"Simulator launcher lacks {contract}")

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

    print("selectable Apple kernel runtime policy verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
