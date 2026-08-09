#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Build one replayed Darwin UML kernel without relying on a developer-specific
# LLVM, LLD, or cross-tool location.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
# shellcheck disable=SC1091
. "$ROOT_DIR/kernel/toolchain/darwin.env"
CONFIG_DIR="$ROOT_DIR/kernel/config"

fail()
{
	printf '%s\n' "kernel/build-kernel.sh: $*" >&2
	exit 1
}

usage()
{
	cat <<'EOF'
Usage: kernel/build-kernel.sh --source PATH --build PATH --page-size 16k|4k
                              [--jobs N]

The compiler, linker, make program, cross prefix, and tool overrides are
configured through UML_KERNEL_{MAKE,CC,HOST_CC,LD,CROSS_COMPILE,AR,NM,OBJCOPY,
OBJDUMP,READELF,STRIP,LLVM,JOBS,EXTRA_HOST_CFLAGS,EXTRA_HOST_CXXFLAGS}. One
kernel policy ships: kernel/config/common.fragment is the whole configuration
and only the page-size fragment varies. The output directory must be outside
the Linux source tree. Darwin host-tool compatibility is injected from
kernel/toolchain/darwin-host.
EOF
}

SOURCE_DIR=''
BUILD_DIR=''
PAGE_SIZE=''
while [ "$#" -gt 0 ]; do
	case "$1" in
	--source)
		[ "$#" -ge 2 ] || fail "--source requires a path"
		SOURCE_DIR=$2
		shift 2
		;;
	--build)
		[ "$#" -ge 2 ] || fail "--build requires a path"
		BUILD_DIR=$2
		shift 2
		;;
	--page-size)
		[ "$#" -ge 2 ] || fail "--page-size requires 16k or 4k"
		PAGE_SIZE=$2
		shift 2
		;;
	--jobs)
		[ "$#" -ge 2 ] || fail "--jobs requires a positive integer"
		UML_KERNEL_JOBS=$2
		shift 2
		;;
	-h|--help)
		usage
		exit 0
		;;
	*)
		fail "unknown option: $1"
		;;
	esac
done

[ -n "$SOURCE_DIR" ] && [ -n "$BUILD_DIR" ] && [ -n "$PAGE_SIZE" ] || {
	usage >&2
	exit 2
}
case "$PAGE_SIZE" in
16k|4k) ;;
*) fail "--page-size must be 16k or 4k" ;;
esac
case "$UML_KERNEL_JOBS" in
*[!0-9]*|'') fail "UML_KERNEL_JOBS must be a positive integer" ;;
esac
[ "$UML_KERNEL_JOBS" -gt 0 ] || fail "UML_KERNEL_JOBS must be a positive integer"

[ -f "$SOURCE_DIR/Makefile" ] || fail "not a Linux source tree: $SOURCE_DIR"
"$ROOT_DIR/kernel/abi/sync.sh" --kernel-source "$SOURCE_DIR" --verify ||
	fail "kernel ABI copies are stale; replay the tracked series or run kernel/abi/sync.sh"
mkdir -p -- "$BUILD_DIR"
SOURCE_DIR=$(CDPATH= cd -- "$SOURCE_DIR" && pwd -P)
BUILD_DIR=$(CDPATH= cd -- "$BUILD_DIR" && pwd -P)
[ "$SOURCE_DIR" != "$BUILD_DIR" ] ||
	fail "kernel output must be outside the source tree"

command -v "$UML_KERNEL_MAKE" >/dev/null 2>&1 ||
	fail "make program is unavailable: $UML_KERNEL_MAKE"
command -v "$UML_KERNEL_CC" >/dev/null 2>&1 ||
	fail "C compiler is unavailable: $UML_KERNEL_CC"
command -v "$UML_KERNEL_HOST_CC" >/dev/null 2>&1 ||
	fail "host C compiler is unavailable: $UML_KERNEL_HOST_CC"
command -v "$UML_KERNEL_LD" >/dev/null 2>&1 ||
	fail "linker is unavailable: $UML_KERNEL_LD"

# Kbuild's standard deterministic-archive flag is not supported by Apple's BSD
# ar. Selecting a compatible archiver here does not make output hashes a
# cross-run acceptance identity.
# When a caller selects an LLVM clang installed with its sibling LLVM tools,
# use that coherent tool family unless an explicit override was supplied.  This
# remains path-agnostic (Homebrew, Xcode toolchains, and custom LLVM installs
# all work) and leaves non-LLVM callers untouched.
compiler_path=$(command -v "$UML_KERNEL_CC")
compiler_dir=$(dirname -- "$compiler_path")
if [ -x "$compiler_dir/llvm-ar" ]; then
	: "${UML_KERNEL_AR:=$compiler_dir/llvm-ar}"
	: "${UML_KERNEL_NM:=$compiler_dir/llvm-nm}"
	: "${UML_KERNEL_OBJCOPY:=$compiler_dir/llvm-objcopy}"
	: "${UML_KERNEL_OBJDUMP:=$compiler_dir/llvm-objdump}"
	: "${UML_KERNEL_READELF:=$compiler_dir/llvm-readelf}"
	: "${UML_KERNEL_STRIP:=$compiler_dir/llvm-strip}"
fi

# Linux 6.18 uses __counted_by flexible arrays through type expressions. Some
# Apple toolchain releases accept the attribute but cannot compile those
# expressions. Diagnose that host-toolchain mismatch before a long Kbuild.
if ! printf '%s\n' \
	'#define __counted_by(member) __attribute__((__counted_by__(member)))' \
	'struct uml_counted_by_probe { int count; int values[] __counted_by(count); };' \
	'int uml_counted_by_probe(struct uml_counted_by_probe *probe) {' \
	'  return __builtin_types_compatible_p(__typeof__(probe->values), __typeof__(&(probe->values)[0]));' \
	'}' | "$UML_KERNEL_CC" -x c -Werror -c -o /dev/null - >/dev/null 2>&1; then
	fail "C compiler lacks Linux 6.18 __counted_by type-expression support: $UML_KERNEL_CC (install a newer LLVM or set UML_KERNEL_CC/UML_KERNEL_HOST_CC)"
