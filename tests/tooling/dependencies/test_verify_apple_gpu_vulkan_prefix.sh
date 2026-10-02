#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
VERIFIER="$ROOT_DIR/dependencies/cmake/VerifyAppleGpuPrefix.cmake"
BUILDER="$ROOT_DIR/dependencies/build-apple-gpu.sh"
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/uml-apple-gpu-vulkan-prefix.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM
BASE_PREFIX="$TEST_ROOT/prefix"

TEST_NAME=test-verify-apple-gpu-vulkan-prefix.sh
. "$(dirname -- "$0")/../support/expect.sh"

verify()
{
	platform=${2:-macos}
	cmake -DPREFIX="$1" -DSOURCE_ROOT="$ROOT_DIR" \
		-DPLATFORM="$platform" -P "$VERIFIER"
}

copy_fixture()
{
	cp -R "$BASE_PREFIX" "$1"
}

# make_framework PREFIX FRAMEWORK SOURCE [CLANG-ARGUMENT...]: build a fixture
# framework into PREFIX. INSTALL_NAME and MINIMUM_OS override its install name
# and Info.plist deployment target.
make_framework()
{
	prefix=$1
	framework=$2
	source=$3
	shift 3
	binary="$prefix/Frameworks/$framework.framework/$framework"
	mkdir -p "$(dirname -- "$binary")"
	xcrun --sdk macosx clang -arch arm64 -mmacosx-version-min=26.0 \
		-dynamiclib "$source" "$@" \
		-Wl,-install_name,"${INSTALL_NAME:-@rpath/$framework.framework/$framework}" \
		-o "$binary"
	cat >"$(dirname -- "$binary")/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>$framework</string>
<key>CFBundleIdentifier</key><string>org.linux-um-darwin.fixture.$framework</string>
<key>CFBundlePackageType</key><string>FMWK</string>
<key>LSMinimumSystemVersion</key><string>${MINIMUM_OS:-26.0}</string>
</dict></plist>
EOF
}

# expect_framework_rejection DIAGNOSTIC FRAMEWORK SOURCE [CLANG-ARGUMENT...]:
# a copy of the fixture whose FRAMEWORK is rebuilt from SOURCE must be
# rejected with DIAGNOSTIC.
FRAMEWORK_CASE=0
expect_framework_rejection()
{
	diagnostic=$1
	shift
	FRAMEWORK_CASE=$((FRAMEWORK_CASE + 1))
	CASE_PREFIX="$TEST_ROOT/framework-$FRAMEWORK_CASE"
	copy_fixture "$CASE_PREFIX"
	make_framework "$CASE_PREFIX" "$@"
	expect_cmake_diagnostic "$diagnostic" verify "$CASE_PREFIX"
}

command -v xcrun >/dev/null 2>&1 || fail "xcrun is required"
command -v python3 >/dev/null 2>&1 || fail "python3 is required"
mkdir -p "$BASE_PREFIX/Frameworks" "$BASE_PREFIX/lib" \
	"$BASE_PREFIX/share/uml-dependencies" \
	"$BASE_PREFIX/include/EGL" "$BASE_PREFIX/include/GL" \
	"$BASE_PREFIX/include/KHR" "$BASE_PREFIX/include/epoxy" \
	"$BASE_PREFIX/include/virgl" \
	"$BASE_PREFIX/include/vulkan" "$BASE_PREFIX/include/vk_video"

