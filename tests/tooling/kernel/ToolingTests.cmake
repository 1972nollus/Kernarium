# SPDX-License-Identifier: GPL-2.0-or-later

uml_add_tooling_test(
    NAME kernel-versioned-layout
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_versioned_layout.sh
    LABELS kernel;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME kernel-slice-producer-contract
    COMMAND
        ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_kernel_slice_contract.sh
    LABELS kernel;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME kernel-artifact-contract
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_kernel_artifact_contract.py
        --artifact-module
            ${PROJECT_SOURCE_DIR}/kernel/common/artifacts/kernel_artifact.py
        --schema
            ${PROJECT_SOURCE_DIR}/kernel/common/artifacts/kernel-artifact-manifest.schema.json
    LABELS kernel;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME kernel-git-source-preparer
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_materialize_git.sh
    LABELS kernel;dependency
    TIMEOUT 30)

uml_add_tooling_test(
    NAME kernel-source-location-config
    COMMAND
        ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_source_location_config.sh
    LABELS kernel;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME kernel-series-topology
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_series_topology.sh
    LABELS kernel;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME kernel-compiler-selection
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_kernel_toolchain.sh
        ${PROJECT_SOURCE_DIR}/kernel/mainline-6.18/build/KernelToolchain.cmake
    LABELS kernel;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME kernel-darwin-host-compat
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_darwin_host_compat.sh
    LABELS kernel;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME kernel-next-version-policy-actions
    COMMAND ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_next_version_policy.sh
    LABELS kernel;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME kernel-compressed-memory-policy
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_compressed_memory_policy.py
        --kernel-config
            ${PROJECT_SOURCE_DIR}/kernel/mainline-6.18/config/common.fragment
    LABELS kernel;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME kernel-virtio-gpu-shutdown-policy
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_virtio_gpu_shutdown_policy.py
        --series-manifest
            ${PROJECT_SOURCE_DIR}/kernel/mainline-6.18/patches/series/manifest.tsv
        --series-directory
            ${PROJECT_SOURCE_DIR}/kernel/mainline-6.18/patches/series
    LABELS kernel;gpu;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME kernel-ubd-discard-policy
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_ubd_discard_policy.py
        --mainline-patch
            ${PROJECT_SOURCE_DIR}/kernel/mainline-6.18/patches/series/0024-UML-DARWIN-LOCAL-um-batch-UBD-discard-ranges.patch
        --android-patch
            ${PROJECT_SOURCE_DIR}/kernel/android-6.18/patches/series/0037-UML-DARWIN-LOCAL-um-batch-UBD-discard-ranges.patch
    LABELS kernel;android;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME kernel-virtio-gpu-shared-memory-policy
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_virtio_gpu_shared_memory_policy.py
        --series-manifest
            ${PROJECT_SOURCE_DIR}/kernel/mainline-6.18/patches/series/manifest.tsv
        --series-directory
            ${PROJECT_SOURCE_DIR}/kernel/mainline-6.18/patches/series
    LABELS kernel;gpu;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME apple-shared-kernel-image-policy
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_apple_kernel_image_policy.py
        --presets ${PROJECT_SOURCE_DIR}/CMakePresets.json
        --root-cmake ${PROJECT_SOURCE_DIR}/CMakeLists.txt
        --kernel-cmake ${PROJECT_SOURCE_DIR}/kernel/CMakeLists.txt
        --artifact-cmake
            ${PROJECT_SOURCE_DIR}/kernel/common/artifacts/KernelArtifact.cmake
        --acceptance-16k
            ${PROJECT_SOURCE_DIR}/kernel/mainline-6.18/artifacts/accepted-16k.env
        --acceptance-4k
            ${PROJECT_SOURCE_DIR}/kernel/mainline-6.18/artifacts/accepted-4k.env
        --acceptance-x86-4k
            ${PROJECT_SOURCE_DIR}/kernel/mainline-6.18/slices/x86_64/artifacts/accepted-4k.env
        --experiment-16k
            ${PROJECT_SOURCE_DIR}/kernel/mainline-6.18/artifacts/vulkan-experiment-16k.env
        --experiment-4k
            ${PROJECT_SOURCE_DIR}/kernel/mainline-6.18/artifacts/vulkan-experiment-4k.env
        --series-metadata
            ${PROJECT_SOURCE_DIR}/kernel/mainline-6.18/patches/metadata.env
        --android-kernel-acceptance
            ${PROJECT_SOURCE_DIR}/kernel/android-6.18/artifacts/accepted-16k.env
        --android-kernel-publisher
            ${PROJECT_SOURCE_DIR}/android/aosp-17/tools/publish-kernel-artifact.py
        --runtime-cmake ${PROJECT_SOURCE_DIR}/Kernarium/Runtime/CMakeLists.txt
        --static-guest-source
            ${PROJECT_SOURCE_DIR}/darwin/runtime/static_guest.cpp
        --runtime-source
            ${PROJECT_SOURCE_DIR}/Kernarium/Runtime/KernariumRuntime.mm
        --runtime-header
            ${PROJECT_SOURCE_DIR}/Kernarium/Runtime/KernariumRuntime.h
        --runtime-loader
            ${PROJECT_SOURCE_DIR}/Kernarium/Kernarium/KernariumRuntimeLoader.mm
        --runtime-loader-header
            ${PROJECT_SOURCE_DIR}/Kernarium/Kernarium/KernariumRuntimeLoader.h
        --content-view
            ${PROJECT_SOURCE_DIR}/Kernarium/Kernarium/ContentView.swift
        --serial-console-view
            ${PROJECT_SOURCE_DIR}/Kernarium/Kernarium/SerialConsoleView.swift
        --carrier-verifier
            ${PROJECT_SOURCE_DIR}/Kernarium/scripts/verify-signed-kernel-carrier.py
        --linker-patch
            ${PROJECT_SOURCE_DIR}/kernel/mainline-6.18/patches/series/0001-uml-establish-shared-architecture-contracts-and-buil.patch
        --package-script
            ${PROJECT_SOURCE_DIR}/Kernarium/scripts/package-ios-frameworks.sh
        --xcode-project
            ${PROJECT_SOURCE_DIR}/Kernarium/Kernarium.xcodeproj/project.pbxproj
        --package-resolved
            ${PROJECT_SOURCE_DIR}/Kernarium/Kernarium.xcodeproj/project.xcworkspace/xcshareddata/swiftpm/Package.resolved
        --mac-launch-script
            ${PROJECT_SOURCE_DIR}/Kernarium/scripts/run-ios-mac.sh
        --macos-app-cmake
            ${PROJECT_SOURCE_DIR}/darwin/app/macos/CMakeLists.txt
        --macos-app ${PROJECT_SOURCE_DIR}/darwin/app/macos/main.cpp
        --macos-linux-main
            ${PROJECT_SOURCE_DIR}/darwin/app/macos/linux_main.cpp
        --android-cmake ${PROJECT_SOURCE_DIR}/android/aosp-17/CMakeLists.txt
        --android-main
            ${PROJECT_SOURCE_DIR}/darwin/app/macos/android17_main.cpp
        --toolchain-env
            ${PROJECT_SOURCE_DIR}/kernel/mainline-6.18/build/toolchain/darwin.env
    LABELS kernel;audit
    TIMEOUT 30)

uml_add_tooling_test(
    NAME kernel-darwin-irq-edge
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/tooling/kernel/test_darwin_irq_edge.py
    LABELS kernel
    TIMEOUT 120)
