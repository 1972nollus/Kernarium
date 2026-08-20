#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
VERIFIER="$ROOT_DIR/dependencies/verify-runtime-dependencies.sh"
TEST_PARENT=${TMPDIR:-/tmp}
TEST_ROOT=$(mktemp -d "${TEST_PARENT%/}/uml-runtime-dependencies.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM

expect_rejection()
{
	diagnostic=$1
	shift
	if "$VERIFIER" "$@" >"$TEST_ROOT/rejected.out" \
		2>"$TEST_ROOT/rejected.err"; then
		printf '%s\n' "runtime dependency verifier accepted: $diagnostic" >&2
		exit 1
	fi
	grep -F "$diagnostic" "$TEST_ROOT/rejected.err" >/dev/null || {
		printf '%s\n' "runtime rejection lacked diagnostic: $diagnostic" >&2
		cat "$TEST_ROOT/rejected.err" >&2
		exit 1
	}
}

"$VERIFIER" --binary /usr/bin/true >/dev/null

cat >"$TEST_ROOT/library.c" <<'EOF'
int uml_runtime_dependency_fixture(void) { return 0; }
EOF
cat >"$TEST_ROOT/main.c" <<'EOF'
int uml_runtime_dependency_fixture(void);
int main(void) { return uml_runtime_dependency_fixture(); }
EOF
xcrun --sdk macosx clang -arch arm64 -dynamiclib \
	-Wl,-install_name,"$TEST_ROOT/libambient.dylib" \
	"$TEST_ROOT/library.c" -o "$TEST_ROOT/libambient.dylib"
xcrun --sdk macosx clang -arch arm64 "$TEST_ROOT/main.c" \
	"$TEST_ROOT/libambient.dylib" -o "$TEST_ROOT/ambient-client"
expect_rejection 'non-system dynamic dependency' \
	--binary "$TEST_ROOT/ambient-client"

PREFIX="$TEST_ROOT/prefix"
RUNTIME="$TEST_ROOT/runtime"
mkdir -p -- "$PREFIX/Frameworks/EGL.framework" \
	"$PREFIX/Frameworks/GLESv2.framework" "$RUNTIME/Frameworks"
cat >"$TEST_ROOT/egl.c" <<'EOF'
int uml_egl_fixture(void) { return 19; }
EOF
cat >"$TEST_ROOT/gles.c" <<'EOF'
int uml_gles_fixture(void) { return 23; }
EOF
cat >"$TEST_ROOT/framework-main.c" <<'EOF'
#include <stdio.h>
int uml_egl_fixture(void);
int uml_gles_fixture(void);
int main(void)
{
	puts("Virgl ANGLE/Metal provider: hardware");
	return uml_egl_fixture() == uml_gles_fixture();
}
EOF
cat >"$TEST_ROOT/framework-main-missing-marker.c" <<'EOF'
int uml_egl_fixture(void);
int uml_gles_fixture(void);
int main(void) { return uml_egl_fixture() == uml_gles_fixture(); }
EOF
for framework in EGL GLESv2; do
	case "$framework" in
	EGL) source="$TEST_ROOT/egl.c" ;;
	GLESv2) source="$TEST_ROOT/gles.c" ;;
	esac
	xcrun --sdk macosx clang -arch arm64 -dynamiclib \
		-Wl,-install_name,"@rpath/$framework.framework/$framework" \
		"$source" -o "$PREFIX/Frameworks/$framework.framework/$framework"
	cat >"$PREFIX/Frameworks/$framework.framework/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict><key>CFBundleExecutable</key><string>$framework</string></dict></plist>
EOF
done
cp -R "$PREFIX/Frameworks/EGL.framework" "$RUNTIME/Frameworks/"
cp -R "$PREFIX/Frameworks/GLESv2.framework" "$RUNTIME/Frameworks/"
xcrun --sdk macosx clang -arch arm64 "$TEST_ROOT/framework-main.c" \
	-F"$PREFIX/Frameworks" -framework EGL -framework GLESv2 \
	-Wl,-rpath,@executable_path/Frameworks -o "$RUNTIME/apple-gpu-client"

verify_framework_runtime()
{
	"$VERIFIER" --binary "$RUNTIME/apple-gpu-client" \
		--apple-gpu-prefix "$PREFIX" \
		--framework-directory "$RUNTIME/Frameworks"
}
verify_framework_runtime >/dev/null

xcrun --sdk macosx clang -arch arm64 \
	"$TEST_ROOT/framework-main-missing-marker.c" \
	-F"$PREFIX/Frameworks" -framework EGL -framework GLESv2 \
	-Wl,-rpath,@executable_path/Frameworks \
	-o "$RUNTIME/apple-gpu-client-missing-marker"
expect_rejection 'host-provider marker' \
	--binary "$RUNTIME/apple-gpu-client-missing-marker" \
	--apple-gpu-prefix "$PREFIX" \
	--framework-directory "$RUNTIME/Frameworks"

printf '\0' >> "$RUNTIME/Frameworks/EGL.framework/EGL"
expect_rejection 'does not match the authenticated prefix' \
	--binary "$RUNTIME/apple-gpu-client" \
	--apple-gpu-prefix "$PREFIX" \
	--framework-directory "$RUNTIME/Frameworks"
cp "$PREFIX/Frameworks/EGL.framework/EGL" \
	"$RUNTIME/Frameworks/EGL.framework/EGL"

mkdir -p -- "$RUNTIME/duplicate"
cp -R "$PREFIX/Frameworks/EGL.framework" "$RUNTIME/duplicate/"
expect_rejection 'exactly one EGL.framework copy' \
	--binary "$RUNTIME/apple-gpu-client" \
	--apple-gpu-prefix "$PREFIX" \
	--framework-directory "$RUNTIME/Frameworks"
rm -rf -- "$RUNTIME/duplicate"

touch "$RUNTIME/Frameworks/GLESv2.framework/unexpected"
expect_rejection 'payload is not exact' \
	--binary "$RUNTIME/apple-gpu-client" \
	--apple-gpu-prefix "$PREFIX" \
	--framework-directory "$RUNTIME/Frameworks"
rm -- "$RUNTIME/Frameworks/GLESv2.framework/unexpected"

cat >"$TEST_ROOT/opengl-main.c" <<'EOF'
typedef void *CGLContextObj;
extern CGLContextObj CGLGetCurrentContext(void);
int main(void) { return CGLGetCurrentContext() != 0; }
EOF
xcrun --sdk macosx clang -arch arm64 "$TEST_ROOT/opengl-main.c" \
	-framework OpenGL -o "$TEST_ROOT/opengl-client"
expect_rejection 'forbidden graphics dependency' \
	--binary "$TEST_ROOT/opengl-client"

printf '%s\n' 'runtime dependency closure fixture passed'
