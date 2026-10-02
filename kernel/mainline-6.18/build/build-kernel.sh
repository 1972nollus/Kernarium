#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Build one replayed Darwin UML kernel without relying on a developer-specific
# LLVM, LLD, or cross-tool location.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
LINE_ROOT="$ROOT_DIR/kernel/mainline-6.18"
COMMON_ROOT="$ROOT_DIR/kernel/common"
# shellcheck disable=SC1091
. "$LINE_ROOT/build/toolchain/darwin.env"
CONFIG_DIR="$LINE_ROOT/config"
SOURCE_LOCATION="$LINE_ROOT/scripts/source-location.sh"
SLICE_SELECTOR="$LINE_ROOT/scripts/select-slice.sh"

fail()
{
	printf '%s\n' "kernel/mainline-6.18/build/build-kernel.sh: $*" >&2
	exit 1
}

usage()
{
	cat <<'EOF'
Usage: kernel/mainline-6.18/build/build-kernel.sh \
       --build PATH --page-size 16k|4k [--slice arm64|x86_64] [--jobs N]
       [--lockdep]

The compiler, linker, make program, cross prefix, and tool overrides are
configured through UML_KERNEL_{MAKE,CC,HOST_CC,LD,CROSS_COMPILE,AR,NM,OBJCOPY,
OBJDUMP,READELF,STRIP,LLVM,JOBS,EXTRA_HOST_CFLAGS,EXTRA_HOST_CXXFLAGS}. pahole
and the static libelf used for BTF come from UML_KERNEL_HOST_TOOLS_PREFIX
(default .build/dependencies/kernel-host-tools, built by
dependencies/build-kernel-host-tools.sh); UML_KERNEL_PAHOLE may select another
pahole v1.29 or newer.
The arm64 slice supports 16 KiB and 4 KiB pages. The x86_64 userspace slice is
4 KiB only and composes its own config fragment after the shared common and
page fragments. The output directory must be outside
the Linux source tree. Darwin host-tool compatibility is injected from
kernel/mainline-6.18/build/toolchain/darwin-host. macOS, iOS Simulator, and physical iOS all use
the same constrained native address-space profile. An omitted jobs value uses
all online host processors. The source is always read from the required,
Git-ignored kernel/mainline-6.18/source-location.conf. --lockdep also applies
config/lockdep.fragment for a lock-checking probe kernel, which is never an
accepted artifact.
EOF
}

BUILD_DIR=''
PAGE_SIZE=''
SLICE=arm64
LOCKDEP=0
while [ "$#" -gt 0 ]; do
	case "$1" in
	--slice)
		[ "$#" -ge 2 ] || fail "--slice requires arm64 or x86_64"
		SLICE=$2
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
	--lockdep)
		LOCKDEP=1
		shift
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

[ -r "$SLICE_SELECTOR" ] || fail "missing slice selector: $SLICE_SELECTOR"
# shellcheck disable=SC1090
. "$SLICE_SELECTOR"
uml_kernel_select_slice "$LINE_ROOT" "$SLICE" ||
	fail "cannot select kernel slice: $SLICE"

[ -n "$BUILD_DIR" ] && [ -n "$PAGE_SIZE" ] || {
	usage >&2
	exit 2
}
[ -x "$SOURCE_LOCATION" ] || fail "missing source-location verifier: $SOURCE_LOCATION"
SOURCE_DIR=$("$SOURCE_LOCATION" --require-source) ||
	fail "kernel build requires a valid kernel/mainline-6.18/source-location.conf"
case "$PAGE_SIZE" in
16k|4k) ;;
*) fail "--page-size must be 16k or 4k" ;;
esac
[ "$UML_KERNEL_SLICE" != x86_64 ] || [ "$PAGE_SIZE" = 4k ] ||
	fail "the x86_64 userspace slice supports only the 4k page profile"
case "$UML_KERNEL_JOBS" in
*[!0-9]*|'') fail "UML_KERNEL_JOBS must be a positive integer" ;;
esac
[ "$UML_KERNEL_JOBS" -gt 0 ] || fail "UML_KERNEL_JOBS must be a positive integer"

[ -f "$SOURCE_DIR/Makefile" ] || fail "not a Linux source tree: $SOURCE_DIR"
[ -z "$(git -C "$SOURCE_DIR" status --porcelain --untracked-files=all)" ] ||
	fail "kernel source working tree is dirty"
source_tree=$(git -C "$SOURCE_DIR" rev-parse HEAD^{tree} 2>/dev/null || true)
[ "$source_tree" = "$UML_KERNEL_FINAL_TREE" ] ||
	fail "$UML_KERNEL_SLICE source tree is $source_tree; expected $UML_KERNEL_FINAL_TREE"
"$COMMON_ROOT/abi/sync.sh" --kernel-source "$SOURCE_DIR" --verify ||
	fail "kernel ABI copies are stale; replay the tracked series or run kernel/common/abi/sync.sh"
