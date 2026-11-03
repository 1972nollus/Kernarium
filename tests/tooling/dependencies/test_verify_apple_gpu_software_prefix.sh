#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The iOS Simulator's GPU prefix renders on the CPU: Mesa's software Gallium
# screen, no KosmicKrisp, no Vulkan headers and no Venus. Check that the
# verifier accepts that prefix and rejects each way it could carry the device
# stack, or lose its rasterizer.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
VERIFIER="$ROOT_DIR/dependencies/cmake/VerifyAppleGpuPrefix.cmake"
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/uml-apple-gpu-software-prefix.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM
BASE_PREFIX="$TEST_ROOT/prefix"
CONTRACT=ios-simulator-gpu-software-arm64.tsv

TEST_NAME=test-verify-apple-gpu-software-prefix.sh
. "$(dirname -- "$0")/../support/expect.sh"

verify()
{
	cmake -DPREFIX="$1" -DSOURCE_ROOT="$ROOT_DIR" \
		-DPLATFORM=ios-simulator -P "$VERIFIER"
}

copy_fixture()
{
	cp -R "$BASE_PREFIX" "$1"
}

simulator_clang()
{
	xcrun --sdk iphonesimulator clang -arch arm64 \
		-target arm64-apple-ios27.0-simulator "$@"
}

# make_framework PREFIX FRAMEWORK SOURCE [CLANG-ARGUMENT...]
make_framework()
{
	prefix=$1
	framework=$2
	source=$3
	shift 3
	binary="$prefix/Frameworks/$framework.framework/$framework"
	mkdir -p "$(dirname -- "$binary")"
	simulator_clang -dynamiclib "$source" "$@" \
		-Wl,-install_name,"@rpath/$framework.framework/$framework" \
		-o "$binary"
	cat >"$(dirname -- "$binary")/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>$framework</string>
<key>CFBundleIdentifier</key><string>org.linux-um-darwin.fixture.$framework</string>
<key>CFBundlePackageType</key><string>FMWK</string>
<key>MinimumOSVersion</key><string>27.0</string>
</dict></plist>
EOF
}

command -v xcrun >/dev/null 2>&1 || fail "xcrun is required"
mkdir -p "$BASE_PREFIX/Frameworks" "$BASE_PREFIX/lib" \
	"$BASE_PREFIX/share/uml-dependencies" \
	"$BASE_PREFIX/include/EGL" "$BASE_PREFIX/include/GL" \
	"$BASE_PREFIX/include/KHR" "$BASE_PREFIX/include/epoxy" \
	"$BASE_PREFIX/include/virgl"

cat >"$TEST_ROOT/fixture.c" <<'EOF'
int uml_apple_gpu_fixture(void) { return 0; }
EOF
cat >"$TEST_ROOT/gallium.c" <<'EOF'
void *softpipe_create_screen(void) { return 0; }
void *uml_mesa_gallium_fixture(void) { return softpipe_create_screen(); }
EOF
cat >"$TEST_ROOT/egl.c" <<'EOF'
extern void *uml_mesa_gallium_fixture(void);
void *eglGetPlatformDisplay(unsigned platform, void *display, const long *attributes)
{
    (void)platform;
    (void)display;
    (void)attributes;
    return uml_mesa_gallium_fixture();
}
EOF
simulator_clang -c "$TEST_ROOT/fixture.c" -o "$TEST_ROOT/fixture.o"
xcrun ar rcs "$BASE_PREFIX/lib/libepoxy.a" "$TEST_ROOT/fixture.o"
xcrun ar rcs "$BASE_PREFIX/lib/libvirglrenderer.a" "$TEST_ROOT/fixture.o"
make_framework "$BASE_PREFIX" MesaGallium "$TEST_ROOT/gallium.c"
make_framework "$BASE_PREFIX" MesaEGL "$TEST_ROOT/egl.c" \
	"$BASE_PREFIX/Frameworks/MesaGallium.framework/MesaGallium"
for header in EGL/egl.h epoxy/egl.h virgl/virglrenderer.h virgl/virgl_hw.h; do
	printf '%s\n' fixture >"$BASE_PREFIX/include/$header"
done

cp "$ROOT_DIR/dependencies/manifest.tsv" \
	"$BASE_PREFIX/share/uml-dependencies/manifest.tsv"
cp "$ROOT_DIR/dependencies/platforms/$CONTRACT" \
	"$BASE_PREFIX/share/uml-dependencies/$CONTRACT"
series_tree()
{
	awk -F '\t' '$1 == "tree" { print $2 }' "$1/series.tsv"
}
{
	printf 'source\ttree\n'
	for component in libepoxy virglrenderer mesa; do
		printf '%s\t%s\n' "$component" \
			"$(series_tree "$ROOT_DIR/dependencies/patches/$component")"
	done
	printf 'venus-protocol\t%s\n' \
		"$(series_tree "$ROOT_DIR/tools/rootfs/vulkan/patches/venus-protocol")"
} >"$BASE_PREFIX/share/uml-dependencies/source-trees.tsv"
# The software variant builds no protocol, so its receipt has no protocol
# identity.
cat >"$BASE_PREFIX/share/uml-dependencies/build-receipt.tsv" <<EOF
field	value
format	2
product	apple-gpu-vulkan-experiment
platform	ios-simulator
sdk_name	iphonesimulator
sdk_version	fixture
sdk_path	/fixture/iPhoneSimulator.sdk
compiler	fixture clang
built_at_utc	2026-11-03T00:00:00Z
EOF