cat >"$TEST_ROOT/fixture.c" <<'EOF'
int uml_apple_gpu_fixture(void) { return 0; }
EOF
cat >"$TEST_ROOT/mesa-egl.c" <<'EOF'
extern void *uml_mesa_gallium_fixture(void);
void *eglGetPlatformDisplay(unsigned platform, void *display, const long *attributes)
{
    (void)platform;
    (void)display;
    (void)attributes;
    return uml_mesa_gallium_fixture();
}
EOF
cat >"$TEST_ROOT/mesa-gallium.c" <<'EOF'
extern void *vkGetInstanceProcAddr(void *, const char *);
void *uml_mesa_gallium_fixture(void)
{
    return vkGetInstanceProcAddr(0, "vkCreateInstance");
}
EOF
cat >"$TEST_ROOT/virgl.c" <<'EOF'
extern void *vkGetInstanceProcAddr(void *, const char *);
void *uml_direct_vulkan_provider(void)
{
    return vkGetInstanceProcAddr(0, "vkCreateInstance");
}
EOF
cat >"$TEST_ROOT/kosmickrisp.c" <<'EOF'
void *vkGetInstanceProcAddr(void *instance, const char *name)
{
    (void)instance;
    (void)name;
    return 0;
}
EOF
xcrun --sdk macosx clang -arch arm64 -mmacosx-version-min=26.0 \
	-c "$TEST_ROOT/fixture.c" -o "$TEST_ROOT/fixture.o"
xcrun --sdk macosx clang -arch arm64 -mmacosx-version-min=26.0 \
	-c "$TEST_ROOT/virgl.c" -o "$TEST_ROOT/virgl.o"
xcrun ar rcs "$BASE_PREFIX/lib/libepoxy.a" "$TEST_ROOT/fixture.o"
xcrun ar rcs "$BASE_PREFIX/lib/libvirglrenderer.a" "$TEST_ROOT/virgl.o"
make_framework "$BASE_PREFIX" KosmicKrisp "$TEST_ROOT/kosmickrisp.c"
make_framework "$BASE_PREFIX" MesaGallium "$TEST_ROOT/mesa-gallium.c" \
	"$BASE_PREFIX/Frameworks/KosmicKrisp.framework/KosmicKrisp"
make_framework "$BASE_PREFIX" MesaEGL "$TEST_ROOT/mesa-egl.c" \
	"$BASE_PREFIX/Frameworks/MesaGallium.framework/MesaGallium"

for header in \
	EGL/egl.h \
	epoxy/egl.h \
	virgl/virglrenderer.h \
	virgl/virgl_hw.h \
	vulkan/vulkan.h; do
	printf '%s\n' fixture >"$BASE_PREFIX/include/$header"
done
KOSMICKRISP_FRAMEWORK="$BASE_PREFIX/Frameworks/KosmicKrisp.framework"
mkdir -p "$KOSMICKRISP_FRAMEWORK/Headers" \
	"$KOSMICKRISP_FRAMEWORK/Modules"
printf '%s\n' '#include <vulkan/vulkan.h>' \
	>"$KOSMICKRISP_FRAMEWORK/Headers/KosmicKrisp.h"
cat >"$KOSMICKRISP_FRAMEWORK/Modules/module.modulemap" <<'EOF'
framework module KosmicKrisp {
    umbrella header "KosmicKrisp.h"
    export *
}
EOF

cp "$ROOT_DIR/dependencies/manifest.tsv" \
	"$BASE_PREFIX/share/uml-dependencies/manifest.tsv"
cp "$ROOT_DIR/dependencies/platforms/macos-gpu-vulkan-arm64.tsv" \
	"$BASE_PREFIX/share/uml-dependencies/macos-gpu-vulkan-arm64.tsv"
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
MANIFEST_SHA=$(shasum -a 256 "$ROOT_DIR/dependencies/manifest.tsv" | awk '{print $1}')
CONTRACT_SHA=$(shasum -a 256 \
	"$ROOT_DIR/dependencies/platforms/macos-gpu-vulkan-arm64.tsv" | awk '{print $1}')
cat >"$BASE_PREFIX/share/uml-dependencies/build-receipt.tsv" <<EOF
field	value
format	2
product	apple-gpu-vulkan-experiment
platform	macos
sdk_name	macosx
sdk_version	fixture
sdk_path	/fixture/MacOSX.sdk
compiler	fixture clang
built_at_utc	2026-08-23T00:00:00Z
source_manifest_sha256	$MANIFEST_SHA
platform_contract_sha256	$CONTRACT_SHA
mesa_base_commit	aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
vulkan_headers_base_commit	bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb
venus_protocol_generated_sha256	0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
EOF