python3 -B "$COMMON_ROOT/video/sync-protocol.py" --kernel-source "$SOURCE_DIR" --verify ||
	fail "kernel Virtio video protocol copy is stale; replay the tracked series or run kernel/common/video/sync-protocol.py"
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

# Every line generates BTF. pahole encodes it at each vmlinux link and
# resolve_btfids links the static libelf; both come from the pinned
# kernel-host-tools prefix, never from PATH. Kconfig derives the BTF options
# from the pahole version, and 1.29 is the first release whose parallel
# encoding is always ordered.
pahole_help="build it with dependencies/build-kernel-host-tools.sh or set UML_KERNEL_PAHOLE"
[ -x "$UML_KERNEL_PAHOLE" ] ||
	fail "pahole is unavailable: $UML_KERNEL_PAHOLE ($pahole_help)"
pahole_version=$("$UML_KERNEL_PAHOLE" --version 2>/dev/null) ||
	fail "pahole does not run: $UML_KERNEL_PAHOLE ($pahole_help)"
case "$pahole_version" in
v[0-9]*.[0-9]*)
	pahole_major=${pahole_version#v}
	pahole_major=${pahole_major%%.*}
	pahole_minor=${pahole_version#v*.}
	pahole_minor=${pahole_minor%%[!0-9]*}
	;;
*) fail "unrecognized pahole version '$pahole_version': $UML_KERNEL_PAHOLE" ;;
esac
case "$pahole_major$pahole_minor" in
''|*[!0-9]*) fail "unrecognized pahole version '$pahole_version': $UML_KERNEL_PAHOLE" ;;
esac
[ "$pahole_major" -gt 1 ] || { [ "$pahole_major" -eq 1 ] && [ "$pahole_minor" -ge 29 ]; } ||
	fail "pahole $pahole_version is older than v1.29: $UML_KERNEL_PAHOLE ($pahole_help)"
for host_tool_file in include/libelf.h include/gelf.h include/elf.h lib/libelf.a; do
	[ -f "$UML_KERNEL_HOST_TOOLS_PREFIX/$host_tool_file" ] ||
		fail "kernel host tools lack $host_tool_file: $UML_KERNEL_HOST_TOOLS_PREFIX ($pahole_help)"
done

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
	host_compat="$LINE_ROOT/build/toolchain/darwin-host/host_compat.h"
	host_include="$LINE_ROOT/build/toolchain/darwin-host/include"
	host_tool_bin="$LINE_ROOT/build/toolchain/darwin-host/bin"
	host_tools_include="$UML_KERNEL_HOST_TOOLS_PREFIX/include"
	host_uapi_flags="-I$host_include -I$host_tools_include -idirafter $BUILD_DIR/arch/arm64/include/generated/uapi -idirafter $SOURCE_DIR/arch/arm64/include/uapi -idirafter $SOURCE_DIR/include/uapi -idirafter $BUILD_DIR/include/generated/uapi"
	host_cflags="-include $host_compat $host_uapi_flags $UML_KERNEL_EXTRA_HOST_CFLAGS"
	host_cxxflags="$host_uapi_flags $UML_KERNEL_EXTRA_HOST_CXXFLAGS"
	# resolve_btfids asks pkg-config for libelf; only the pinned prefix may
	# answer, and the library path serves a host without pkg-config.
	set -- "HOSTCFLAGS=$host_cflags" "HOSTCXXFLAGS=$host_cxxflags" \
		"HOSTLDFLAGS=-L$UML_KERNEL_HOST_TOOLS_PREFIX/lib" \
		"PAHOLE=$UML_KERNEL_PAHOLE" "$@"
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
		set -- "LLVM=$UML_KERNEL_LLVM" "$@"
	fi
	PATH="$host_tool_bin:$PATH" \
		PKG_CONFIG_LIBDIR="$UML_KERNEL_HOST_TOOLS_PREFIX/lib/pkgconfig" \
		PKG_CONFIG_PATH= "$UML_KERNEL_MAKE" \
		-C "$SOURCE_DIR" "O=$BUILD_DIR" \
		ARCH=um SUBARCH=arm64 UML_HOST_OS=Darwin \
		"CC=$UML_KERNEL_CC" "HOSTCC=$UML_KERNEL_HOST_CC" \
		"LD=$UML_KERNEL_LD" "CROSS_COMPILE=$UML_KERNEL_CROSS_COMPILE" "$@"
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
		CONFIG_*=*)
			symbol=${line%%=*}
			value=${line#*=}
			case "$value" in
			''|*[!0-9]*)
				fail "unsupported numeric kernel config value: $line"
				;;
			esac
			"$SOURCE_DIR/scripts/config" --file "$BUILD_DIR/.config" \
				--set-val "${symbol#CONFIG_}" "$value"
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