fi

run_make()
{
	host_compat="$ROOT_DIR/kernel/toolchain/darwin-host/host_compat.h"
	host_include="$ROOT_DIR/kernel/toolchain/darwin-host/include"
	host_tool_bin="$ROOT_DIR/kernel/toolchain/darwin-host/bin"
	host_uapi_flags="-I$host_include -idirafter $BUILD_DIR/arch/arm64/include/generated/uapi -idirafter $SOURCE_DIR/arch/arm64/include/uapi -idirafter $SOURCE_DIR/include/uapi -idirafter $BUILD_DIR/include/generated/uapi"
	host_cflags="-include $host_compat $host_uapi_flags $UML_KERNEL_EXTRA_HOST_CFLAGS"
	host_cxxflags="$host_uapi_flags $UML_KERNEL_EXTRA_HOST_CXXFLAGS"
	set -- "HOSTCFLAGS=$host_cflags" "HOSTCXXFLAGS=$host_cxxflags" "$@"
	if [ -n "$UML_KERNEL_AR" ]; then
		set -- "AR=$UML_KERNEL_AR" "$@"
	fi
	if [ -n "$UML_KERNEL_NM" ]; then
		set -- "NM=$UML_KERNEL_NM" "$@"
	fi
	if [ -n "$UML_KERNEL_OBJCOPY" ]; then
		set -- "OBJCOPY=$UML_KERNEL_OBJCOPY" "$@"
	fi
	if [ -n "$UML_KERNEL_OBJDUMP" ]; then
		set -- "OBJDUMP=$UML_KERNEL_OBJDUMP" "$@"
	fi
	if [ -n "$UML_KERNEL_READELF" ]; then
		set -- "READELF=$UML_KERNEL_READELF" "$@"
	fi
	if [ -n "$UML_KERNEL_STRIP" ]; then
		set -- "STRIP=$UML_KERNEL_STRIP" "$@"
	fi
	if [ -n "$UML_KERNEL_LLVM" ]; then
		PATH="$host_tool_bin:$PATH" "$UML_KERNEL_MAKE" \
			-C "$SOURCE_DIR" "O=$BUILD_DIR" \
			ARCH=um SUBARCH=arm64 UML_HOST_OS=Darwin \
			"CC=$UML_KERNEL_CC" "HOSTCC=$UML_KERNEL_HOST_CC" \
			"LD=$UML_KERNEL_LD" "CROSS_COMPILE=$UML_KERNEL_CROSS_COMPILE" \
			"LLVM=$UML_KERNEL_LLVM" "$@"
	else
		PATH="$host_tool_bin:$PATH" "$UML_KERNEL_MAKE" \
			-C "$SOURCE_DIR" "O=$BUILD_DIR" \
			ARCH=um SUBARCH=arm64 UML_HOST_OS=Darwin \
			"CC=$UML_KERNEL_CC" "HOSTCC=$UML_KERNEL_HOST_CC" \
			"LD=$UML_KERNEL_LD" "CROSS_COMPILE=$UML_KERNEL_CROSS_COMPILE" "$@"
	fi
}

apply_config_fragment()
{
	fragment=$1
	while IFS= read -r line; do
		case "$line" in
		CONFIG_*=y)
			symbol=${line%%=*}
			"$SOURCE_DIR/scripts/config" --file "$BUILD_DIR/.config" \
				-e "${symbol#CONFIG_}"
			;;
		'# CONFIG_'*' is not set')
			symbol=${line#\# CONFIG_}
			symbol=${symbol% is not set}
			"$SOURCE_DIR/scripts/config" --file "$BUILD_DIR/.config" \
				-d "$symbol"
			;;
		\#*|'') ;;
		*) fail "unsupported kernel config fragment line: $line" ;;
		esac
	done < "$fragment"
}

case "$PAGE_SIZE" in
16k)
	CONFIG_FRAGMENT="$CONFIG_DIR/page-16k.fragment"
		;;
4k)
	CONFIG_FRAGMENT="$CONFIG_DIR/page-4k.fragment"
		;;
esac
[ -f "$CONFIG_FRAGMENT" ] || fail "missing page-size config fragment: $CONFIG_FRAGMENT"
[ -f "$CONFIG_DIR/common.fragment" ] || fail "missing common config fragment"
cp "$CONFIG_DIR/common.fragment" "$BUILD_DIR/.config"
apply_config_fragment "$CONFIG_FRAGMENT"
run_make olddefconfig
run_make -j"$UML_KERNEL_JOBS"
[ -f "$BUILD_DIR/vmlinux" ] ||
	fail "Kbuild completed without producing $BUILD_DIR/vmlinux"
if [ -n "$UML_KERNEL_NM" ]; then
	"$ROOT_DIR/kernel/abi/verify-linux-imports.sh" \
		--kernel-binary "$BUILD_DIR/vmlinux" --nm "$UML_KERNEL_NM" \
		--page-size "$PAGE_SIZE"
else
	"$ROOT_DIR/kernel/abi/verify-linux-imports.sh" \
		--kernel-binary "$BUILD_DIR/vmlinux" --page-size "$PAGE_SIZE"
fi
printf '%s\n' "built $PAGE_SIZE Darwin UML kernel: $BUILD_DIR/vmlinux"
