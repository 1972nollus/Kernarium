#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Build the one supported Android 17 ACK kernel: arm64 UML with 16 KiB pages.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
LINE_ROOT="$ROOT_DIR/kernel/android-6.18"
COMMON_ROOT="$ROOT_DIR/kernel/common"
METADATA="$LINE_ROOT/patches/metadata.env"
CONFIG_FRAGMENT="$LINE_ROOT/config/android.fragment"
PAGE_FRAGMENT="$COMMON_ROOT/config/page-16k.fragment"
REQUIRED_BUILTINS="$LINE_ROOT/config/required-builtins.txt"
FORBIDDEN_OPTIONS="$LINE_ROOT/config/forbidden-options.txt"
TARGET_WRAPPER="$SCRIPT_DIR/toolchain/aarch64-linux-clang.sh"
HOST_WRAPPER="$SCRIPT_DIR/toolchain/linux-host-clang.sh"
ELF_VERIFIER="$SCRIPT_DIR/verify-kernel-elf.py"

ACK_WORKSPACE=
KERNEL_SOURCE=
DARWIN_SDK=
BUILD_DIR=
JOBS=

fail()
{
	printf '%s\n' "android/aosp-17/build/build-kernel.sh: $*" >&2
	exit 1
}

usage()
{
	cat <<'EOF'
Usage: android/aosp-17/build/build-kernel.sh \
       --ack-workspace ABSOLUTE_PATH \
       --kernel-source ABSOLUTE_PATH \
       --darwin-sdk ABSOLUTE_PATH \
       --build ABSOLUTE_PATH [--jobs N]

Run locally on the x86_64 Linux builder. The command consumes the already
replayed ACK tree and checked-out ACK prebuilts. It always builds an AArch64
Darwin UML ET_DYN image with 16 KiB pages, CONFIG_MODULES=n, and every selected
facility linked into vmlinux. There is no page-size or module mode selector.
EOF
}

absolute_path()
{
	name=$1
	value=$2
	case "$value" in
	/*) ;;
	*) fail "$name must be an absolute local path" ;;
	esac
	case "$value" in
	/|*/../*|*/..|*/./*|*/.) fail "$name must be normalized" ;;
	esac
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--ack-workspace)
		[ "$#" -ge 2 ] || fail "--ack-workspace requires a path"
		ACK_WORKSPACE=$2
		shift 2
		;;
	--kernel-source)
		[ "$#" -ge 2 ] || fail "--kernel-source requires a path"
		KERNEL_SOURCE=$2
		shift 2
		;;
	--darwin-sdk)
		[ "$#" -ge 2 ] || fail "--darwin-sdk requires a path"
		DARWIN_SDK=$2
		shift 2
		;;
	--build)
		[ "$#" -ge 2 ] || fail "--build requires a path"
		BUILD_DIR=$2
		shift 2
		;;
	--jobs)
		[ "$#" -ge 2 ] || fail "--jobs requires a positive integer"
		JOBS=$2
		shift 2
		;;
	-h|--help)
		usage
		exit 0
		;;
	*) fail "unknown option: $1" ;;
	esac
done

[ -n "$ACK_WORKSPACE" ] || fail "--ack-workspace is required"
[ -n "$KERNEL_SOURCE" ] || fail "--kernel-source is required"
[ -n "$DARWIN_SDK" ] || fail "--darwin-sdk is required"
[ -n "$BUILD_DIR" ] || fail "--build is required"
absolute_path "ACK workspace" "$ACK_WORKSPACE"
absolute_path "kernel source" "$KERNEL_SOURCE"
absolute_path "Darwin SDK" "$DARWIN_SDK"
absolute_path "build directory" "$BUILD_DIR"
[ "$(uname -s)" = Linux ] || fail "builder host must be Linux"
[ "$(uname -m)" = x86_64 ] || fail "builder host must be x86_64"
if [ -z "$JOBS" ]; then
	JOBS=$(getconf _NPROCESSORS_ONLN)
fi
case "$JOBS" in
*[!0-9]*|'') fail "jobs must be a positive integer" ;;
esac
[ "$JOBS" -gt 0 ] || fail "jobs must be a positive integer"

