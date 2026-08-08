#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

MODULE=${1:?KernelToolchain.cmake is required}
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/uml-kernel-toolchain.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM
TEST_ROOT=$(CDPATH= cd -- "$TEST_ROOT" && pwd -P)
REAL_CC=$(command -v cc) || {
	printf '%s\n' 'test_kernel_toolchain.sh: cc is unavailable' >&2
	exit 1
}

fail()
{
	printf '%s\n' "test_kernel_toolchain.sh: $*" >&2
	exit 1
}

make_compiler()
{
	path=$1
	mode=$2
	mkdir -p -- "$(dirname -- "$path")"
	cat >"$path" <<EOF
#!/bin/sh
mode='$mode'
output=''
while [ "\$#" -gt 0 ]; do
	if [ "\$1" = -o ]; then
		shift
		[ "\$#" -gt 0 ] || exit 2
		output=\$1
	fi
	shift
done
case "\$mode" in
compatible)
	[ -n "\$output" ] || exit 2
	printf '%s\n' 'int uml_kernel_toolchain_fixture;' |
		'$REAL_CC' -x c -c -o "\$output" -
	;;
unsupported)
	printf '%s\n' "error: __typeof__ on an expression of type 'int[] __counted_by(count)' is not yet supported" >&2
	exit 1
	;;
no-object)
	exit 0
	;;
*)
	exit 2
	;;
esac
EOF
	chmod +x "$path"
}

make_program()
{
	path=$1
	mkdir -p -- "$(dirname -- "$path")"
	cat >"$path" <<'EOF'
#!/bin/sh
exit 0
EOF
	chmod +x "$path"
}

make_toolchain()
{
	root=$1
	mode=$2
	omit=${3:-none}
	make_compiler "$root/bin/clang" "$mode"
	for tool in ld.lld llvm-ar llvm-nm llvm-objcopy llvm-objdump llvm-readelf llvm-strip; do
		[ "$tool" = "$omit" ] || make_program "$root/bin/$tool"
	done
}

cat >"$TEST_ROOT/select-compiler.cmake" <<'EOF'
cmake_minimum_required(VERSION 3.27)
foreach(required IN ITEMS MODULE OUTPUT WORK_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "fixture argument ${required} is missing")
    endif()
endforeach()
include("${MODULE}")
if(DEFINED REQUESTED AND NOT REQUESTED STREQUAL "")
    uml_select_kernel_clang(
        OUT_COMPILER SELECTED
        WORK_DIR "${WORK_DIR}"
        REQUESTED "${REQUESTED}")
else()
    uml_select_kernel_clang(
        OUT_COMPILER SELECTED
        WORK_DIR "${WORK_DIR}"
        CANDIDATES ${CANDIDATES})
endif()
file(WRITE "${OUTPUT}" "${SELECTED}\n")
EOF

make_compiler "$TEST_ROOT/apple-clang" unsupported
make_compiler "$TEST_ROOT/no-object-clang" no-object
make_compiler "$TEST_ROOT/compatible-clang" compatible

cmake -DMODULE="$MODULE" \
	-DOUTPUT="$TEST_ROOT/fallback.out" \
	-DWORK_DIR="$TEST_ROOT/fallback-probe" \
	-DCANDIDATES="$TEST_ROOT/apple-clang;$TEST_ROOT/no-object-clang;$TEST_ROOT/compatible-clang" \
	-P "$TEST_ROOT/select-compiler.cmake"
[ "$(cat "$TEST_ROOT/fallback.out")" = "$TEST_ROOT/compatible-clang" ] ||
	fail "automatic selection did not skip unsupported and non-producing compilers"

cmake -DMODULE="$MODULE" \
	-DOUTPUT="$TEST_ROOT/explicit.out" \
	-DWORK_DIR="$TEST_ROOT/explicit-probe" \
	-DREQUESTED="$TEST_ROOT/compatible-clang" \
	-P "$TEST_ROOT/select-compiler.cmake"