verify "$BASE_PREFIX" >/dev/null

expect_failure "wrong platform" verify "$BASE_PREFIX" ios-simulator

# A prefix is current exactly while each source tree matches its series.
for component in virglrenderer mesa venus-protocol; do
	CASE_PREFIX="$TEST_ROOT/stale-$component"
	copy_fixture "$CASE_PREFIX"
	awk -F '\t' -v OFS='\t' -v component="$component" \
		'$1 == component { $2 = "0000000000000000000000000000000000000000" }
		{ print }' "$BASE_PREFIX/share/uml-dependencies/source-trees.tsv" \
		>"$CASE_PREFIX/share/uml-dependencies/source-trees.tsv"
	expect_failure "stale $component source tree" verify "$CASE_PREFIX"
done

CASE_PREFIX="$TEST_ROOT/missing-source-tree"
copy_fixture "$CASE_PREFIX"
grep -v '^libepoxy' "$BASE_PREFIX/share/uml-dependencies/source-trees.tsv" \
	>"$CASE_PREFIX/share/uml-dependencies/source-trees.tsv"
expect_failure "missing source tree" verify "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/extra-source-tree"
copy_fixture "$CASE_PREFIX"
printf 'vulkan-headers\t%s\n' 0000000000000000000000000000000000000000 \
	>>"$CASE_PREFIX/share/uml-dependencies/source-trees.tsv"
expect_failure "unexpected source tree" verify "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/installed-patches"
copy_fixture "$CASE_PREFIX"
mkdir -p "$CASE_PREFIX/share/uml-dependencies/patches"
cp -R "$ROOT_DIR/dependencies/patches/mesa" \
	"$CASE_PREFIX/share/uml-dependencies/patches/"
expect_failure "installed patch files" verify "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/invalid-protocol-identity"
copy_fixture "$CASE_PREFIX"
sed 's/^venus_protocol_generated_sha256.*/venus_protocol_generated_sha256\tunknown/' \
	"$BASE_PREFIX/share/uml-dependencies/build-receipt.tsv" \
	>"$CASE_PREFIX/share/uml-dependencies/build-receipt.tsv"
expect_failure "invalid protocol build identity" verify "$CASE_PREFIX"

# The installed platform contract must be the checked-in one.
CASE_PREFIX="$TEST_ROOT/edited-contract"
copy_fixture "$CASE_PREFIX"
sed 's/^helper_process\t.*/helper_process\tenabled/' \
	"$BASE_PREFIX/share/uml-dependencies/macos-gpu-vulkan-arm64.tsv" \
	>"$CASE_PREFIX/share/uml-dependencies/macos-gpu-vulkan-arm64.tsv"
expect_failure "edited platform contract" verify "$CASE_PREFIX"
grep -q 'provenance does not match' "$TEST_ROOT/rejected-$FAILURE_NUMBER.err" ||
	fail "an edited platform contract was not reported as a provenance mismatch"

CASE_PREFIX="$TEST_ROOT/empty-sdk-name"
copy_fixture "$CASE_PREFIX"
sed 's/^sdk_name\t.*/sdk_name\t/' \
	"$BASE_PREFIX/share/uml-dependencies/build-receipt.tsv" \
	>"$CASE_PREFIX/share/uml-dependencies/build-receipt.tsv"
expect_failure "empty receipt SDK name" verify "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/wrong-receipt-product"
copy_fixture "$CASE_PREFIX"
sed 's/^product\t.*/product\tother/' \
	"$BASE_PREFIX/share/uml-dependencies/build-receipt.tsv" \
	>"$CASE_PREFIX/share/uml-dependencies/build-receipt.tsv"
expect_failure "receipt for another product" verify "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/hidden-network-input"
copy_fixture "$CASE_PREFIX"
printf 'network_input\thttps://invalid.example/source\n' \
	>>"$CASE_PREFIX/share/uml-dependencies/build-receipt.tsv"
expect_failure "hidden network input" verify "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/duplicate-provider"
copy_fixture "$CASE_PREFIX"
cp -R "$CASE_PREFIX/Frameworks/KosmicKrisp.framework" \
	"$CASE_PREFIX/Frameworks/Vulkan.framework"