for input in "$METADATA" "$CONFIG_FRAGMENT" "$PAGE_FRAGMENT" \
	"$REQUIRED_BUILTINS" "$FORBIDDEN_OPTIONS"; do
	[ -f "$input" ] || fail "required input is missing: $input"
done
for tool in "$TARGET_WRAPPER" "$HOST_WRAPPER" "$ELF_VERIFIER"; do
	[ -x "$tool" ] || fail "required executable is missing: $tool"
done
# shellcheck disable=SC1090
. "$METADATA"

[ -f "$KERNEL_SOURCE/Makefile" ] ||
	fail "kernel source is not an ACK tree: $KERNEL_SOURCE"
[ -f "$DARWIN_SDK/SDKSettings.json" ] ||
	fail "Darwin SDK is missing SDKSettings.json"
source_tree=$(git -C "$KERNEL_SOURCE" rev-parse HEAD^{tree})
[ "$source_tree" = "$UML_KERNEL_FINAL_TREE" ] ||
	fail "kernel source tree is $source_tree, expected $UML_KERNEL_FINAL_TREE"
[ -z "$(git -C "$KERNEL_SOURCE" status --porcelain)" ] ||
	fail "kernel source checkout is dirty"
git -C "$KERNEL_SOURCE" merge-base --is-ancestor \
	"$UML_KERNEL_UPSTREAM_COMMIT" HEAD || fail "kernel source is not based on pinned ACK"

"$COMMON_ROOT/abi/sync.sh" --kernel-source "$KERNEL_SOURCE" --verify >/dev/null
python3 -B "$COMMON_ROOT/video/sync-protocol.py" --kernel-source "$KERNEL_SOURCE" --verify
mkdir -p -- "$BUILD_DIR"
KERNEL_SOURCE=$(CDPATH= cd -- "$KERNEL_SOURCE" && pwd -P)
BUILD_DIR=$(CDPATH= cd -- "$BUILD_DIR" && pwd -P)
[ "$KERNEL_SOURCE" != "$BUILD_DIR" ] ||
	fail "kernel output must be outside the source tree"

CLANG_ROOT="$ACK_WORKSPACE/prebuilts/clang/host/linux-x86/clang-$UML_KERNEL_CLANG_VERSION"
CLANG_BIN="$CLANG_ROOT/bin"
# Host C tools build against the kernel-build-tools musl sysroot and libraries,
# as Kleaf builds them. Rust proc macros are loaded into the glibc-linked
# rustc, so their link alone selects the glibc host toolchain again.
KERNEL_BUILD_TOOLS="$ACK_WORKSPACE/prebuilts/kernel-build-tools"
MUSL_SYSROOT="$KERNEL_BUILD_TOOLS/sysroots/x86_64-unknown-linux-musl"
MUSL_LIB="$KERNEL_BUILD_TOOLS/linux_musl-x86/lib64"
MUSL_INCLUDE="$KERNEL_BUILD_TOOLS/linux_musl-x86/include"
HOST_TOOLCHAIN_ROOT="$ACK_WORKSPACE/prebuilts/gcc/linux-x86/host/$UML_KERNEL_HOST_GLIBC_TOOLCHAIN"
HOST_SYSROOT="$HOST_TOOLCHAIN_ROOT/sysroot"
HOST_GCC_LIB="$HOST_TOOLCHAIN_ROOT/lib/gcc/x86_64-linux/4.8.3"
HOST_RUNTIME_LIB="$HOST_TOOLCHAIN_ROOT/x86_64-linux/lib64"
RUST_ROOT="$ACK_WORKSPACE/prebuilts/rust-toolchain/linux-x86/$UML_KERNEL_RUST_VERSION"
BUILD_TOOLS="$ACK_WORKSPACE/prebuilts/build-tools"
KERNEL_TOOLS="$KERNEL_BUILD_TOOLS/linux_musl-x86/bin"

