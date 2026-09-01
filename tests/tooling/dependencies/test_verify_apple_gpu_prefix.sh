#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
VERIFIER="$ROOT_DIR/dependencies/cmake/VerifyAppleGpuPrefix.cmake"
BUILDER="$ROOT_DIR/dependencies/build-apple-gpu.sh"
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/uml-apple-gpu-prefix.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM
BASE_PREFIX="$TEST_ROOT/prefix"
FAILURE_NUMBER=0

fail()
{
	printf '%s\n' "test-verify-apple-gpu-prefix.sh: $*" >&2
	exit 1
}

verify()
{
	cmake -DPREFIX="$1" -DSOURCE_ROOT="$ROOT_DIR" -DPLATFORM=macos \
		-P "$VERIFIER"
}

copy_fixture()
{
	cp -R "$BASE_PREFIX" "$1"
}

expect_failure()
{
	description=$1
	prefix=$2
	FAILURE_NUMBER=$((FAILURE_NUMBER + 1))
	if verify "$prefix" >"$TEST_ROOT/rejected-$FAILURE_NUMBER.out" \
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
	minimum=$3
	binary="$BASE_PREFIX/Frameworks/$framework.framework/$framework"
	mkdir -p "$(dirname -- "$binary")"
	xcrun --sdk macosx clang -arch arm64 \
		-mmacosx-version-min="$minimum" -dynamiclib "$source" \
		-Wl,-install_name,"@rpath/$framework.framework/$framework" \
		-o "$binary"
	cat >"$(dirname -- "$binary")/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>$framework</string>
<key>CFBundleIdentifier</key><string>org.linux-um-darwin.fixture.$framework</string>
<key>CFBundlePackageType</key><string>FMWK</string>
<key>LSMinimumSystemVersion</key><string>$minimum</string>
</dict></plist>
EOF
}

command -v xcrun >/dev/null 2>&1 || fail "xcrun is required"
mkdir -p "$BASE_PREFIX/Frameworks" "$BASE_PREFIX/lib" \
	"$BASE_PREFIX/share/uml-dependencies/patches" \
	"$BASE_PREFIX/include/EGL" "$BASE_PREFIX/include/GLES" \
	"$BASE_PREFIX/include/GLES2" "$BASE_PREFIX/include/GLES3" \
	"$BASE_PREFIX/include/KHR" "$BASE_PREFIX/include/epoxy" \
	"$BASE_PREFIX/include/virgl"

cat >"$TEST_ROOT/fixture.c" <<'EOF'
int uml_apple_gpu_fixture(void) { return 0; }
void *eglGetPlatformDisplayEXT(unsigned platform, void *display, const int *attributes)
{
    (void)platform;
    (void)display;
    (void)attributes;
    return 0;
}
EOF
cat >"$TEST_ROOT/missing-bootstrap.c" <<'EOF'
int uml_apple_gpu_missing_bootstrap(void) { return 0; }
EOF
cat >"$TEST_ROOT/forbidden.c" <<'EOF'
extern void CGLUmlForbiddenFixture(void);
void uml_apple_gpu_forbidden_fixture(void) { CGLUmlForbiddenFixture(); }
EOF
xcrun --sdk macosx clang -arch arm64 -mmacosx-version-min=14.0 \
	-c "$TEST_ROOT/fixture.c" -o "$TEST_ROOT/fixture.o"
xcrun ar rcs "$BASE_PREFIX/lib/libepoxy.a" "$TEST_ROOT/fixture.o"
xcrun ar rcs "$BASE_PREFIX/lib/libvirglrenderer.a" "$TEST_ROOT/fixture.o"
make_framework EGL "$TEST_ROOT/fixture.c" 14.0
make_framework GLESv2 "$TEST_ROOT/fixture.c" 14.0

for header in \
	EGL/egl.h \
	epoxy/egl.h \
	virgl/virglrenderer.h \
	virgl/virgl_hw.h; do
	printf '%s\n' fixture >"$BASE_PREFIX/include/$header"
done
cp "$ROOT_DIR/dependencies/manifest.tsv" \
	"$BASE_PREFIX/share/uml-dependencies/manifest.tsv"
cp "$ROOT_DIR/dependencies/platforms/macos-gpu-arm64.tsv" \
	"$BASE_PREFIX/share/uml-dependencies/macos-gpu-arm64.tsv"
for component in utm-webkit utm-libepoxy utm-virglrenderer; do
	cp -R "$ROOT_DIR/dependencies/patches/$component" \
		"$BASE_PREFIX/share/uml-dependencies/patches/"
done

verify "$BASE_PREFIX" >/dev/null

