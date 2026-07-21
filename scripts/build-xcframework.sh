#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_root=${UML_XCFRAMEWORK_BUILD_DIR:-"$project_root/.build/xcframework"}
output=${UML_XCFRAMEWORK_OUTPUT:-"$build_root/UMLBackend.xcframework"}
toolchain="$project_root/cmake/toolchains/Apple.cmake"

build_slice() {
    platform=$1
    build_dir="$build_root/$platform"
    cmake -S "$project_root" -B "$build_dir" \
        -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
        -DUML_APPLE_PLATFORM="$platform" \
        -DUML_BUILD_CLI=OFF \
        -DUML_BUILD_KERNEL_RUNNER=OFF \
        -DUML_BUILD_TESTS=OFF \
        -DCMAKE_BUILD_TYPE=Release
    cmake --build "$build_dir" --target uml_backend --parallel
}

build_slice macos
build_slice ios
build_slice ios-simulator

rm -rf "$output"
xcodebuild -create-xcframework \
    -library "$build_root/macos/darwin/backend/libuml_backend.a" \
    -headers "$project_root/darwin/backend/include" \
    -library "$build_root/ios/darwin/backend/libuml_backend.a" \
    -headers "$project_root/darwin/backend/include" \
    -library "$build_root/ios-simulator/darwin/backend/libuml_backend.a" \
    -headers "$project_root/darwin/backend/include" \
    -output "$output"

echo "$output"