MAKE="$BUILD_TOOLS/linux-x86/bin/make"
M4="$BUILD_TOOLS/linux-x86/bin/m4"
BINDGEN="$ACK_WORKSPACE/prebuilts/clang-tools/linux-x86/bin/bindgen"
PAHOLE="$KERNEL_TOOLS/pahole"
RUSTC="$RUST_ROOT/bin/rustc"
RUSTFMT="$RUST_ROOT/bin/rustfmt"
RUST_LIB_SRC="$RUST_ROOT/lib/rustlib/src/rust/library"
for tool in "$MAKE" "$M4" "$BINDGEN" "$PAHOLE" "$RUSTC" "$RUSTFMT" \
	"$CLANG_BIN/clang" "$CLANG_BIN/ld.lld" "$CLANG_BIN/llvm-ar" \
	"$CLANG_BIN/llvm-nm" "$CLANG_BIN/llvm-objcopy" \
	"$CLANG_BIN/llvm-objdump" "$CLANG_BIN/llvm-readelf" \
	"$CLANG_BIN/llvm-strip"; do
	[ -x "$tool" ] || fail "pinned ACK tool is missing: $tool"
done
[ -d "$HOST_GCC_LIB" ] || fail "host GCC runtime is missing: $HOST_GCC_LIB"
[ -d "$HOST_RUNTIME_LIB" ] || fail "host glibc runtime is missing: $HOST_RUNTIME_LIB"
[ -f "$MUSL_SYSROOT/lib/libc.so" ] || fail "host musl sysroot is missing: $MUSL_SYSROOT"
for library in libelf.so libz-host.so; do
	[ -f "$MUSL_LIB/$library" ] || fail "host musl library is missing: $MUSL_LIB/$library"
done
[ -f "$MUSL_INCLUDE/libelf.h" ] || fail "host libelf headers are missing: $MUSL_INCLUDE"
[ -d "$RUST_LIB_SRC" ] || fail "Rust library source is missing: $RUST_LIB_SRC"

export UML_ANDROID17_TARGET_CLANG="$CLANG_BIN/clang"
export UML_ANDROID17_HOST_CLANG="$CLANG_BIN/clang"
export UML_ANDROID17_HOST_SYSROOT="$MUSL_SYSROOT"
export UML_ANDROID17_HOST_INCLUDE="$MUSL_INCLUDE"
export LIBCLANG_PATH="$CLANG_ROOT/lib"
TOOL_PATH="$BUILD_TOOLS/path/linux-x86:$BUILD_TOOLS/linux-x86/bin:$KERNEL_TOOLS:$CLANG_BIN:$RUST_ROOT/bin:$PATH"
# The kernel-build-tools zlib is named libz-host.so; resolve_btfids links -lz.
HOST_LINK_ALIASES="$BUILD_DIR/host-link-aliases"
mkdir -p -- "$HOST_LINK_ALIASES"
ln -sfn -- "$MUSL_LIB/libz-host.so" "$HOST_LINK_ALIASES/libz.so"
HOST_LINK_FLAGS="-L$MUSL_LIB -L$HOST_LINK_ALIASES -Wl,-rpath,$MUSL_LIB"
PROCMACRO_LINK_FLAGS="--target=x86_64-unknown-linux-gnu --sysroot=$HOST_SYSROOT \
-B$HOST_GCC_LIB -rtlib=libgcc -Wl,-rpath-link,$HOST_RUNTIME_LIB \
-L$HOST_RUNTIME_LIB -L$HOST_GCC_LIB"

run_make()
{
	PATH="$TOOL_PATH" M4="$M4" "$MAKE" \
		-C "$KERNEL_SOURCE" "O=$BUILD_DIR" \
		ARCH=um SUBARCH=arm64 UML_HOST_OS=Darwin \
		"DARWIN_SDKROOT=$DARWIN_SDK" \
		"CC=$TARGET_WRAPPER" "HOSTCC=$HOST_WRAPPER" \
		"HOSTLDFLAGS=$HOST_LINK_FLAGS" \
		"PROCMACROLDFLAGS=$PROCMACRO_LINK_FLAGS" \
		"HOSTRUSTFLAGS=--target x86_64-unknown-linux-musl" \
		HOSTPKG_CONFIG=false \
		"LD=$CLANG_BIN/ld.lld" "AR=$CLANG_BIN/llvm-ar" \
		"NM=$CLANG_BIN/llvm-nm" "OBJCOPY=$CLANG_BIN/llvm-objcopy" \
		"OBJDUMP=$CLANG_BIN/llvm-objdump" \
		"READELF=$CLANG_BIN/llvm-readelf" \
		"STRIP=$CLANG_BIN/llvm-strip" \
		"RUSTC=$RUSTC" "HOSTRUSTC=$RUSTC" "RUSTFMT=$RUSTFMT" \
		"BINDGEN=$BINDGEN" "RUST_LIB_SRC=$RUST_LIB_SRC" \
		"PAHOLE=$PAHOLE" "$@"
}

