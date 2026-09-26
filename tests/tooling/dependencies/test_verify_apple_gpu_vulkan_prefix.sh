#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
VERIFIER="$ROOT_DIR/dependencies/cmake/VerifyAppleGpuPrefix.cmake"
BUILDER="$ROOT_DIR/dependencies/build-apple-gpu.sh"
PUBLISHER="$ROOT_DIR/dependencies/publish-prefix.py"
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/uml-apple-gpu-vulkan-prefix.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM
BASE_PREFIX="$TEST_ROOT/prefix"
FAILURE_NUMBER=0

fail()
{
	printf '%s\n' "test-verify-apple-gpu-vulkan-prefix.sh: $*" >&2
	exit 1
}

verify()
{
	platform=${2:-macos}
	cmake -DPREFIX="$1" -DSOURCE_ROOT="$ROOT_DIR" \
		-DPLATFORM="$platform" \
		-DPRODUCT=apple-gpu-vulkan-experiment -P "$VERIFIER"
}

copy_fixture()
{
	cp -R "$BASE_PREFIX" "$1"
}

expect_failure()
{
	description=$1
	prefix=$2
	platform=${3:-macos}
	FAILURE_NUMBER=$((FAILURE_NUMBER + 1))
	if verify "$prefix" "$platform" \
		>"$TEST_ROOT/rejected-$FAILURE_NUMBER.out" \
		2>"$TEST_ROOT/rejected-$FAILURE_NUMBER.err"; then
		fail "$description was incorrectly accepted"
	fi
	[ -s "$TEST_ROOT/rejected-$FAILURE_NUMBER.err" ] ||
		fail "$description was rejected without a diagnostic"
}

