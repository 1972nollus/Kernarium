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


OBSOLETE_BUILD = "build-ios-device-16k"
SWIFTTERM_REVISION = "7691f85b222a67a66b58499e1b2647443cf0dda7"
SWIFTTERM_VERSION = "1.18.0"
ELF_HEADER = struct.Struct("<16sHHIQQQIHHHHHH")
PROGRAM_HEADER = struct.Struct("<IIQQQQQQ")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def read(path: Path) -> str:
    return path.resolve().read_text(encoding="utf-8")


def env_value(source: str, key: str) -> str:
    prefix = f"{key}="
    rows = [row[len(prefix):] for row in source.splitlines() if row.startswith(prefix)]
    require(len(rows) == 1, f"metadata lacks one {key}")
    return rows[0].strip("'")


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


def verify_accepted_artifact_cache(artifact_cmake: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="accepted-artifact-cache-") as directory:
        temporary = Path(directory)
        managed = temporary / "managed"
        external = temporary / "external" / "copied-artifact"
        script = temporary / "verify.cmake"
        script.write_text(
            f'''cmake_minimum_required(VERSION 3.27)
include([=[{artifact_cmake.resolve()}]=])

# An older build tree has no default tracker. Its current accepted value is
# retained and becomes the baseline for following later acceptance changes.
set(TEST_MANAGED_ARTIFACT [=[{managed / "accepted-v1"}]=] CACHE PATH "test")
uml_cache_accepted_artifact_path(
    TEST_MANAGED_ARTIFACT [=[{managed / "accepted-v1"}]=] [=[{managed}]=] "test")
if(NOT TEST_MANAGED_ARTIFACT STREQUAL [=[{managed / "accepted-v1"}]=] OR
   NOT TEST_MANAGED_ARTIFACT_ACCEPTED_DEFAULT STREQUAL
       [=[{managed / "accepted-v1"}]=] OR
   NOT TEST_MANAGED_ARTIFACT_FOLLOW_ACCEPTED)
    message(FATAL_ERROR "accepted artifact cache migration failed")
endif()

# An ambiguous pre-tracker value is preserved as an explicit override even
# when it names another publication below the managed root.
set(TEST_MIGRATED_OVERRIDE [=[{managed / "migrated-override"}]=]
    CACHE PATH "test")
uml_cache_accepted_artifact_path(
    TEST_MIGRATED_OVERRIDE [=[{managed / "accepted-v1"}]=]
    [=[{managed}]=] "test"
    LEGACY_DEFAULTS [=[{managed / "recognized-other"}]=])
if(NOT TEST_MIGRATED_OVERRIDE STREQUAL [=[{managed / "migrated-override"}]=] OR
   TEST_MIGRATED_OVERRIDE_FOLLOW_ACCEPTED)
    message(FATAL_ERROR "migrated managed override was replaced")
endif()

# A finite caller-owned legacy default advances on first use and then follows
# later accepted defaults through the normal tracker.
set(TEST_RECOGNIZED_LEGACY [=[{managed / "legacy-v0"}]=]
    CACHE PATH "test")
uml_cache_accepted_artifact_path(
    TEST_RECOGNIZED_LEGACY [=[{managed / "legacy-v1"}]=]
    [=[{managed}]=] "test"
    LEGACY_DEFAULTS [=[{managed / "legacy-v0"}]=])
if(NOT TEST_RECOGNIZED_LEGACY STREQUAL [=[{managed / "legacy-v1"}]=] OR
   NOT TEST_RECOGNIZED_LEGACY_FOLLOW_ACCEPTED)
    message(FATAL_ERROR "recognized legacy default did not advance")
endif()
uml_cache_accepted_artifact_path(
    TEST_RECOGNIZED_LEGACY [=[{managed / "legacy-v2"}]=]
    [=[{managed}]=] "test"
    LEGACY_DEFAULTS [=[{managed / "legacy-v0"}]=])
if(NOT TEST_RECOGNIZED_LEGACY STREQUAL [=[{managed / "legacy-v2"}]=] OR
   NOT TEST_RECOGNIZED_LEGACY_FOLLOW_ACCEPTED)
    message(FATAL_ERROR "migrated legacy default stopped following")
endif()

# Migrate the earlier equality-only tracker by following its prior default.
set(TEST_TRACKER_MIGRATION [=[{managed / "tracked-v1"}]=]
    CACHE PATH "test")
set(TEST_TRACKER_MIGRATION_ACCEPTED_DEFAULT [=[{managed / "tracked-v1"}]=]
    CACHE INTERNAL "test")
uml_cache_accepted_artifact_path(
    TEST_TRACKER_MIGRATION [=[{managed / "tracked-v2"}]=]
    [=[{managed}]=] "test")
if(NOT TEST_TRACKER_MIGRATION STREQUAL [=[{managed / "tracked-v2"}]=] OR
   NOT TEST_TRACKER_MIGRATION_FOLLOW_ACCEPTED)
    message(FATAL_ERROR "equality-only default tracker migration failed")
endif()

# A value which still equals the prior generated default follows acceptance.
uml_cache_accepted_artifact_path(
    TEST_MANAGED_ARTIFACT [=[{managed / "accepted-v2"}]=] [=[{managed}]=] "test")
if(NOT TEST_MANAGED_ARTIFACT STREQUAL [=[{managed / "accepted-v2"}]=] OR
   NOT TEST_MANAGED_ARTIFACT_ACCEPTED_DEFAULT STREQUAL
       [=[{managed / "accepted-v2"}]=] OR
   NOT TEST_MANAGED_ARTIFACT_FOLLOW_ACCEPTED)
    message(FATAL_ERROR "accepted artifact did not follow the new default")
endif()

# An explicit whole-bundle override remains stable even when it is another
# publication below the normal managed artifact root.
set(TEST_MANAGED_ARTIFACT [=[{managed / "local-override"}]=]
    CACHE PATH "test" FORCE)
unset(TEST_MANAGED_ARTIFACT)
uml_cache_accepted_artifact_path(
    TEST_MANAGED_ARTIFACT [=[{managed / "accepted-v3"}]=] [=[{managed}]=] "test")
if(NOT TEST_MANAGED_ARTIFACT STREQUAL [=[{managed / "local-override"}]=] OR
   NOT TEST_MANAGED_ARTIFACT_ACCEPTED_DEFAULT STREQUAL
       [=[{managed / "accepted-v3"}]=] OR
   TEST_MANAGED_ARTIFACT_FOLLOW_ACCEPTED)
    message(FATAL_ERROR "managed whole-bundle override was replaced")
endif()

# An accepted default may later equal the pinned path without changing its
# override identity. It must remain pinned across the following change too.
uml_cache_accepted_artifact_path(
    TEST_MANAGED_ARTIFACT [=[{managed / "local-override"}]=]
    [=[{managed}]=] "test")
if(NOT TEST_MANAGED_ARTIFACT STREQUAL [=[{managed / "local-override"}]=] OR
   NOT TEST_MANAGED_ARTIFACT_ACCEPTED_DEFAULT STREQUAL
       [=[{managed / "local-override"}]=] OR
   TEST_MANAGED_ARTIFACT_FOLLOW_ACCEPTED)
    message(FATAL_ERROR "matching accepted default reclassified an override")
endif()
uml_cache_accepted_artifact_path(
    TEST_MANAGED_ARTIFACT [=[{managed / "accepted-v4"}]=] [=[{managed}]=] "test")
if(NOT TEST_MANAGED_ARTIFACT STREQUAL [=[{managed / "local-override"}]=] OR
   TEST_MANAGED_ARTIFACT_FOLLOW_ACCEPTED)
    message(FATAL_ERROR "managed override followed a later default")
endif()

# Clearing an override opts back into the current accepted default.
set(TEST_MANAGED_ARTIFACT "" CACHE PATH "test" FORCE)
unset(TEST_MANAGED_ARTIFACT)
uml_cache_accepted_artifact_path(
    TEST_MANAGED_ARTIFACT [=[{managed / "accepted-v4"}]=] [=[{managed}]=] "test")
if(NOT TEST_MANAGED_ARTIFACT STREQUAL [=[{managed / "accepted-v4"}]=])
    message(FATAL_ERROR "cleared override did not restore acceptance")
endif()
uml_cache_accepted_artifact_path(
    TEST_MANAGED_ARTIFACT [=[{managed / "accepted-v5"}]=] [=[{managed}]=] "test")
if(NOT TEST_MANAGED_ARTIFACT STREQUAL [=[{managed / "accepted-v5"}]=] OR
   NOT TEST_MANAGED_ARTIFACT_FOLLOW_ACCEPTED)
    message(FATAL_ERROR "restored acceptance did not follow a later default")
endif()

set(TEST_EXTERNAL_ARTIFACT [=[{external}]=] CACHE PATH "test")
uml_cache_accepted_artifact_path(
    TEST_EXTERNAL_ARTIFACT [=[{managed / "accepted-v3"}]=] [=[{managed}]=] "test")
if(NOT TEST_EXTERNAL_ARTIFACT STREQUAL [=[{external}]=])
    message(FATAL_ERROR "external artifact override was replaced")
endif()
''',
            encoding="utf-8",
        )
        checked = subprocess.run(
            ["cmake", "-P", str(script)],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        require(
            checked.returncode == 0,
            "accepted artifact cache policy failed:\n"
            f"{checked.stdout}{checked.stderr}",
        )
        invalid_legacy_defaults = (
            (
                "relative",
                Path("relative-legacy"),
                "Legacy accepted-artifact defaults must be absolute",
            ),
            (
                "outside",
                external,
                "Legacy accepted-artifact defaults must be below the managed root",
            ),
        )
        for name, legacy, diagnostic in invalid_legacy_defaults:
            invalid_script = temporary / f"invalid-{name}.cmake"
            invalid_script.write_text(
                f'''cmake_minimum_required(VERSION 3.27)
include([=[{artifact_cmake.resolve()}]=])
uml_cache_accepted_artifact_path(
    TEST_INVALID_LEGACY [=[{managed / "accepted"}]=]
    [=[{managed}]=] "test" LEGACY_DEFAULTS [=[{legacy}]=])
''',
                encoding="utf-8",
            )
            rejected = subprocess.run(
                ["cmake", "-P", str(invalid_script)],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            require(
                rejected.returncode != 0
                and diagnostic in f"{rejected.stdout}{rejected.stderr}",
                f"invalid {name} legacy default was not rejected correctly:\n"
                f"{rejected.stdout}{rejected.stderr}",
            )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--presets", type=Path, required=True)
    parser.add_argument("--root-cmake", type=Path, required=True)
    parser.add_argument("--kernel-cmake", type=Path, required=True)
    parser.add_argument("--artifact-cmake", type=Path, required=True)
    parser.add_argument("--acceptance-16k", type=Path, required=True)
    parser.add_argument("--acceptance-4k", type=Path, required=True)
    parser.add_argument("--acceptance-x86-4k", type=Path, required=True)
    parser.add_argument("--experiment-16k", type=Path, required=True)
    parser.add_argument("--experiment-4k", type=Path, required=True)
    parser.add_argument("--series-metadata", type=Path, required=True)
    parser.add_argument("--android-kernel-acceptance", type=Path, required=True)
    parser.add_argument("--android-kernel-publisher", type=Path, required=True)
    parser.add_argument("--runtime-cmake", type=Path, required=True)
    parser.add_argument("--static-guest-source", type=Path, required=True)
    parser.add_argument("--runtime-source", type=Path, required=True)
    parser.add_argument("--runtime-header", type=Path, required=True)
    parser.add_argument("--runtime-loader", type=Path, required=True)
    parser.add_argument("--runtime-loader-header", type=Path, required=True)
    parser.add_argument("--content-view", type=Path, required=True)
    parser.add_argument("--serial-console-view", type=Path, required=True)
    parser.add_argument("--carrier-verifier", type=Path, required=True)
    parser.add_argument("--linker-patch", type=Path, required=True)
    parser.add_argument("--package-script", type=Path, required=True)
    parser.add_argument("--xcode-project", type=Path, required=True)
    parser.add_argument("--package-resolved", type=Path, required=True)
    parser.add_argument("--mac-launch-script", type=Path, required=True)
    parser.add_argument("--macos-app-cmake", type=Path, required=True)
    parser.add_argument("--macos-app", type=Path, required=True)
    parser.add_argument("--macos-linux-main", type=Path, required=True)
    parser.add_argument("--android-cmake", type=Path, required=True)
    parser.add_argument("--android-main", type=Path, required=True)
    parser.add_argument("--toolchain-env", type=Path, required=True)
    arguments = parser.parse_args()

    presets_source = read(arguments.presets)
    presets = json.loads(presets_source)
    configurations = {
        preset["name"]: preset for preset in presets["configurePresets"]
    }
    acceptance_16k = read(arguments.acceptance_16k)
    acceptance_4k = read(arguments.acceptance_4k)
    acceptance_x86_4k = read(arguments.acceptance_x86_4k)
    android_kernel_acceptance = read(arguments.android_kernel_acceptance)
    experiment_16k = read(arguments.experiment_16k)
    experiment_4k = read(arguments.experiment_4k)
    series_metadata = read(arguments.series_metadata)
    directory_16k = env_value(
        acceptance_16k, "UML_KERNEL_ARTIFACT_DIRECTORY_NAME"
    )
    directory_4k = env_value(
        acceptance_4k, "UML_KERNEL_ARTIFACT_DIRECTORY_NAME"
    )
    directory_x86_4k = env_value(
        acceptance_x86_4k, "UML_KERNEL_ARTIFACT_DIRECTORY_NAME"
    )
    require(
        configurations["macos-integration"]["cacheVariables"]
        ["UML_KERNEL_PRODUCT"]
        == "mainline-6.18",
        "macOS integration does not select the mainline artifact family",
    )
    require(
        "UML_KERNEL_BUILD_DIR"
        not in configurations["macos-integration"]["cacheVariables"]
        and "UML_KERNEL_BUILD_DIR"
        not in configurations["macos-integration-4k"]["cacheVariables"],
        "macOS integration still exposes a Kbuild output directory",
    )
    experiment_profile_16k = configurations["macos-integration-venus"]
    experiment_profile_4k = configurations["macos-integration-venus-4k"]
    require(
        experiment_profile_16k["cacheVariables"]["UML_KERNEL_PRODUCT"]
        == "mainline-6.18-vulkan-experiment"
        and experiment_profile_16k["cacheVariables"]["UML_APPLE_GPU_PRODUCT"]
        == "apple-gpu-vulkan-experiment",
        "macOS Venus integration does not select both experimental products",
    )
    require(
        experiment_profile_4k["cacheVariables"]["UML_GUEST_16K_PAGES"] is False
        and experiment_profile_16k["binaryDir"]
        == "${sourceDir}/.build/presets/${presetName}"
        and experiment_profile_4k["binaryDir"]
        == "${sourceDir}/.build/presets/${presetName}",
        "macOS Venus page profiles do not use distinct artifact build trees",
    )
    x86_profile = configurations["macos-x86_64"]
    require(
        x86_profile["cacheVariables"]["UML_KERNEL_PRODUCT"]
        == "mainline-6.18"
        and x86_profile["cacheVariables"]["UML_MAINLINE_KERNEL_SLICE"]
        == "x86_64"
        and x86_profile["cacheVariables"]["UML_GUEST_16K_PAGES"] is False
        and x86_profile["cacheVariables"]["UML_BUILD_UNIT_TESTS"] is False
        and x86_profile["cacheVariables"]["UML_BUILD_INTEGRATION_TESTS"]
        is False,
        "macOS x86_64 product is not a fixed mainline 4 KiB slice",
    )
    android_profile = configurations["macos-android17"]
    require(
        android_profile["cacheVariables"]["UML_BUILD_CLI"] is False
        and android_profile["cacheVariables"]["UML_BUILD_ANDROID17_CLI"] is True
        and android_profile["cacheVariables"]["UML_GUEST_16K_PAGES"] is True,
        "Android macOS product is not a distinct 16 KiB-only executable",
    )
    require(
        android_profile["cacheVariables"]["UML_KERNEL_PRODUCT"] == "android-17",
        "Android macOS product does not select the independent ACK artifact",
    )
    require(
        "macos-android17-4k" not in configurations,
        "Android product exposes a forbidden 4 KiB configure preset",
    )
    # The device runtime carries the Vulkan-experiment GPU stack, whose guest
    # reaches host-visible blob memory through the vhost-user shared-memory
    # aperture. That aperture lives in the Vulkan-experiment kernel line, so
    # the two experimental products are selected together, exactly as the
    # macOS Venus integration selects them. KosmicKrisp is a Metal 4 driver and
    # the iOS Simulator exposes no Metal 4 device, so the device is the only
    # Kernarium runtime.
    profile_16k = configurations["kernarium-ios-device"]
    profile_4k = configurations["kernarium-ios-device-4k"]
    require(
        profile_16k["cacheVariables"]["UML_KERNEL_PRODUCT"]
        == "mainline-6.18-vulkan-experiment"
        and profile_16k["cacheVariables"]["UML_GUEST_16K_PAGES"] is True,
        "device 16 KiB runtime profile does not select "
        "mainline-6.18-vulkan-experiment",
    )
    require(
        profile_16k["cacheVariables"]["UML_APPLE_GPU_PRODUCT"]
        == "apple-gpu-vulkan-experiment",
        "the device runtime does not select both experimental products",
    )
    require(
        profile_4k["cacheVariables"]["UML_GUEST_16K_PAGES"] is False
        and "UML_KERNARIUM_KERNEL_IMAGE" not in profile_4k["cacheVariables"],
        "device 4 KiB runtime profile bypasses artifact selection",
    )
    for suffix in ("", "-4k"):
        require(
            f"kernarium-ios-device-venus{suffix}" not in configurations,
            "device retains a Vulkan-experiment runtime preset",
        )
    require(
        profile_16k["binaryDir"] != profile_4k["binaryDir"],
        "device page profiles share one CMake build tree",
    )
    for suffix in ("", "-4k", "-android"):
        require(
            f"kernarium-ios-simulator{suffix}" not in configurations,
            "the iOS Simulator retains a Kernarium runtime preset",
        )

    root_cmake = read(arguments.root_cmake)
    for contract in (
        "UML_X86_64_USERSPACE_ONLY",
        "DARWIN_UML_X86_64_USERSPACE_ONLY",
        "The fixed x86_64 userspace product does not use the arm64 unit or ",
        "The fixed x86_64 userspace slice requires the x86_64 gadget backend",
    ):
        require(contract in root_cmake, f"root product selection lacks {contract}")
    backend_selection_start = "if(NOT UML_GUEST_16K_PAGES AND"
    require(
        backend_selection_start in root_cmake,
        "root product selection lacks the 4 KiB x86_64 backend gate",
    )
    backend_selection = root_cmake.split(backend_selection_start, 1)[1].split(
        "set(UML_BUILD_X86_64_GUEST ON)", 1
    )[0]
    require(
        'UML_MAINLINE_KERNEL_SLICE STREQUAL "x86_64"' in backend_selection
        and "UML_BUILD_UNIT_TESTS" in backend_selection
        and "UML_BUILD_INTEGRATION_TESTS" not in backend_selection,
        "x86_64 backend selection no longer isolates regular arm64 integration",
    )
    static_guest_source = read(arguments.static_guest_source)
    fixed_dispatch = static_guest_source.split(
        "#if DARWIN_UML_X86_64_USERSPACE_ONLY", 1
    )[1].split("#elif DARWIN_UML_X86_64_GUEST", 1)[0]
    require(
        "return run_x86_guest<Kind>" in fixed_dispatch
        and "mutable_registers.padding" not in fixed_dispatch,
        "fixed x86_64 product still selects its userspace ISA per task",
    )

    kernel_cmake = read(arguments.kernel_cmake)
    artifact_cmake = read(arguments.artifact_cmake)
    verify_accepted_artifact_cache(arguments.artifact_cmake)
    for contract in (
        "uml_import_kernel_artifact",
        "UML_MAINLINE_618_KERNEL_ARTIFACT_DIR",
        "UML_ACTIVE_KERNEL_ARTIFACT_TARGET",
        "UML_MAINLINE_KERNEL_SLICE",
        "accepted-16k.env",
        "accepted-4k.env",
        "vulkan-experiment-16k.env",
        "vulkan-experiment-4k.env",
        "mainline-6.18-vulkan-experiment",
        "slices/x86_64/artifacts/accepted-4k.env",
        "mainline-6.18-4k-20260830-binfmt-xstate-a8fdb7c4",
        "mainline-6.18-16k-20260830-binfmt-xstate-a8fdb7c4",
        "The x86_64 mainline kernel slice supports only 4 KiB guest pages",
        "Accepted mainline kernel slice",
    ):
        require(contract in kernel_cmake, f"kernel importer lacks {contract}")
    for forbidden in (
        "add_custom_command",
        "build-kernel.sh",
        "prepare-source.sh",
        "source-location.conf",
        "UML_KERNEL_BUILD_DIR",
        "UML_KERNEL_SOURCE_DIR",
        "UML_KERNEL_JOBS",
    ):
        require(
            forbidden not in kernel_cmake,
            f"Apple kernel importer retains producer operation {forbidden}",
        )
    for contract in (
        "verify-kernel-artifact.py",
        "--compatible",
        "EXPECTED_SLICE",
        "--expected-slice",
        "EXPECTED_RELEASE_SERIES",
        "--expected-release-series",
        "--abi-metadata",
        "add_library(\"${ARTIFACT_TARGET}\" INTERFACE)",
        "UML_KERNEL_IMAGE",
        "UML_KERNEL_MODULES_BUILTIN",
        "UML_ARTIFACT_SLICE",
        "CMAKE_CONFIGURE_DEPENDS",
        "uml_cache_accepted_artifact_path",
        "LEGACY_DEFAULTS",
    ):
        require(contract in artifact_cmake, f"artifact CMake lacks {contract}")
    require(
        env_value(acceptance_16k, "UML_KERNEL_ARTIFACT_SCHEMA") in ("2", "3")
        and env_value(acceptance_16k, "UML_KERNEL_ARTIFACT_SLICE") == "arm64"
        and env_value(acceptance_16k, "UML_KERNEL_ARTIFACT_PAGE_SIZE") == "16384"
        and directory_16k.startswith("mainline-6.18-arm64-16k-"),
        "16 KiB artifact acceptance is malformed",
    )
    require(
        env_value(acceptance_4k, "UML_KERNEL_ARTIFACT_SCHEMA") in ("2", "3")
        and env_value(acceptance_4k, "UML_KERNEL_ARTIFACT_SLICE") == "arm64"
        and env_value(acceptance_4k, "UML_KERNEL_ARTIFACT_PAGE_SIZE") == "4096"
        and directory_4k.startswith("mainline-6.18-arm64-4k-"),
        "4 KiB artifact acceptance is malformed",
    )
    experiment_directory_16k = env_value(
        experiment_16k, "UML_KERNEL_ARTIFACT_DIRECTORY_NAME"
    )
    experiment_directory_4k = env_value(
        experiment_4k, "UML_KERNEL_ARTIFACT_DIRECTORY_NAME"
    )
    require(
        env_value(experiment_16k, "UML_KERNEL_ARTIFACT_SCHEMA") in ("2", "3")
        and env_value(experiment_16k, "UML_KERNEL_ARTIFACT_SLICE") == "arm64"
        and env_value(experiment_16k, "UML_KERNEL_ARTIFACT_PAGE_SIZE") == "16384"
        and experiment_directory_16k.startswith("mainline-6.18-arm64-16k-"),
        "16 KiB Vulkan experiment descriptor is malformed",
    )
    require(
        env_value(experiment_4k, "UML_KERNEL_ARTIFACT_SCHEMA") in ("2", "3")
        and env_value(experiment_4k, "UML_KERNEL_ARTIFACT_SLICE") == "arm64"
        and env_value(experiment_4k, "UML_KERNEL_ARTIFACT_PAGE_SIZE") == "4096"
        and experiment_directory_4k.startswith("mainline-6.18-arm64-4k-"),
        "4 KiB Vulkan experiment descriptor is malformed",
    )
    for key in (
        "UML_KERNEL_ARTIFACT_FAMILY",
        "UML_KERNEL_ARTIFACT_RELEASE",
        "UML_KERNEL_ARTIFACT_FINAL_TREE",
        "UML_KERNEL_ARTIFACT_ABI_VERSION",
        "UML_KERNEL_ARTIFACT_ABI_FINGERPRINT",
    ):
        require(
            env_value(experiment_16k, key) == env_value(experiment_4k, key),
            f"Vulkan experiment page profiles disagree on {key}",
        )
    # The experiment is the product built from the series this tree carries, so
    # its descriptors must name that series rather than a stale publication.
    require(
        env_value(experiment_16k, "UML_KERNEL_ARTIFACT_FINAL_TREE")
        == env_value(series_metadata, "UML_KERNEL_FINAL_TREE")
        and env_value(experiment_16k, "UML_KERNEL_ARTIFACT_ABI_VERSION")
        == env_value(series_metadata, "UML_KERNEL_ABI_VERSION")
        and env_value(experiment_16k, "UML_KERNEL_ARTIFACT_ABI_FINGERPRINT")
        == env_value(series_metadata, "UML_KERNEL_ABI_FINGERPRINT"),
        "Vulkan experiment descriptors do not name the maintained series",
    )
    require(
        env_value(acceptance_x86_4k, "UML_KERNEL_ARTIFACT_SCHEMA") in ("2", "3")
        and env_value(acceptance_x86_4k, "UML_KERNEL_ARTIFACT_SLICE")
        == "x86_64"
        and env_value(acceptance_x86_4k, "UML_KERNEL_ARTIFACT_PAGE_SIZE")
        == "4096"
        and directory_x86_4k.startswith("mainline-6.18-x86_64-4k-"),
        "x86-64 4 KiB artifact acceptance is malformed",
    )
    require(
        env_value(android_kernel_acceptance, "UML_KERNEL_ARTIFACT_FAMILY")
        == "android-6.18"
        and env_value(
            android_kernel_acceptance, "UML_KERNEL_ARTIFACT_SCHEMA"
        ) in ("2", "3")
        and env_value(
            android_kernel_acceptance, "UML_KERNEL_ARTIFACT_SLICE"
        ) == "arm64"
        and env_value(
            android_kernel_acceptance, "UML_KERNEL_ARTIFACT_PAGE_SIZE"
        ) == "16384"
        and env_value(
            android_kernel_acceptance, "UML_KERNEL_ARTIFACT_DIRECTORY_NAME"
        ).startswith("android-6.18-arm64-16k-"),
        "Android ACK artifact acceptance is malformed",
    )
    android_kernel_publisher = read(arguments.android_kernel_publisher)
    for contract in (
        'family = "android-6.18"',
        'kernel_slice = "arm64"',
        '"slice": kernel_slice',
        'verify_config(source_paths["kernel/config"], page_size, kernel_slice)',
        "verify_artifact(",
        'page_size = 16384',
        'if name.endswith(".ko")',
        "dedicated x86_64 Linux builder",
        "verify_kernel_config(staging, android_root, enforce_tracked_policy=True)",
    ):
        require(
            contract in android_kernel_publisher,
            f"Android ACK artifact publisher lacks {contract}",
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
        "UML_ACTIVE_KERNEL_ARTIFACT_TARGET",
        "UML_ARTIFACT_FAMILY",
        "UML_KERNEL_PAGE_SIZE",
        'KERNARIUM_KERNEL_RELEASE_SERIES "6.18"',
        "KRuntime_${KERNARIUM_KERNEL_RELEASE_COMPONENT}_${KERNARIUM_KERNEL_PAGE_VARIANT}",
        "KRuntime_ACK_${KERNARIUM_KERNEL_RELEASE_COMPONENT}_${KERNARIUM_KERNEL_PAGE_VARIANT}",
        'OUTPUT_NAME "${KERNARIUM_RUNTIME_FRAMEWORK_NAME}"',
        "verify-signed-kernel-carrier.py",
        "LINKER:-sectcreate,__TEXT,__uml_elf,${_kernarium_kernel_image}",
        'LINK_DEPENDS "${_kernarium_kernel_image}"',
        "BUILD_WITH_INSTALL_RPATH TRUE",
        'INSTALL_RPATH "@loader_path/.."',
        "INSTALL_RPATH_USE_LINK_PATH FALSE",
    ):
        require(contract in runtime_cmake, f"runtime carrier lacks {contract}")
    for forbidden in (
        "UML_KERNARIUM_KERNEL_IMAGE",
        "/Volumes/Kernel/build-16k",
        "/Volumes/Kernel/build-4k",
        "UML_ANDROID17_ROOTFS_RELEASE_TARGET",
        "UML_ANDROID_ROOTFS_RELEASE_MANIFEST",
        "LINKER:-sectcreate,__DATA,__uml_rootfs",
        "KERNARIUM_ANDROID_BASE_DISK_SIZE",
    ):
        require(
            forbidden not in runtime_cmake,
            f"runtime carrier retains direct kernel input {forbidden}",
        )

    runtime_source = read(arguments.runtime_source)
    runtime_header = read(arguments.runtime_header)
    loader_source = read(arguments.runtime_loader)
    loader_header = read(arguments.runtime_loader_header)
    require(
        "kernarium_runtime_get_api_v8" in runtime_source
        and "KERNARIUM_RUNTIME_FRAMEWORK_NAME" in runtime_source,
        "runtime does not publish its versioned identity API",
    )
    for contract in (
        "boot->android_disk_path",
        'std::string {adbAuthorizationPrefix} + "1"',
        '"ubd0=" + androidDisk.string()',
        '"initrd=" + androidDisk.string()',
    ):
        require(contract in runtime_source, f"iOS Android runtime lacks {contract}")
    for forbidden in (
        "boot->android_manifest_path",
        "boot->android_base_disk_path",
        "boot->android_instance_path",
        "uml_android_rootfs_manifest_start",
        "KERNARIUM_ANDROID_BASE_DISK_SIZE",
        "userdata.cow",
    ):
        require(
            forbidden not in runtime_source,
            f"runtime retains rootfs identity check {forbidden}",
        )
    require(
        "__attribute__((constructor))" not in runtime_source,
        "deferred runtime still owns the pre-main timer constructor",
    )
    for contract in (
        "#define KERNARIUM_RUNTIME_API_VERSION 8U",
        "uint32_t auto_resize_guest;",
        "set_console_output_callback",
        "send_console_input",
    ):
        require(contract in runtime_header, f"runtime API lacks {contract}")
    require(
        "boot->auto_resize_guest > 1U" in runtime_source
        and ".autoResizeGuest = boot->auto_resize_guest != 0" in runtime_source,
        "runtime does not validate and forward the display resize policy",
    )
    for contract in (
        "publishConsoleOutput",
        "runtimeSetConsoleOutputCallback",
        "runtimeSendConsoleInput",
    ):
        require(contract in runtime_source, f"runtime console bridge lacks {contract}")
    for contract in (
        "__attribute__((constructor))",
        "pthread_sigmask(SIG_BLOCK",
        "dlopen(executable.fileSystemRepresentation, RTLD_NOW | RTLD_GLOBAL)",
        'dlsym(handle, "kernarium_runtime_get_api_v8")',
        "selectedPageSize == guestPageSize",
        "releaseFirst >= '0' && releaseFirst <= '9'",
        "forwardConsoleOutput",
        "kernarium_runtime_loader_send_console_input",
    ):
        require(contract in loader_source, f"application runtime loader lacks {contract}")
    for forbidden in (
        "api->kernel_release == nullptr",
        "api->framework_name == nullptr",
        "runtime identity mismatch",
    ):
        require(
            forbidden not in loader_source,
            f"application loader retains release admission {forbidden}",
        )
    require(
        "KernariumConsoleOutputHandler" in loader_header,
        "application runtime loader does not expose serial output",
    )

    linker_patch = read(arguments.linker_patch)
    require(
        "diff --git a/arch/um/kernel/dyn.lds.S" in linker_patch
        and linker_patch.count("+  . = ALIGN(0x4000);") >= 2
        and "following dynamic" in linker_patch,
        "the architecture topic does not align the early signed-text boundaries",
    )
    verify_carrier_tool(arguments.carrier_verifier)

    runtime_4k = "KRuntime_6_18_4K"
    runtime_16k = "KRuntime_6_18_16K"
    runtime_android = "KRuntime_ACK_6_18_16K"
    package_script = read(arguments.package_script)
    for contract in (
        "kernarium-runtime-ios-device-4k",
        "kernarium-runtime-ios-device-16k",
        'create_xcframework "$RUNTIME_4K"',
        'create_xcframework "$RUNTIME_16K"',
        'create_xcframework "$RUNTIME_ANDROID_16K"',
        'GPU_FRAMEWORKS="MesaEGL MesaGallium KosmicKrisp"',
        "LC_RPATH",
        "@loader_path/..",
        'build-ios-zstd.sh',
        'GPTFDisk.xcframework',
        'LibZstd.xcframework',
    ):
        require(contract in package_script, f"runtime packaging lacks {contract}")
    # KosmicKrisp is a Metal 4 driver and the iOS Simulator exposes no Metal 4
    # device, so there is no Simulator slice of this stack to package. The
    # storage and compression cores carry no Metal dependency and keep theirs.
    for forbidden in (
        "kernarium-runtime-ios-simulator",
        "ios-simulator-arm64",
        "--platform ios-simulator",
    ):
        require(
            forbidden not in package_script,
            f"runtime packaging still expects an iOS Simulator slice: {forbidden}",
        )

    xcode_project = read(arguments.xcode_project)
    for runtime in (runtime_4k, runtime_16k, runtime_android):
        require(
            f"{runtime}.xcframework in Embed Frameworks" in xcode_project,
            f"Xcode does not embed {runtime}",
        )
    require(
        "KernariumRuntime" not in xcode_project
        and "KernariumGPTFDisk" not in xcode_project,
        "Xcode retains an obsolete framework name",
    )
    # Mesa Zink is the host GL provider and KosmicKrisp the Vulkan driver it
    # renders on; both ride in the application next to the runtime frameworks.
    for provider in ("MesaEGL", "MesaGallium", "KosmicKrisp"):
        require(
            f"{provider}.xcframework in Embed Frameworks" in xcode_project,
            f"Xcode does not embed {provider}",
        )
    require(
        "EGL.xcframework" not in xcode_project.replace("MesaEGL.xcframework", "")
        and "GLESv2" not in xcode_project,
        "Xcode retains the removed ANGLE GL provider",
    )
    require(
        'SUPPORTED_PLATFORMS = iphoneos;' in xcode_project
        and "iphonesimulator" not in xcode_project,
        "Xcode still builds the application for the iOS Simulator",
    )
    for dependency in ("GPTFDisk", "LibZstd"):
        require(
            f"{dependency}.xcframework in Frameworks" in xcode_project,
            f"Xcode does not link {dependency}",
        )
        require(
            f"{dependency}.xcframework in Embed Frameworks" not in xcode_project,
            f"Xcode embeds static dependency {dependency}",
        )
    for contract in (
        'repositoryURL = "https://github.com/migueldeicaza/SwiftTerm";',
        f"version = {SWIFTTERM_VERSION};",
        "SwiftTerm in Frameworks",
    ):
        require(contract in xcode_project, f"Xcode SwiftTerm dependency lacks {contract}")
    package_resolved = json.loads(read(arguments.package_resolved))
    swiftterm_pins = [
        pin for pin in package_resolved["pins"] if pin["identity"] == "swiftterm"
    ]
    require(len(swiftterm_pins) == 1, "Package.resolved lacks one SwiftTerm pin")
    swiftterm_state = swiftterm_pins[0]["state"]
    require(
        swiftterm_state.get("version") == SWIFTTERM_VERSION
        and swiftterm_state.get("revision") == SWIFTTERM_REVISION,
        "Package.resolved does not authenticate the selected SwiftTerm release",
    )

    content_view = read(arguments.content_view)
    controls = content_view.split(
        "private var floatingButtonsContent", 1
    )[1].split("private var settingsButton", 1)[0]
    require(
        controls.index("settingsButton")
        < controls.index("displayModeButton")
        < controls.index("keyboardButton"),
        "display-mode control is not between settings and keyboard",
    )
    require(
        "static let defaultMode: Self = .framebuffer" in content_view,
        "Kernarium does not default to the framebuffer surface",
    )
    require(
        "let scale = retinaMode ? screen.nativeScale : 1" in content_view
        and "(CGFloat(160) * scale).rounded()" in content_view
        and "request.dots_per_inch = dotsPerInch" in content_view,
        "Kernarium does not scale and forward the Android display density",
    )
    require(
        "request.auto_resize_guest = autoResizeGuest ? 1 : 0" in content_view,
        "Kernarium does not forward the persisted display resize policy",
    )
    require(
        '"androidboot.lcd_density="' in runtime_source
        and "std::to_string(dotsPerInch)" in runtime_source
        and "boot->android_command_line_path, dotsPerInch" in runtime_source,
        "Kernarium does not publish its per-launch DPI to Android init",
    )
    serial_console = read(arguments.serial_console_view)
    for contract in (
        "import SwiftTerm",
        "TerminalViewDelegate",
        "TerminalView(frame: .zero)",
        "view.feed(byteArray:",
        "kernarium_runtime_loader_send_console_input",
    ):
        require(contract in serial_console, f"SwiftTerm console lacks {contract}")
    frameworks_phase = xcode_project.split(
        "/* Begin PBXFrameworksBuildPhase section */", 1
    )[1].split("/* End PBXFrameworksBuildPhase section */", 1)[0]
    require(
        "KRuntime_" not in frameworks_phase,
        "a selectable runtime is linked for process launch instead of only embedded",
    )

    mac_launch_script = read(arguments.mac_launch_script)
    for contract in (
        "UML_MAINLINE_618_KERNEL_ARTIFACT_16K_DIR",
        "UML_MAINLINE_618_KERNEL_ARTIFACT_4K_DIR",
        # The device runtimes take the Vulkan-experiment kernel line with the
        # Vulkan-experiment GPU stack.
        "vulkan-experiment-16k.env",
        "vulkan-experiment-4k.env",
        "cmake --build --preset kernarium-ios-device",
        "cmake --build --preset kernarium-ios-device-4k",
        "platform=macOS,variant=Designed for iPad",
        "com.apple.containermanagerd.metadata.plist",
        "TEST_RUNNER_KERNARIUM_ROOTFS=",
    ):
        require(
            contract in mac_launch_script,
            f"iOS-on-Mac launcher lacks {contract}",
        )
    require(
        '-DUML_ANDROID17_ROOTFS_RELEASE_DIR=' not in mac_launch_script,
        "iOS build still passes a rootfs release into CMake",
    )
    require(
        "simctl" not in mac_launch_script,
        "iOS-on-Mac launcher still drives the Simulator",
    )

    macos_app_cmake = read(arguments.macos_app_cmake)
    for contract in (
        "UML_ACTIVE_KERNEL_ARTIFACT_TARGET",
        "uml_add_macos_embedded_kernel_app",
        "get_target_property(_uml_kernel_image",
        "LINKER:-sectcreate,__DATA,__uml_elf,${_uml_kernel_image}",
        "uml-darwin linux_main.cpp",
        "uml-darwin-android17 android17_main.cpp",
    ):
        require(contract in macos_app_cmake, f"macOS app binding lacks {contract}")
    for forbidden in (
        "TARGET uml_kernel",
        "UML_KERNEL_BUILD_DIR",
        "UML_ANDROID17_ROOTFS_RELEASE_TARGET",
        "LINKER:-sectcreate,__DATA,__uml_rootfs",
    ):
        require(
            forbidden not in macos_app_cmake,
            f"macOS app binding retains producer-global input {forbidden}",
        )

    macos_app = read(arguments.macos_app)
    for prefix, member in (("mem=", "memory"), ("ncpus=", "cpus")):
        require(
            f'prepared.kernel, "{prefix}", configuration.default_guest_{member}'
            in macos_app,
            f"shared macOS runner does not apply the selected {prefix} default",
        )

    macos_linux_main = read(arguments.macos_linux_main)
    for declaration in (
        'macos_default_guest_memory{"mem=8G"}',
        'macos_default_guest_cpus{"ncpus=10"}',
    ):
        require(declaration in macos_linux_main, f"Linux launcher lacks {declaration}")
    for member, value in (
        ("default_guest_memory", "macos_default_guest_memory"),
        ("default_guest_cpus", "macos_default_guest_cpus"),
    ):
        require(
            f".{member} = {value}" in macos_linux_main,
            f"Linux launcher does not select {value}",
        )
    require(
        ".touchscreen_input = true" not in macos_linux_main,
        "Linux launcher must retain desktop mouse input",
    )

    android_cmake = read(arguments.android_cmake)
    for contract in (
        "if(NOT UML_GUEST_16K_PAGES)",
        "UML_ANDROID17_KERNEL_ARTIFACT_DIR",
        "uml_import_kernel_artifact",
        "EXPECTED_SLICE arm64",
        "The Android 17 kernel artifact must use the arm64 slice",
        "android-6.18-16k-20260829-no-ipconfig-8f74db26",
        "EXPECTED_RELEASE_SERIES 6.18",
    ):
        require(contract in android_cmake, f"Android product lacks {contract}")
    for forbidden in (
        "UML_ANDROID17_ROOTFS_RELEASE_DIR",
        "uml_import_android17_rootfs_release",
        "UML_ANDROID_ROOTFS_RELEASE_MANIFEST",
    ):
        require(
            forbidden not in android_cmake,
            f"Android product still imports rootfs identity {forbidden}",
        )

    android_main = read(arguments.android_main)
    for contract in (
        'android_default_guest_memory{"mem=4G"}',
        'android_default_guest_cpus{"ncpus=10"}',
        ".input_mode = uml::app::DisplayInput::touchscreen",
        'release_option{"--android-release="}',
        'disk_option{"--android-disk="}',
        'boot_mode_option{"--android-boot-mode="}',
        '"ubd0=" + disk.string()',
        '"initrd=" + disk.string()',
        '"androidboot.kernarium.adb_auth="',
        'std::string{adb_authorization_prefix} + "0"',
        '"initrd_offset=" + std::to_string(boot.ramdisk.offset)',
        '"initrd_size=" + std::to_string(boot.ramdisk.size)',
        '"root=/dev/ram0"',
    ):
        require(contract in android_main, f"Android launcher lacks {contract}")
    for forbidden in (
        "section$start$__DATA$__uml_rootfs",
        "UML_ANDROID17_ROOTFS_RELEASE_ID",
        "UML_ANDROID17_ROOTFS_DIRECTORY_NAME",
        "UML_ANDROID17_BASE_DISK_SIZE",
        "release-owned kernel argument cannot be overridden",
        "Android instance belongs to another rootfs release",
        "--android-disk-size-gib",
        "--android-instance",
        "--android-base-disk",
        "UML_ANDROID17_INSTANCE",
        "UML_ANDROID17_BASE_DISK",
        'instance.string() + ","',
        "UML_ANDROID17_DISK_SIZE_GIB",
        "prepareDisk(",
    ):
        require(
            forbidden not in android_main,
            f"Android launcher retains runtime identity check {forbidden}",
        )

    for path, source in (
        (arguments.presets, presets_source),
        (arguments.runtime_cmake, runtime_cmake),
        (arguments.mac_launch_script, mac_launch_script),
        (arguments.macos_app_cmake, macos_app_cmake),
        (arguments.macos_app, macos_app),
        (arguments.macos_linux_main, macos_linux_main),
        (arguments.android_cmake, android_cmake),
        (arguments.android_main, android_main),
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