apply_page_fragment()
{
	while IFS= read -r line; do
		case "$line" in
		CONFIG_*=y)
			symbol=${line%%=*}
			"$KERNEL_SOURCE/scripts/config" --file "$BUILD_DIR/.config" \
				-e "${symbol#CONFIG_}"
			;;
		CONFIG_*=*)
			symbol=${line%%=*}
			value=${line#*=}
			case "$value" in
			''|*[!0-9]*) fail "unsupported page-size value: $line" ;;
			esac
			"$KERNEL_SOURCE/scripts/config" --file "$BUILD_DIR/.config" \
				--set-val "${symbol#CONFIG_}" "$value"
			;;
		'# CONFIG_'*' is not set')
			symbol=${line#\# CONFIG_}
			symbol=${symbol% is not set}
			"$KERNEL_SOURCE/scripts/config" --file "$BUILD_DIR/.config" \
				-d "$symbol"
			;;
		\#*|'') ;;
		*) fail "unsupported page-size fragment line: $line" ;;
		esac
	done < "$PAGE_FRAGMENT"
}

verify_config_fragment()
{
	fragment=$1
	mismatches=$(awk -v config="$BUILD_DIR/.config" '
	function record(symbol, wanted, got) {
		problems = problems sprintf("  %s: requested %s, resolved %s\n",
			symbol, wanted, got)
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
			wanted = substr($0, index($0, "=") + 1)
		} else if (match($0, /^# CONFIG_[A-Za-z0-9_]+ is not set$/)) {
			symbol = $2
			wanted = "n"
		} else {
			next
		}
		got = (symbol in built) ? built[symbol] : "n"
		if (got == wanted || (wanted == "n" && !(symbol in built)))
			next
		record(symbol, wanted, (symbol in built) ? got : "absent")
	}
	END { if (count > 0) printf "%s", problems }
	' "$fragment")
	[ -z "$mismatches" ] || {
		printf '%s\n' "$fragment did not survive olddefconfig:" >&2
		printf '%s\n' "$mismatches" >&2
		fail "kernel configuration differs from its tracked fragment"
	}
}

verify_config_policy()
{
	while IFS= read -r symbol; do
		case "$symbol" in \#*|'') continue ;; esac
		grep -qx "$symbol=y" "$BUILD_DIR/.config" ||
			fail "required kernel feature is not built in: $symbol"
	done < "$REQUIRED_BUILTINS"
	while IFS= read -r symbol; do
		case "$symbol" in \#*|'') continue ;; esac
		if grep -Eq "^$symbol=(y|m)$" "$BUILD_DIR/.config"; then
			fail "forbidden kernel option is enabled: $symbol"
		fi
	done < "$FORBIDDEN_OPTIONS"
	if grep -Eq '^CONFIG_[A-Za-z0-9_]+=m$' "$BUILD_DIR/.config"; then
		fail "Android kernel configuration contains loadable modules"
	fi
}

cp -- "$CONFIG_FRAGMENT" "$BUILD_DIR/.config"
apply_page_fragment
run_make olddefconfig
verify_config_fragment "$CONFIG_FRAGMENT"
verify_config_fragment "$PAGE_FRAGMENT"
verify_config_policy
run_make -j"$JOBS"

[ -f "$BUILD_DIR/vmlinux" ] || fail "Kbuild did not produce vmlinux"
[ -s "$BUILD_DIR/modules.builtin" ] ||
	fail "Kbuild did not produce the built-in driver inventory"
module_file=$(find "$BUILD_DIR" -type f -name '*.ko' -print -quit)
[ -z "$module_file" ] || fail "loadable module output is forbidden: $module_file"
"$ELF_VERIFIER" "$BUILD_DIR/vmlinux"
"$COMMON_ROOT/abi/verify-linux-imports.sh" \
	--kernel-binary "$BUILD_DIR/vmlinux" --nm "$CLANG_BIN/llvm-nm" \
	--page-size 16k --slice arm64
printf '%s\n' "built Android 17 ACK 6.18 kernel: $BUILD_DIR/vmlinux"