make_framework()
{
	framework=$1
	source=$2
	shift 2
	binary="$BASE_PREFIX/Frameworks/$framework.framework/$framework"
	mkdir -p "$(dirname -- "$binary")"
	xcrun --sdk macosx clang -arch arm64 -mmacosx-version-min=26.0 \
		-dynamiclib "$source" "$@" \
		-Wl,-install_name,"@rpath/$framework.framework/$framework" \
		-o "$binary"
	cat >"$(dirname -- "$binary")/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>$framework</string>
<key>CFBundleIdentifier</key><string>org.linux-um-darwin.fixture.$framework</string>
<key>CFBundlePackageType</key><string>FMWK</string>
<key>LSMinimumSystemVersion</key><string>26.0</string>
</dict></plist>
EOF
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
make_framework KosmicKrisp "$TEST_ROOT/kosmickrisp.c"
make_framework MesaGallium "$TEST_ROOT/mesa-gallium.c" \
	"$BASE_PREFIX/Frameworks/KosmicKrisp.framework/KosmicKrisp"
make_framework MesaEGL "$TEST_ROOT/mesa-egl.c" \
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

expect_failure "wrong platform" "$BASE_PREFIX" ios-simulator

# A prefix is current exactly while each source tree matches its series.
for component in virglrenderer mesa venus-protocol; do
	CASE_PREFIX="$TEST_ROOT/stale-$component"
	copy_fixture "$CASE_PREFIX"
	awk -F '\t' -v OFS='\t' -v component="$component" \
		'$1 == component { $2 = "0000000000000000000000000000000000000000" }
		{ print }' "$BASE_PREFIX/share/uml-dependencies/source-trees.tsv" \
		>"$CASE_PREFIX/share/uml-dependencies/source-trees.tsv"
	expect_failure "stale $component source tree" "$CASE_PREFIX"
done

CASE_PREFIX="$TEST_ROOT/missing-source-tree"
copy_fixture "$CASE_PREFIX"
grep -v '^libepoxy' "$BASE_PREFIX/share/uml-dependencies/source-trees.tsv" \
	>"$CASE_PREFIX/share/uml-dependencies/source-trees.tsv"
expect_failure "missing source tree" "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/extra-source-tree"
copy_fixture "$CASE_PREFIX"
printf 'vulkan-headers\t%s\n' 0000000000000000000000000000000000000000 \
	>>"$CASE_PREFIX/share/uml-dependencies/source-trees.tsv"
expect_failure "unexpected source tree" "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/installed-patches"
copy_fixture "$CASE_PREFIX"
mkdir -p "$CASE_PREFIX/share/uml-dependencies/patches"
cp -R "$ROOT_DIR/dependencies/patches/mesa" \
	"$CASE_PREFIX/share/uml-dependencies/patches/"
expect_failure "installed patch files" "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/invalid-protocol-identity"
copy_fixture "$CASE_PREFIX"
sed 's/^venus_protocol_generated_sha256.*/venus_protocol_generated_sha256\tunknown/' \
	"$BASE_PREFIX/share/uml-dependencies/build-receipt.tsv" \
	>"$CASE_PREFIX/share/uml-dependencies/build-receipt.tsv"
expect_failure "invalid protocol build identity" "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/hidden-network-input"
copy_fixture "$CASE_PREFIX"
printf 'network_input\thttps://invalid.example/source\n' \
	>>"$CASE_PREFIX/share/uml-dependencies/build-receipt.tsv"
expect_failure "hidden network input" "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/duplicate-provider"
copy_fixture "$CASE_PREFIX"
cp -R "$CASE_PREFIX/Frameworks/KosmicKrisp.framework" \
	"$CASE_PREFIX/Frameworks/Vulkan.framework"
expect_failure "duplicate Vulkan provider" "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/invalid-existing-prefix"
copy_fixture "$CASE_PREFIX"
printf '%s\n' ambient >"$CASE_PREFIX/lib/unexpected"
BEFORE=$(find "$CASE_PREFIX" -type f -exec shasum -a 256 {} \; | LC_ALL=C sort)
if "$BUILDER" --product apple-gpu-vulkan-experiment \
	--platform macos --prefix "$CASE_PREFIX" --verify-only \
	>"$TEST_ROOT/builder-rejected.out" 2>"$TEST_ROOT/builder-rejected.err"; then
	fail "invalid existing Vulkan prefix was incorrectly accepted"
fi
AFTER=$(find "$CASE_PREFIX" -type f -exec shasum -a 256 {} \; | LC_ALL=C sort)
[ "$AFTER" = "$BEFORE" ] || fail "invalid existing Vulkan prefix was modified"

PUBLISH_ROOT="$TEST_ROOT/concurrent-publication"
mkdir -p "$PUBLISH_ROOT/first" "$PUBLISH_ROOT/second"
printf '%s\n' first >"$PUBLISH_ROOT/first/identity"
printf '%s\n' second >"$PUBLISH_ROOT/second/identity"
python3 "$PUBLISHER" "$PUBLISH_ROOT/first" "$PUBLISH_ROOT/winner" &
FIRST_PID=$!
python3 "$PUBLISHER" "$PUBLISH_ROOT/second" "$PUBLISH_ROOT/winner" &
SECOND_PID=$!
FIRST_STATUS=0
SECOND_STATUS=0
wait "$FIRST_PID" || FIRST_STATUS=$?
wait "$SECOND_PID" || SECOND_STATUS=$?
case "$FIRST_STATUS:$SECOND_STATUS" in
0:1|1:0) ;;
*) fail "exclusive publication returned $FIRST_STATUS and $SECOND_STATUS" ;;
esac
[ -f "$PUBLISH_ROOT/winner/identity" ] ||
	fail "concurrent publication produced no winner"
LOSERS=$(find "$PUBLISH_ROOT" -mindepth 1 -maxdepth 1 -type d \
	\( -name first -o -name second \) | wc -l | tr -d ' ')
[ "$LOSERS" -eq 1 ] || fail "concurrent publication did not retain one loser"

grep -Fq -- '-Dvulkan-drivers=kosmickrisp' "$BUILDER" ||
	fail "builder does not select the KosmicKrisp Vulkan driver"
grep -Fq -- '-Dgallium-drivers=zink' "$BUILDER" ||
	fail "builder does not select the Zink OpenGL driver"
grep -Fq -- '-Dzink-vulkan-library-file=' "$BUILDER" ||
	fail "builder does not link Zink against the packaged Vulkan driver"
grep -Fq -- '-Dllvm=disabled' "$BUILDER" ||
	fail "builder does not use the embedded KosmicKrisp shader blobs"
grep -Fq -- '--wrap-mode=nodownload' "$BUILDER" ||
	fail "builder does not disable Meson downloads"
grep -Fq 'env -i PATH="$MESA_TOOL_PATH"' "$BUILDER" ||
	fail "builder does not sanitize downstream build environments"

printf '%s\n' "Experimental Apple GPU Vulkan prefix tests passed"