verify "$BASE_PREFIX" >/dev/null ||
	fail "a Simulator prefix with a software screen and no Vulkan driver was rejected"

# The same prefix is not a device or macOS prefix.
expect_failure "software prefix as the iOS device prefix" \
	cmake -DPREFIX="$BASE_PREFIX" -DSOURCE_ROOT="$ROOT_DIR" -DPLATFORM=ios \
	-P "$VERIFIER"
expect_failure "software prefix as the macOS prefix" \
	cmake -DPREFIX="$BASE_PREFIX" -DSOURCE_ROOT="$ROOT_DIR" -DPLATFORM=macos \
	-P "$VERIFIER"

# A Gallium library without the software screen has nothing to render with.
CASE_PREFIX="$TEST_ROOT/no-screen"
copy_fixture "$CASE_PREFIX"
cat >"$TEST_ROOT/gallium-bare.c" <<'EOF'
void *uml_mesa_gallium_fixture(void) { return 0; }
EOF
make_framework "$CASE_PREFIX" MesaGallium "$TEST_ROOT/gallium-bare.c"
expect_cmake_diagnostic \
	"MesaGallium.framework carries no software Gallium screen" \
	verify "$CASE_PREFIX"

# A reference to the creator is not a definition.
CASE_PREFIX="$TEST_ROOT/referenced-screen"
copy_fixture "$CASE_PREFIX"
cat >"$TEST_ROOT/gallium-reference.c" <<'EOF'
extern void *softpipe_create_screen(void);
void *uml_mesa_gallium_fixture(void) { return softpipe_create_screen(); }
EOF
make_framework "$CASE_PREFIX" MesaGallium "$TEST_ROOT/gallium-reference.c" \
	-Wl,-U,_softpipe_create_screen
expect_cmake_diagnostic \
	"MesaGallium.framework carries no software Gallium screen" \
	verify "$CASE_PREFIX"

# llvmpipe is the other software screen the stack can be built with.
CASE_PREFIX="$TEST_ROOT/llvmpipe-screen"
copy_fixture "$CASE_PREFIX"
cat >"$TEST_ROOT/gallium-llvmpipe.c" <<'EOF'
void *llvmpipe_create_screen(void) { return 0; }
void *uml_mesa_gallium_fixture(void) { return llvmpipe_create_screen(); }
EOF
make_framework "$CASE_PREFIX" MesaGallium "$TEST_ROOT/gallium-llvmpipe.c"
verify "$CASE_PREFIX" >/dev/null ||
	fail "a Simulator prefix with an llvmpipe screen was rejected"

# The Simulator has no Vulkan driver: neither its framework nor the headers
# may appear, and the Gallium library may not link it.
CASE_PREFIX="$TEST_ROOT/with-driver"
copy_fixture "$CASE_PREFIX"
cat >"$TEST_ROOT/driver.c" <<'EOF'
void *vkGetInstanceProcAddr(void *instance, const char *name)
{
    (void)instance;
    (void)name;
    return 0;
}
EOF
make_framework "$CASE_PREFIX" KosmicKrisp "$TEST_ROOT/driver.c"
expect_cmake_diagnostic "Unexpected Apple GPU frameworks" \
	verify "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/with-vulkan-headers"
copy_fixture "$CASE_PREFIX"
mkdir -p "$CASE_PREFIX/include/vulkan"
printf '%s\n' fixture >"$CASE_PREFIX/include/vulkan/vulkan.h"
expect_cmake_diagnostic "Unexpected Apple GPU include payload" \
	verify "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/gallium-vulkan"
copy_fixture "$CASE_PREFIX"
cat >"$TEST_ROOT/gallium-vulkan.c" <<'EOF'
extern void *vkGetInstanceProcAddr(void *, const char *);
void *softpipe_create_screen(void)
{
    return vkGetInstanceProcAddr(0, "vkCreateInstance");
}
void *uml_mesa_gallium_fixture(void) { return softpipe_create_screen(); }
EOF
make_framework "$CASE_PREFIX" MesaGallium "$TEST_ROOT/gallium-vulkan.c" \
	-Wl,-U,_vkGetInstanceProcAddr
expect_cmake_diagnostic \
	"MesaGallium.framework contains a forbidden provider symbol" \
	verify "$CASE_PREFIX"

# The receipt of a software build names no protocol.
CASE_PREFIX="$TEST_ROOT/receipt-protocol"
copy_fixture "$CASE_PREFIX"
printf 'venus_protocol_generated_sha256\t%s\n' \
	0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef \
	>>"$CASE_PREFIX/share/uml-dependencies/build-receipt.tsv"
expect_cmake_diagnostic \
	"Unexpected or duplicate Apple GPU receipt field: venus_protocol_generated_sha256" \
	verify "$CASE_PREFIX"

# A device-platform binary is not a Simulator binary.
CASE_PREFIX="$TEST_ROOT/device-binary"
copy_fixture "$CASE_PREFIX"
xcrun --sdk iphoneos clang -arch arm64 -target arm64-apple-ios27.0 \
	-dynamiclib "$TEST_ROOT/gallium.c" \
	-Wl,-install_name,@rpath/MesaGallium.framework/MesaGallium \
	-o "$CASE_PREFIX/Frameworks/MesaGallium.framework/MesaGallium"
expect_failure "device-platform MesaGallium in the Simulator prefix" \
	verify "$CASE_PREFIX"

printf '%s\n' "Apple GPU software prefix tests passed"