# `olddefconfig` resolves a requested symbol against its dependencies in
# silence.  A symbol whose menu is closed is simply dropped, and one that
# another symbol selects is force-enabled, so the composed fragments can
# disagree with every kernel they produce without anything saying so.
# CONFIG_NAMESPACES defaulting off under EXPERT cost USER_NS and TIME_NS that
# way, and the guest lost every namespace while the fragment still claimed two
# of them.  Compare what was asked for against what the resolved configuration
# actually holds.  Later fragments intentionally override earlier policy for a
# product slice.
verify_composed_config()
{
	mismatches=$(awk -v config="$BUILD_DIR/.config" '
	function record(symbol, wanted, got) {
		problems = problems sprintf("  %s: %s requested %s, resolved %s\n",
			symbol, owner[symbol], wanted, got)
		count++
	}
	BEGIN {
		while ((getline line < config) > 0) {
			if (match(line, /^CONFIG_[A-Za-z0-9_]+=/)) {
				split(line, parts, "=")
				built[parts[1]] = substr(line, index(line, "=") + 1)
			} else if (match(line,
				/^# CONFIG_[A-Za-z0-9_]+ is not set$/)) {
				split(line, parts, " ")
				built[parts[2]] = "n"
			}
		}
		close(config)
	}
	/^#/ && !/^# CONFIG_[A-Za-z0-9_]+ is not set$/ { next }
	/^$/ { next }
	{
		if (match($0, /^CONFIG_[A-Za-z0-9_]+=/)) {
			symbol = substr($0, 1, index($0, "=") - 1)
			requested[symbol] = substr($0, index($0, "=") + 1)
		} else if (match($0, /^# CONFIG_[A-Za-z0-9_]+ is not set$/)) {
			symbol = $2
			requested[symbol] = "n"
		} else {
			next
		}
		owner[symbol] = FILENAME
	}
	END {
		for (symbol in requested) {
			wanted = requested[symbol]
			# An absent symbol is one whose dependencies were never met.
			got = (symbol in built) ? built[symbol] : "n"
			if (got == wanted)
				continue
			# A .config records a disabled symbol as a comment or omits it,
			# so both spellings of "off" agree.
			if (wanted == "n" && !(symbol in built))
				continue
			record(symbol, wanted, (symbol in built) ? got : "absent")
		}
		if (count > 0)
			printf "%s", problems
	}
	' "$@")
	[ -z "$mismatches" ] || {
		printf '%s\n' \
			"kernel/mainline-6.18/build/build-kernel.sh: composed configuration did not survive olddefconfig:" >&2
		printf '%s\n' "$mismatches" >&2
		fail "kernel configuration does not match its fragment"
	}
}

case "$PAGE_SIZE" in
16k)
	CONFIG_FRAGMENT="$COMMON_ROOT/config/page-16k.fragment"
		;;
4k)
	CONFIG_FRAGMENT="$CONFIG_DIR/page-4k.fragment"
		;;
esac
[ -f "$CONFIG_FRAGMENT" ] || fail "missing page-size config fragment: $CONFIG_FRAGMENT"
[ -f "$CONFIG_DIR/common.fragment" ] || fail "missing common config fragment"
cp "$CONFIG_DIR/common.fragment" "$BUILD_DIR/.config"
apply_config_fragment "$CONFIG_FRAGMENT"
if [ -n "$UML_KERNEL_SLICE_CONFIG_FRAGMENT" ]; then
	[ -f "$UML_KERNEL_SLICE_CONFIG_FRAGMENT" ] ||
		fail "missing slice config fragment: $UML_KERNEL_SLICE_CONFIG_FRAGMENT"
	apply_config_fragment "$UML_KERNEL_SLICE_CONFIG_FRAGMENT"
fi
if [ "$LOCKDEP" -eq 1 ]; then
	[ -f "$CONFIG_DIR/lockdep.fragment" ] ||
		fail "missing lockdep config fragment: $CONFIG_DIR/lockdep.fragment"
	apply_config_fragment "$CONFIG_DIR/lockdep.fragment"
fi
run_make olddefconfig
set -- "$CONFIG_DIR/common.fragment" "$CONFIG_FRAGMENT"
if [ -n "$UML_KERNEL_SLICE_CONFIG_FRAGMENT" ]; then
	set -- "$@" "$UML_KERNEL_SLICE_CONFIG_FRAGMENT"
fi
if [ "$LOCKDEP" -eq 1 ]; then
	set -- "$@" "$CONFIG_DIR/lockdep.fragment"
fi
verify_composed_config "$@"
run_make -j"$UML_KERNEL_JOBS"
[ -f "$BUILD_DIR/vmlinux" ] ||
	fail "Kbuild completed without producing $BUILD_DIR/vmlinux"
python3 -B "$COMMON_ROOT/abi/bridge_imports.py" \
	--kernel-binary "$BUILD_DIR/vmlinux" --page-size "$PAGE_SIZE" \
	--slice "$UML_KERNEL_SLICE"
if [ "$(uname -s)" = Darwin ]; then
	python3 "$COMMON_ROOT/tooling/verify-console-input.py" \
		--source "$SOURCE_DIR" --build "$BUILD_DIR"
fi
printf '%s\n' "built $UML_KERNEL_SLICE $PAGE_SIZE Darwin UML kernel: $BUILD_DIR/vmlinux"