[ "$(cat "$TEST_ROOT/explicit.out")" = "$TEST_ROOT/compatible-clang" ] ||
	fail "an explicit compatible compiler was not retained"

if cmake -DMODULE="$MODULE" \
	-DOUTPUT="$TEST_ROOT/rejected.out" \
	-DWORK_DIR="$TEST_ROOT/rejected-probe" \
	-DREQUESTED="$TEST_ROOT/apple-clang" \
	-P "$TEST_ROOT/select-compiler.cmake" \
	>"$TEST_ROOT/rejected.log" 2>&1; then
	fail "an explicit unsupported compiler was accepted"
fi
grep -F 'Requested kernel compiler does not support Linux 6.18' \
	"$TEST_ROOT/rejected.log" >/dev/null ||
	fail "unsupported compiler rejection lacked the expected diagnostic"

if cmake -DMODULE="$MODULE" \
	-DOUTPUT="$TEST_ROOT/missing-compiler.out" \
	-DWORK_DIR="$TEST_ROOT/missing-compiler-probe" \
	-DREQUESTED="$TEST_ROOT/does-not-exist/clang" \
	-P "$TEST_ROOT/select-compiler.cmake" \
	>"$TEST_ROOT/missing-compiler.log" 2>&1; then
	fail "a missing explicit compiler was accepted"
fi
grep -F 'Requested kernel compiler is unavailable or not executable' \
	"$TEST_ROOT/missing-compiler.log" >/dev/null ||
	fail "missing explicit compiler lacked the expected diagnostic"

if cmake -DMODULE="$MODULE" \
	-DOUTPUT="$TEST_ROOT/no-object.out" \
	-DWORK_DIR="$TEST_ROOT/no-object-probe" \
	-DREQUESTED="$TEST_ROOT/no-object-clang" \
	-P "$TEST_ROOT/select-compiler.cmake" \
	>"$TEST_ROOT/no-object.log" 2>&1; then
	fail "a compiler that produced no object was accepted"
fi
grep -F 'reported success without producing' "$TEST_ROOT/no-object.log" >/dev/null ||
	fail "missing compiler output lacked the expected diagnostic"

cat >"$TEST_ROOT/select-toolchain.cmake" <<'EOF'
cmake_minimum_required(VERSION 3.27)
foreach(required IN ITEMS MODULE OUTPUT WORK_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "fixture argument ${required} is missing")
    endif()
endforeach()
include("${MODULE}")
uml_select_kernel_toolchain(
    WORK_DIR "${WORK_DIR}"
    TOOLCHAIN_ROOT "${TOOLCHAIN_ROOT}"
    CONFIGURED_CC "${CONFIGURED_CC}"
    COMPILER_CANDIDATES ${COMPILER_CANDIDATES}
    REQUESTED_CC "${REQUESTED_CC}"
    REQUESTED_HOST_CC "${REQUESTED_HOST_CC}"
    REQUESTED_LD "${REQUESTED_LD}"
    REQUESTED_AR "${REQUESTED_AR}"
    REQUESTED_NM "${REQUESTED_NM}"
    REQUESTED_OBJCOPY "${REQUESTED_OBJCOPY}"
    REQUESTED_OBJDUMP "${REQUESTED_OBJDUMP}"
    REQUESTED_READELF "${REQUESTED_READELF}"
    REQUESTED_STRIP "${REQUESTED_STRIP}"
    OUT_CC SELECTED_CC
    OUT_HOST_CC SELECTED_HOST_CC
    OUT_LD SELECTED_LD
    OUT_AR SELECTED_AR
    OUT_NM SELECTED_NM
    OUT_OBJCOPY SELECTED_OBJCOPY
    OUT_OBJDUMP SELECTED_OBJDUMP
    OUT_READELF SELECTED_READELF
    OUT_STRIP SELECTED_STRIP)
file(WRITE "${OUTPUT}"
    "CC=${SELECTED_CC}\n"
    "HOST_CC=${SELECTED_HOST_CC}\n"
    "LD=${SELECTED_LD}\n"
    "AR=${SELECTED_AR}\n"
    "NM=${SELECTED_NM}\n"
    "OBJCOPY=${SELECTED_OBJCOPY}\n"
    "OBJDUMP=${SELECTED_OBJDUMP}\n"
    "READELF=${SELECTED_READELF}\n"
    "STRIP=${SELECTED_STRIP}\n")
