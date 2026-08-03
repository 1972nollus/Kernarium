#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_root=${UML_XCFRAMEWORK_BUILD_DIR:-"$project_root/.build/xcframework"}
output=${UML_XCFRAMEWORK_OUTPUT:-"$build_root/UMLEmulator.xcframework"}
toolchain="$project_root/cmake/toolchains/Apple.cmake"

build_slice() {
    platform=$1
    build_dir="$build_root/$platform"
    cmake -S "$project_root" -B "$build_dir" \
        -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
        -DUML_APPLE_PLATFORM="$platform" \
        -DUML_BUILD_CLI=OFF \
        -DUML_BUILD_KERNEL_RUNNER=OFF \
        -DUML_BUILD_UNIT_TESTS=OFF \
        -DUML_BUILD_INTEGRATION_TESTS=OFF \
        -DUML_BUILD_TOOLING_TESTS=OFF \
        -DUML_BUILD_GPU=OFF \
        -DCMAKE_BUILD_TYPE=Release
    cmake --build "$build_dir" \
        --target uml_emulator architecture-audit --parallel

    library="$build_dir/darwin/emulator/libuml_emulator.a"
    test -f "$library"
    xcrun lipo -info "$library"
    xcrun nm -gU "$library" | grep -q ' _uml_emulator_create$'
    if [ "$platform" != macos ]; then
        non_jit_publishers=$(xcrun nm -o "$library" | awk '
            / U _(mprotect|vm_protect|mach_vm_protect|pthread_jit_write_protect_np|sys_icache_invalidate)$/ &&
            $0 !~ /:jit_arena\.cpp\.o:/ { print }
        ')
        if [ -n "$non_jit_publishers" ]; then
            echo "non-JIT executable-memory primitive in $platform archive:" >&2
            echo "$non_jit_publishers" >&2
            exit 1
        fi
    fi

    consumer_dir="$build_dir/external-consumer"
    cmake -S "$project_root/tests/consumer/apple" -B "$consumer_dir" \
        -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
        -DUML_APPLE_PLATFORM="$platform" \
        -DUML_EMULATOR_LIBRARY="$library" \
        -DUML_EMULATOR_HEADERS="$project_root/darwin/api/include" \
        -DCMAKE_BUILD_TYPE=Release
    cmake --build "$consumer_dir" \
        --target uml-emulator-external-consumer --parallel
}

build_slice macos
build_slice ios
build_slice ios-simulator

rm -rf "$output"
xcodebuild -create-xcframework \
    -library "$build_root/macos/darwin/emulator/libuml_emulator.a" \
    -headers "$project_root/darwin/api/include" \
    -library "$build_root/ios/darwin/emulator/libuml_emulator.a" \
    -headers "$project_root/darwin/api/include" \
    -library "$build_root/ios-simulator/darwin/emulator/libuml_emulator.a" \
    -headers "$project_root/darwin/api/include" \
    -output "$output"

echo "$output"