CASE_PREFIX="$TEST_ROOT/wrong-contract"
copy_fixture "$CASE_PREFIX"
sed 's/^format.*/format\t2/' \
	"$CASE_PREFIX/share/uml-dependencies/macos-gpu-arm64.tsv" \
	>"$CASE_PREFIX/share/uml-dependencies/macos-gpu-arm64.tsv.changed"
mv "$CASE_PREFIX/share/uml-dependencies/macos-gpu-arm64.tsv.changed" \
	"$CASE_PREFIX/share/uml-dependencies/macos-gpu-arm64.tsv"
expect_failure "wrong platform contract" "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/tampered-provenance"
copy_fixture "$CASE_PREFIX"
sed 's/^utm-webkit\ted78ab6\t/utm-webkit\ttampered\t/' \
	"$CASE_PREFIX/share/uml-dependencies/manifest.tsv" \
	>"$CASE_PREFIX/share/uml-dependencies/manifest.tsv.changed"
mv "$CASE_PREFIX/share/uml-dependencies/manifest.tsv.changed" \
	"$CASE_PREFIX/share/uml-dependencies/manifest.tsv"
expect_failure "tampered installed provenance" "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/unexpected-library"
copy_fixture "$CASE_PREFIX"
cp "$TEST_ROOT/fixture.o" "$CASE_PREFIX/lib/libambient.dylib"
expect_failure "unexpected library" "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/symlinked-payload"
copy_fixture "$CASE_PREFIX"
ln -s "$TEST_ROOT/fixture.o" "$CASE_PREFIX/include/ambient"
expect_failure "symlinked payload" "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/wrong-architecture"
copy_fixture "$CASE_PREFIX"
xcrun --sdk macosx clang -arch x86_64 -mmacosx-version-min=14.0 \
	-dynamiclib "$TEST_ROOT/fixture.c" \
	-Wl,-install_name,@rpath/EGL.framework/EGL \
	-o "$CASE_PREFIX/Frameworks/EGL.framework/EGL"
expect_failure "wrong Mach-O architecture" "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/wrong-deployment-target"
copy_fixture "$CASE_PREFIX"
xcrun --sdk macosx clang -arch arm64 -mmacosx-version-min=15.0 \
	-dynamiclib "$TEST_ROOT/fixture.c" \
	-Wl,-install_name,@rpath/EGL.framework/EGL \
	-o "$CASE_PREFIX/Frameworks/EGL.framework/EGL"
expect_failure "wrong Mach-O deployment target" "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/missing-platform-bootstrap"
copy_fixture "$CASE_PREFIX"
xcrun --sdk macosx clang -arch arm64 -mmacosx-version-min=14.0 \
	-dynamiclib "$TEST_ROOT/missing-bootstrap.c" \
	-Wl,-install_name,@rpath/EGL.framework/EGL \
	-o "$CASE_PREFIX/Frameworks/EGL.framework/EGL"
expect_failure "missing ANGLE platform bootstrap" "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/forbidden-provider"
copy_fixture "$CASE_PREFIX"
xcrun --sdk macosx clang -arch arm64 -mmacosx-version-min=14.0 \
	-dynamiclib -Wl,-undefined,dynamic_lookup \
	"$TEST_ROOT/forbidden.c" \
	-Wl,-install_name,@rpath/GLESv2.framework/GLESv2 \
	-o "$CASE_PREFIX/Frameworks/GLESv2.framework/GLESv2"
expect_failure "forbidden CGL provider symbol" "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/forbidden-static-provider"
copy_fixture "$CASE_PREFIX"
xcrun --sdk macosx clang -arch arm64 -mmacosx-version-min=14.0 \
	-c "$TEST_ROOT/forbidden.c" -o "$TEST_ROOT/forbidden.o"
xcrun ar rcs "$CASE_PREFIX/lib/libvirglrenderer.a" \
	"$TEST_ROOT/forbidden.o"
expect_failure "forbidden static CGL provider symbol" "$CASE_PREFIX"

CASE_PREFIX="$TEST_ROOT/invalid-existing-prefix"
copy_fixture "$CASE_PREFIX"
printf '%s\n' ambient >"$CASE_PREFIX/lib/unexpected"
BEFORE=$(find "$CASE_PREFIX" -type f -exec shasum -a 256 {} \; | LC_ALL=C sort)
if "$BUILDER" --platform macos --prefix "$CASE_PREFIX" --verify-only \
	>"$TEST_ROOT/builder-rejected.out" 2>"$TEST_ROOT/builder-rejected.err"; then
	fail "invalid existing prefix was incorrectly accepted"
fi
AFTER=$(find "$CASE_PREFIX" -type f -exec shasum -a 256 {} \; | LC_ALL=C sort)
[ "$AFTER" = "$BEFORE" ] || fail "invalid existing prefix was modified"

printf '%s\n' "Apple GPU dependency prefix verification tests passed"