EOF

TOOLCHAIN_A="$TEST_ROOT/toolchain-a"
TOOLCHAIN_B="$TEST_ROOT/toolchain-b"
make_toolchain "$TOOLCHAIN_A" compatible
make_toolchain "$TOOLCHAIN_B" compatible
make_compiler "$TEST_ROOT/host-only-clang" unsupported

# An explicit compiler is stronger than a root, and an explicit host compiler
# and tool are independently authoritative. All remaining tools must follow
# the selected compiler's sibling directory.
make_program "$TEST_ROOT/override/llvm-ar"
cmake -DMODULE="$MODULE" \
	-DOUTPUT="$TEST_ROOT/override.out" \
	-DWORK_DIR="$TEST_ROOT/override-probe" \
	-DTOOLCHAIN_ROOT="$TOOLCHAIN_A" \
	-DREQUESTED_CC="$TOOLCHAIN_B/bin/clang" \
	-DREQUESTED_HOST_CC="$TEST_ROOT/host-only-clang" \
	-DREQUESTED_AR="$TEST_ROOT/override/llvm-ar" \
	-P "$TEST_ROOT/select-toolchain.cmake"
grep -Fx "CC=$TOOLCHAIN_B/bin/clang" "$TEST_ROOT/override.out" >/dev/null ||
	fail "explicit compiler did not take precedence over the toolchain root"
grep -Fx "HOST_CC=$TEST_ROOT/host-only-clang" "$TEST_ROOT/override.out" >/dev/null ||
	fail "executable host compiler was subjected to target-only capability checks"
grep -Fx "AR=$TEST_ROOT/override/llvm-ar" "$TEST_ROOT/override.out" >/dev/null ||
	fail "explicit tool override was not retained"
for tool in LD NM OBJCOPY OBJDUMP READELF STRIP; do
	name=$(printf '%s' "$tool" | tr '[:upper:]' '[:lower:]')
	[ "$tool" = LD ] && name=ld.lld || name=llvm-$name
	grep -Fx "$tool=$TOOLCHAIN_B/bin/$name" "$TEST_ROOT/override.out" >/dev/null ||
		fail "$tool was not selected beside the requested compiler"
done

# An explicit root is stronger than the compiler CMake happened to use for the
# host project. Without either override, the configured compiler is first.
cmake -DMODULE="$MODULE" \
	-DOUTPUT="$TEST_ROOT/root.out" \
	-DWORK_DIR="$TEST_ROOT/root-probe" \
	-DTOOLCHAIN_ROOT="$TOOLCHAIN_A" \
	-DCONFIGURED_CC="$TOOLCHAIN_B/bin/clang" \
	-P "$TEST_ROOT/select-toolchain.cmake"
grep -Fx "CC=$TOOLCHAIN_A/bin/clang" "$TEST_ROOT/root.out" >/dev/null ||
	fail "toolchain root did not take precedence over the configured compiler"

cmake -DMODULE="$MODULE" \
	-DOUTPUT="$TEST_ROOT/configured.out" \
	-DWORK_DIR="$TEST_ROOT/configured-probe" \
	-DCONFIGURED_CC="$TOOLCHAIN_A/bin/clang" \
	-DCOMPILER_CANDIDATES="$TOOLCHAIN_B/bin/clang" \
	-P "$TEST_ROOT/select-toolchain.cmake"
grep -Fx "CC=$TOOLCHAIN_A/bin/clang" "$TEST_ROOT/configured.out" >/dev/null ||
	fail "compatible configured compiler was not tried first"
for tool in LD AR NM OBJCOPY OBJDUMP READELF STRIP; do
	name=$(printf '%s' "$tool" | tr '[:upper:]' '[:lower:]')
	[ "$tool" = LD ] && name=ld.lld || name=llvm-$name
	grep -Fx "$tool=$TOOLCHAIN_A/bin/$name" "$TEST_ROOT/configured.out" >/dev/null ||
		fail "$tool was mixed with a non-sibling tool"