expect_failure "duplicate Vulkan provider" verify "$CASE_PREFIX"

# Each framework has its own dependency allowlist. The GL pair may use
# libexpat, Foundation, and its two Mesa edges; KosmicKrisp may use only Metal,
# QuartzCore, and the base system libraries.
GALLIUM="$BASE_PREFIX/Frameworks/MesaGallium.framework/MesaGallium"
KOSMICKRISP="$BASE_PREFIX/Frameworks/KosmicKrisp.framework/KosmicKrisp"
CASE_PREFIX="$TEST_ROOT/gl-allowlist"
copy_fixture "$CASE_PREFIX"
make_framework "$CASE_PREFIX" MesaEGL "$TEST_ROOT/mesa-egl.c" "$GALLIUM" \
	-lexpat -framework Foundation
verify "$CASE_PREFIX" >/dev/null ||
	fail "a GL framework using its allowed dependencies was rejected"

expect_framework_rejection \
	"KosmicKrisp.framework dependency: /usr/lib/libexpat.1.dylib" \
	KosmicKrisp "$TEST_ROOT/kosmickrisp.c" -lexpat
expect_framework_rejection \
	"KosmicKrisp.framework dependency: /System/Library/Frameworks/Foundation.framework" \
	KosmicKrisp "$TEST_ROOT/kosmickrisp.c" -framework Foundation
expect_framework_rejection \
	"MesaGallium.framework dependency: @rpath/MesaEGL.framework/MesaEGL" \
	MesaGallium "$TEST_ROOT/mesa-gallium.c" "$KOSMICKRISP" \
	"$BASE_PREFIX/Frameworks/MesaEGL.framework/MesaEGL"
expect_framework_rejection \
	"MesaEGL.framework dependency: @rpath/KosmicKrisp.framework/KosmicKrisp" \
	MesaEGL "$TEST_ROOT/mesa-egl.c" "$GALLIUM" "$KOSMICKRISP"
expect_framework_rejection \
	"MesaEGL.framework dependency: /System/Library/Frameworks/OpenGL.framework" \
	MesaEGL "$TEST_ROOT/mesa-egl.c" "$GALLIUM" -framework OpenGL

# An allowed system framework directory admits any path below it, so no
# framework may name a GL or Vulkan framework or loader anywhere in its closure.
printf '%s\n' 'int uml_apple_gpu_stub(void) { return 0; }' >"$TEST_ROOT/stub.c"
for nested in OpenGL Vulkan; do
	xcrun --sdk macosx clang -arch arm64 -mmacosx-version-min=26.0 \
		-dynamiclib "$TEST_ROOT/stub.c" \
		-Wl,-install_name,/System/Library/Frameworks/Metal.framework/Frameworks/$nested.framework/$nested \
		-o "$TEST_ROOT/nested-$nested.dylib"
done
expect_framework_rejection \
	"KosmicKrisp.framework contains a forbidden GL/Vulkan dependency" \
	KosmicKrisp "$TEST_ROOT/kosmickrisp.c" "$TEST_ROOT/nested-OpenGL.dylib"
expect_framework_rejection \
	"MesaEGL.framework contains a forbidden GL/Vulkan dependency" \
	MesaEGL "$TEST_ROOT/mesa-egl.c" "$GALLIUM" "$TEST_ROOT/nested-Vulkan.dylib"

# Install names, load commands, and deployment targets.
expect_framework_rejection "KosmicKrisp.framework has an invalid" \
	KosmicKrisp "$TEST_ROOT/kosmickrisp.c" -Wl,-rpath,@loader_path
expect_framework_rejection "MesaEGL.framework has an invalid" \
	MesaEGL "$TEST_ROOT/mesa-egl.c" "$GALLIUM" -Wl,-rpath,@loader_path
# sh may keep an assignment that prefixes a function call, so these are set
# and unset explicitly.
INSTALL_NAME=@rpath/Other.framework/Other
expect_framework_rejection \
	"KosmicKrisp.framework has an invalid install name" \
	KosmicKrisp "$TEST_ROOT/kosmickrisp.c"
