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
    parser.add_argument("--artifact-cmake", type=Path, required=True)
    parser.add_argument("--acceptance-16k", type=Path, required=True)
    parser.add_argument("--acceptance-4k", type=Path, required=True)
    parser.add_argument("--android-kernel-acceptance", type=Path, required=True)
    parser.add_argument("--android-kernel-publisher", type=Path, required=True)
    parser.add_argument("--runtime-cmake", type=Path, required=True)
    parser.add_argument("--runtime-source", type=Path, required=True)
    parser.add_argument("--runtime-header", type=Path, required=True)
    parser.add_argument("--runtime-loader", type=Path, required=True)
    parser.add_argument("--runtime-loader-header", type=Path, required=True)
    parser.add_argument("--content-view", type=Path, required=True)
    parser.add_argument("--serial-console-view", type=Path, required=True)
    parser.add_argument("--carrier-verifier", type=Path, required=True)
    parser.add_argument("--linker-patch", type=Path, required=True)
    parser.add_argument("--metadata", type=Path, required=True)
    parser.add_argument("--package-script", type=Path, required=True)
    parser.add_argument("--xcode-project", type=Path, required=True)
    parser.add_argument("--package-resolved", type=Path, required=True)
    parser.add_argument("--simulator-script", type=Path, required=True)
    parser.add_argument("--macos-app-cmake", type=Path, required=True)
    parser.add_argument("--macos-app", type=Path, required=True)
    parser.add_argument("--macos-linux-main", type=Path, required=True)
    parser.add_argument("--android-cmake", type=Path, required=True)
    parser.add_argument("--android-release-cmake", type=Path, required=True)
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
    android_kernel_acceptance = read(arguments.android_kernel_acceptance)
    directory_16k = env_value(
        acceptance_16k, "UML_KERNEL_ARTIFACT_DIRECTORY_NAME"
    )
    directory_4k = env_value(
        acceptance_4k, "UML_KERNEL_ARTIFACT_DIRECTORY_NAME"
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
    for platform in ("device", "simulator"):
        profile_16k = configurations[f"kernarium-ios-{platform}"]
        profile_4k = configurations[f"kernarium-ios-{platform}-4k"]
        require(
            profile_16k["cacheVariables"]["UML_KERNEL_PRODUCT"]
            == "mainline-6.18"
            and profile_16k["cacheVariables"]["UML_GUEST_16K_PAGES"] is True,
            f"{platform} 16 KiB runtime profile does not select mainline",
        )
        require(
            profile_4k["cacheVariables"]["UML_GUEST_16K_PAGES"] is False
            and "UML_KERNARIUM_KERNEL_IMAGE"
            not in profile_4k["cacheVariables"],
            f"{platform} 4 KiB runtime profile bypasses artifact selection",
        )
        require(
            profile_16k["binaryDir"] != profile_4k["binaryDir"],
            f"{platform} page profiles share one CMake build tree",
        )

    kernel_cmake = read(arguments.kernel_cmake)
    artifact_cmake = read(arguments.artifact_cmake)
    for contract in (
        "uml_import_kernel_artifact",
        "UML_MAINLINE_618_KERNEL_ARTIFACT_DIR",
        "UML_ACTIVE_KERNEL_ARTIFACT_TARGET",
        "accepted-16k.env",
        "accepted-4k.env",
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
        "add_library(\"${ARTIFACT_TARGET}\" INTERFACE)",
        "UML_KERNEL_IMAGE",
        "UML_KERNEL_MODULES_BUILTIN",
        "CMAKE_CONFIGURE_DEPENDS",
    ):
        require(contract in artifact_cmake, f"artifact CMake lacks {contract}")
    require(
        env_value(acceptance_16k, "UML_KERNEL_ARTIFACT_PAGE_SIZE") == "16384"
        and directory_16k.startswith("mainline-6.18-16k-"),
        "16 KiB artifact acceptance is malformed",
    )
    require(
        env_value(acceptance_4k, "UML_KERNEL_ARTIFACT_PAGE_SIZE") == "4096"
        and directory_4k.startswith("mainline-6.18-4k-"),
        "4 KiB artifact acceptance is malformed",
    )
    require(
        env_value(android_kernel_acceptance, "UML_KERNEL_ARTIFACT_FAMILY")
        == "android-6.18"
        and env_value(
            android_kernel_acceptance, "UML_KERNEL_ARTIFACT_PAGE_SIZE"
        ) == "16384"
        and env_value(
            android_kernel_acceptance, "UML_KERNEL_ARTIFACT_DIRECTORY_NAME"
        ).startswith("android-6.18-16k-"),
        "Android ACK artifact acceptance is malformed",
    )
    android_kernel_publisher = read(arguments.android_kernel_publisher)
    for contract in (
        'family = "android-6.18"',
        "verify_artifact(",
        'page_size = 16384',
        'if name.endswith(".ko")',
        "dedicated x86_64 Linux builder",
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
        "UML_KERNEL_RELEASE",
        "KernariumRuntime_${KERNARIUM_KERNEL_RELEASE_COMPONENT}_${KERNARIUM_KERNEL_PAGE_VARIANT}",
        'OUTPUT_NAME "${KERNARIUM_RUNTIME_FRAMEWORK_NAME}"',
        "verify-signed-kernel-carrier.py",
        "LINKER:-sectcreate,__TEXT,__uml_elf,${_kernarium_kernel_image}",
        'LINK_DEPENDS "${_kernarium_kernel_image}"',
        "UML_ANDROID17_ROOTFS_RELEASE_TARGET",
        "UML_ANDROID_ROOTFS_RELEASE_MANIFEST",
        "LINKER:-sectcreate,__DATA,__uml_rootfs",
    ):
        require(contract in runtime_cmake, f"runtime carrier lacks {contract}")
    for forbidden in (
        "UML_KERNARIUM_KERNEL_IMAGE",
        "/Volumes/Kernel/build-16k",
        "/Volumes/Kernel/build-4k",
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
        "kernarium_runtime_get_api_v4" in runtime_source
        and "KERNARIUM_RUNTIME_FRAMEWORK_NAME" in runtime_source,
        "runtime does not publish its versioned identity API",
    )
    require(
        "__attribute__((constructor))" not in runtime_source,
        "deferred runtime still owns the pre-main timer constructor",
    )
    for contract in (
        "set_console_output_callback",
        "send_console_input",
    ):
        require(contract in runtime_header, f"runtime API lacks {contract}")
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
        'dlsym(handle, "kernarium_runtime_get_api_v4")',
        "selectedPageSize == guestPageSize",
        "forwardConsoleOutput",
        "kernarium_runtime_loader_send_console_input",
    ):
        require(contract in loader_source, f"application runtime loader lacks {contract}")
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
        "KernariumRuntime_" not in frameworks_phase,
        "a selectable runtime is linked for process launch instead of only embedded",
    )

    simulator_script = read(arguments.simulator_script)
    for contract in (
        "UML_MAINLINE_618_KERNEL_ARTIFACT_16K_DIR",
        "UML_MAINLINE_618_KERNEL_ARTIFACT_4K_DIR",
        "accepted-16k.env",
        "accepted-4k.env",
        "cmake --build --preset kernarium-ios-simulator",
        "cmake --build --preset kernarium-ios-simulator-4k",
    ):
        require(contract in simulator_script, f"Simulator launcher lacks {contract}")

    macos_app_cmake = read(arguments.macos_app_cmake)
    for contract in (
        "UML_ACTIVE_KERNEL_ARTIFACT_TARGET",
        "uml_add_macos_embedded_kernel_app",
        "get_target_property(_uml_kernel_image",
        "LINKER:-sectcreate,__DATA,__uml_elf,${_uml_kernel_image}",
        "uml-darwin linux_main.cpp",
        "uml-darwin-android17 android17_main.cpp",
        "LINKER:-sectcreate,__DATA,__uml_rootfs,${_uml_android_manifest}",
    ):
        require(contract in macos_app_cmake, f"macOS app binding lacks {contract}")
    for forbidden in ("TARGET uml_kernel", "UML_KERNEL_BUILD_DIR"):
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

    android_cmake = read(arguments.android_cmake)
    android_release_cmake = read(arguments.android_release_cmake)
    for contract in (
        "if(NOT UML_GUEST_16K_PAGES)",
        "UML_ANDROID17_ROOTFS_RELEASE_DIR",
        "UML_ANDROID17_KERNEL_ARTIFACT_DIR",
        "uml_import_android17_rootfs_release",
        "uml_import_kernel_artifact",
    ):
        require(contract in android_cmake, f"Android product lacks {contract}")
    for contract in (
        "verify-release.py",
        "--installed",
        'STREQUAL "android-17-ack-6.18-16k"',
        'STREQUAL "android-6.18"',
        "UML_ANDROID_ROOTFS_RELEASE_MANIFEST",
        "UML_ANDROID_BASE_DISK",
    ):
        require(
            contract in android_release_cmake,
            f"Android release importer lacks {contract}",
        )

    android_main = read(arguments.android_main)
    for contract in (
        'android_default_guest_memory{"mem=4G"}',
        'android_default_guest_cpus{"ncpus=10"}',
        'release_option{"--android-release="}',
        'instance_option{"--android-instance="}',
        'boot_mode_option{"--android-boot-mode="}',
        "section$start$__DATA$__uml_rootfs",
        '"ubd0=" + instance.string()',
        '"initrd=" + base_disk.string()',
        '"initrd_offset=" + std::to_string(boot.ramdisk.offset)',
        '"initrd_size=" + std::to_string(boot.ramdisk.size)',
        '"root=/dev/ram0"',
        "release-owned kernel argument cannot be overridden",
        "Android instance belongs to another rootfs release",
    ):
        require(contract in android_main, f"Android launcher lacks {contract}")

    for path, source in (
        (arguments.presets, presets_source),
        (arguments.runtime_cmake, runtime_cmake),
        (arguments.simulator_script, simulator_script),
        (arguments.macos_app_cmake, macos_app_cmake),
        (arguments.macos_app, macos_app),
        (arguments.macos_linux_main, macos_linux_main),
        (arguments.android_cmake, android_cmake),
        (arguments.android_release_cmake, android_release_cmake),
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