done

INCOMPLETE="$TEST_ROOT/incomplete-toolchain"
make_toolchain "$INCOMPLETE" compatible llvm-strip
if cmake -DMODULE="$MODULE" \
	-DOUTPUT="$TEST_ROOT/incomplete.out" \
	-DWORK_DIR="$TEST_ROOT/incomplete-probe" \
	-DTOOLCHAIN_ROOT="$INCOMPLETE" \
	-P "$TEST_ROOT/select-toolchain.cmake" \
	>"$TEST_ROOT/incomplete.log" 2>&1; then
	fail "a toolchain with a missing companion tool was accepted"
fi
grep -F 'No kernel llvm-strip tool was found' "$TEST_ROOT/incomplete.log" >/dev/null ||
	fail "missing companion tool lacked the expected diagnostic"

if cmake -DMODULE="$MODULE" \
	-DOUTPUT="$TEST_ROOT/missing-override.out" \
	-DWORK_DIR="$TEST_ROOT/missing-override-probe" \
	-DTOOLCHAIN_ROOT="$TOOLCHAIN_A" \
	-DREQUESTED_NM="$TEST_ROOT/does-not-exist/llvm-nm" \
	-P "$TEST_ROOT/select-toolchain.cmake" \
	>"$TEST_ROOT/missing-override.log" 2>&1; then
	fail "a missing explicit tool override was accepted"
fi
grep -F 'Requested kernel llvm-nm tool is unavailable' \
	"$TEST_ROOT/missing-override.log" >/dev/null ||
	fail "missing explicit tool lacked the expected diagnostic"

# Reconfiguration must recompute the capability-checked internal result. A
# cached path from a fresh configure cannot remain selected after that compiler
# becomes incompatible, and it can become selected again when repaired.
REUSE_SOURCE="$TEST_ROOT/reuse-source"
REUSE_BUILD="$TEST_ROOT/reuse-build"
mkdir -p "$REUSE_SOURCE"
cat >"$REUSE_SOURCE/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.27)
project(kernel_toolchain_cache_fixture NONE)
include("${MODULE}")
uml_select_kernel_clang(
    OUT_COMPILER CURRENT_SELECTION
    WORK_DIR "${CMAKE_BINARY_DIR}/probes"
    CANDIDATES "${CONFIGURED_CC}" "${FALLBACK_CC}")
set(CACHED_SELECTION "${CURRENT_SELECTION}" CACHE INTERNAL "" FORCE)
file(WRITE "${CMAKE_BINARY_DIR}/selected.txt" "${CACHED_SELECTION}\n")
EOF

REUSED_CC="$TEST_ROOT/reused-clang"
make_compiler "$REUSED_CC" compatible
cmake -S "$REUSE_SOURCE" -B "$REUSE_BUILD" \
	-DMODULE="$MODULE" \
	-DCONFIGURED_CC="$REUSED_CC" \
	-DFALLBACK_CC="$TEST_ROOT/compatible-clang" >/dev/null
[ "$(cat "$REUSE_BUILD/selected.txt")" = "$REUSED_CC" ] ||
	fail "fresh configure did not select its first compatible compiler"

make_compiler "$REUSED_CC" unsupported
cmake -S "$REUSE_SOURCE" -B "$REUSE_BUILD" >/dev/null
[ "$(cat "$REUSE_BUILD/selected.txt")" = "$TEST_ROOT/compatible-clang" ] ||
	fail "reused cache kept an incompatible former compiler"

make_compiler "$REUSED_CC" compatible
cmake -S "$REUSE_SOURCE" -B "$REUSE_BUILD" >/dev/null
[ "$(cat "$REUSE_BUILD/selected.txt")" = "$REUSED_CC" ] ||
	fail "reused cache did not restore the repaired configured compiler"

printf '%s\n' "kernel toolchain selection tests passed"