expect_framework_rejection \
	"MesaGallium.framework has an invalid install name" \
	MesaGallium "$TEST_ROOT/mesa-gallium.c" "$KOSMICKRISP"
# The install name is the whole name, not a prefix of it.
INSTALL_NAME="@rpath/KosmicKrisp.framework/KosmicKrisp extra"
expect_framework_rejection \
	"KosmicKrisp.framework has an invalid install name" \
	KosmicKrisp "$TEST_ROOT/kosmickrisp.c"
INSTALL_NAME="@rpath/MesaGallium.framework/MesaGallium extra"
expect_framework_rejection \
	"MesaGallium.framework has an invalid install name" \
	MesaGallium "$TEST_ROOT/mesa-gallium.c" "$KOSMICKRISP"
unset INSTALL_NAME
MINIMUM_OS=25.0
expect_framework_rejection \
	"KosmicKrisp.framework has an invalid deployment target" \
	KosmicKrisp "$TEST_ROOT/kosmickrisp.c"
expect_framework_rejection \
	"MesaGallium.framework has an invalid deployment target" \
	MesaGallium "$TEST_ROOT/mesa-gallium.c" "$KOSMICKRISP"
unset MINIMUM_OS

# Symbols and payloads.
expect_framework_rejection \
	"KosmicKrisp.framework does not export vkGetInstanceProcAddr" \
	KosmicKrisp "$TEST_ROOT/fixture.c"
expect_framework_rejection \
	"MesaEGL.framework does not export the EGL platform bootstrap" \
	MesaEGL "$TEST_ROOT/fixture.c"
cat >"$TEST_ROOT/mesa-gallium-provider.c" <<'EOF'
extern void *vkGetInstanceProcAddr(void *, const char *);
extern int vkCreateInstance(const void *, const void *, void **);
void *uml_mesa_gallium_fixture(void)
{
    (void)vkCreateInstance(0, 0, 0);
    return vkGetInstanceProcAddr(0, "vkCreateInstance");
}
EOF
expect_framework_rejection \
	"MesaGallium.framework contains a forbidden provider symbol" \
	MesaGallium "$TEST_ROOT/mesa-gallium-provider.c" "$KOSMICKRISP" \
	-Wl,-U,_vkCreateInstance

CASE_PREFIX="$TEST_ROOT/gl-payload"
copy_fixture "$CASE_PREFIX"
mkdir -p "$CASE_PREFIX/Frameworks/MesaEGL.framework/Headers"
printf '%s\n' fixture >"$CASE_PREFIX/Frameworks/MesaEGL.framework/Headers/EGL.h"
expect_cmake_diagnostic "Unexpected MesaEGL.framework payload" verify "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/module-map"
copy_fixture "$CASE_PREFIX"
printf '%s\n' 'module Other {}' \
	>"$CASE_PREFIX/Frameworks/KosmicKrisp.framework/Modules/module.modulemap"
expect_cmake_diagnostic "KosmicKrisp.framework has an invalid module map" \
	verify "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/invalid-existing-prefix"
copy_fixture "$CASE_PREFIX"
printf '%s\n' ambient >"$CASE_PREFIX/lib/unexpected"
BEFORE=$(find "$CASE_PREFIX" -type f -exec shasum -a 256 {} \; | LC_ALL=C sort)
if "$BUILDER" --platform macos --prefix "$CASE_PREFIX" --verify-only \
	>"$TEST_ROOT/builder-rejected.out" 2>"$TEST_ROOT/builder-rejected.err"; then
	fail "invalid existing Vulkan prefix was incorrectly accepted"
fi
AFTER=$(find "$CASE_PREFIX" -type f -exec shasum -a 256 {} \; | LC_ALL=C sort)
[ "$AFTER" = "$BEFORE" ] || fail "invalid existing Vulkan prefix was modified"

printf '%s\n' "Experimental Apple GPU Vulkan prefix tests passed"
