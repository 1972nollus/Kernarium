#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_root=${UML_XCFRAMEWORK_BUILD_DIR:-"$project_root/.build/xcframework"}
output=${UML_XCFRAMEWORK_OUTPUT:-"$build_root/UMLEmulator.xcframework"}
toolchain="$project_root/cmake/toolchains/Apple.cmake"
dependency_prefix=${UML_DEPENDENCY_PREFIX:-"$project_root/.build/dependencies/macos-arm64"}
archive_comparator="$project_root/tests/tooling/cmake/test_engine_object_boundaries.py"

fail()
{
	printf '%s\n' "scripts/build-xcframework.sh: $*" >&2
	exit 1
}

canonical_destination()
{
	destination=$1
	parent=$(dirname -- "$destination")
	leaf=$(basename -- "$destination")
	case "$leaf" in
	''|.|..|/) fail "refusing unsafe package destination: $destination" ;;
	esac
	mkdir -p -- "$parent"
	parent=$(CDPATH= cd -- "$parent" && pwd -P)
	printf '%s/%s\n' "$parent" "$leaf"
}

clean_build_environment()
{
	env \
		CFLAGS= \
		CXXFLAGS= \
		OBJCFLAGS= \
		CPPFLAGS= \
		ASMFLAGS= \
		LDFLAGS= \
		"$@"
}

require_cache_entry()
{
	cache=$1
	entry=$2
	grep -Fqx "$entry" "$cache" ||
		fail "configured slice is missing exact cache entry: $entry"
}

verify_release_cache()
{
	platform=$1
	build_dir=$2
	network_provider=$3
	cache="$build_dir/CMakeCache.txt"
	[ -f "$cache" ] || fail "slice has no CMake cache: $build_dir"
	require_cache_entry "$cache" "CMAKE_GENERATOR:INTERNAL=Ninja"
	require_cache_entry "$cache" "CMAKE_BUILD_TYPE:STRING=Release"
	require_cache_entry "$cache" "CMAKE_OSX_ARCHITECTURES:STRING=arm64"
	require_cache_entry "$cache" "UML_APPLE_PLATFORM:STRING=$platform"
	require_cache_entry "$cache" "UML_GUEST_16K_PAGES:BOOL=ON"
	require_cache_entry "$cache" "UML_ENABLE_RUNTIME_PROFILING:BOOL=OFF"
	require_cache_entry "$cache" "DARWIN_UML_JIT_GPR_LINK_PROFILE:BOOL=OFF"
	require_cache_entry "$cache" "DARWIN_UML_JIT_GPR_PRESSURE_PROFILE:BOOL=OFF"
	require_cache_entry "$cache" "UML_BUILD_CLI:BOOL=OFF"
	require_cache_entry "$cache" "UML_BUILD_KERNEL_RUNNER:BOOL=OFF"
	require_cache_entry "$cache" "UML_BUILD_UNIT_TESTS:BOOL=OFF"
	require_cache_entry "$cache" "UML_BUILD_INTEGRATION_TESTS:BOOL=OFF"
	require_cache_entry "$cache" "UML_BUILD_TOOLING_TESTS:BOOL=ON"
	require_cache_entry "$cache" "UML_BUILD_GPU:BOOL=OFF"
	require_cache_entry "$cache" \
		"UML_BUILD_NETWORK_PROVIDER:BOOL=$network_provider"
	for flags in \
		CMAKE_C_FLAGS \
		CMAKE_CXX_FLAGS \
		CMAKE_ASM_FLAGS \
		CMAKE_EXE_LINKER_FLAGS \
		CMAKE_STATIC_LINKER_FLAGS
	do
		require_cache_entry "$cache" "$flags:STRING="
	done
}

verify_public_headers()
{
	header_root=$1
	audit_dir=$2
	mkdir -p -- "$audit_dir"
	[ -f "$header_root/uml/emulator.h" ] ||
		fail "missing public header: $header_root/uml/emulator.h"
	[ -f "$header_root/uml/abi_identity.h" ] ||
		fail "missing public header: $header_root/uml/abi_identity.h"
	symlink=$(find "$header_root" -type l -print -quit)
	[ -z "$symlink" ] || fail "public header tree contains a symlink: $symlink"
	find "$header_root" -type f -print > "$audit_dir/headers.unsorted"
	LC_ALL=C sort "$audit_dir/headers.unsorted" > "$audit_dir/headers.actual"
	printf '%s\n' \
		"$header_root/uml/abi_identity.h" \
		"$header_root/uml/emulator.h" > "$audit_dir/headers.expected"
	LC_ALL=C sort "$audit_dir/headers.expected" > "$audit_dir/headers.expected.sorted"
	cmp "$audit_dir/headers.expected.sorted" "$audit_dir/headers.actual" ||
		fail "public header tree contains an unexpected or missing file: $header_root"
}

compare_static_archives()
{
	python3 "$archive_comparator" --compare-static-archives "$1" "$2"
}

verify_archive()
{
	platform=$1
	archive=$2
	audit_dir=$3
	case "$platform" in
	macos)
		expected_platform=1
		expected_minos=14.0
		;;
	ios)
		expected_platform=2
		expected_minos=16.0
		;;
	ios-simulator)
		expected_platform=7
		expected_minos=16.0
		;;
	*) fail "unknown archive platform: $platform" ;;
	esac

	[ -f "$archive" ] || fail "missing $platform archive: $archive"
	architectures=$(xcrun lipo -archs "$archive")
	[ "$architectures" = arm64 ] ||
		fail "$platform archive has unexpected architectures: $architectures"

	mkdir -p -- "$audit_dir"
	xcrun ar -t "$archive" > "$audit_dir/members.txt"
	member_count=$(awk '
		NF && $0 !~ /^__.SYMDEF/ { ++count }
		END { print count + 0 }
	' "$audit_dir/members.txt")
	[ "$member_count" -gt 0 ] || fail "$platform archive has no object members"

	xcrun otool -l "$archive" > "$audit_dir/load-commands.txt"
	awk -v expected_count="$member_count" \
		-v expected_platform="$expected_platform" \
		-v expected_minos="$expected_minos" '
		function finish_member() {
			if (member == "")
				return
			++members
			if (build_versions != 1 || platform != expected_platform ||
			    minos != expected_minos) {
				printf("invalid build metadata for %s: versions=%d platform=%s minos=%s\n",
				       member, build_versions, platform, minos) > "/dev/stderr"
				failed = 1
			}
		}
		/\.a\([^()]+\):$/ {
			finish_member()
			member = $0
			build_versions = 0
			platform = ""
			minos = ""
			in_build_version = 0
			next
		}
		$1 == "cmd" {
			in_build_version = ($2 == "LC_BUILD_VERSION")
			if (in_build_version)
				++build_versions
			next
		}
		in_build_version && $1 == "platform" && platform == "" {
			platform = $2
			next
		}
		in_build_version && $1 == "minos" && minos == "" {
			minos = $2
			next
		}
		END {
			finish_member()
			if (members != expected_count) {
				printf("archive metadata covered %d of %d members\n",
				       members, expected_count) > "/dev/stderr"
				failed = 1
			}
			exit failed
		}
	' "$audit_dir/load-commands.txt" ||
		fail "$platform archive contains a wrong-platform or wrong-version member"

	xcrun nm -gU "$archive" > "$audit_dir/global-defined.txt"
	awk '
		BEGIN {
			allowed["_uml_emulator_create"] = 1
			allowed["_uml_emulator_destroy"] = 1
			allowed["_uml_emulator_get_capabilities"] = 1
			allowed["_uml_emulator_request_stop"] = 1
			allowed["_uml_emulator_run"] = 1
		}
		$NF ~ /^_uml_emulator_/ {
			if (!($NF in allowed)) {
				printf("unexpected public API symbol: %s\n", $NF) > "/dev/stderr"
				failed = 1
			}
			++seen[$NF]
		}
		END {
			for (symbol in allowed) {
				if (seen[symbol] != 1) {
					printf("public API symbol %s has %d definitions\n",
					       symbol, seen[symbol] + 0) > "/dev/stderr"
					failed = 1
				}
			}
			exit failed
		}
	' "$audit_dir/global-defined.txt" ||
		fail "$platform archive does not implement the exact public C API"

	LC_ALL=C strings "$archive" > "$audit_dir/strings.txt"
	if grep -F "$project_root" "$audit_dir/strings.txt" >/dev/null; then
		fail "$platform archive contains a source-tree path"
	fi
	if grep -F "$work_root" "$audit_dir/strings.txt" >/dev/null; then
		fail "$platform archive contains a package build-tree path"
	fi

	if [ "$platform" != macos ]; then
		xcrun nm -o "$archive" > "$audit_dir/all-symbols.txt"
		awk '
			/ U _(mprotect|vm_protect|mach_vm_protect|pthread_jit_write_protect_np|sys_icache_invalidate)$/ &&
			$0 !~ /:jit_arena\.cpp\.o:/ { print }
		' "$audit_dir/all-symbols.txt" > "$audit_dir/non-jit-publishers.txt"
		if [ -s "$audit_dir/non-jit-publishers.txt" ]; then
			printf '%s\n' \
				"non-JIT executable-memory primitive in $platform archive:" >&2
			cat "$audit_dir/non-jit-publishers.txt" >&2
			exit 1
		fi
	fi
}

build_consumer()
{
	platform=$1
	mode=$2
	location=$3
	headers=$4
	consumer_dir=$5

	if [ "$mode" = package ]; then
		clean_build_environment cmake \
			-S "$project_root/tests/consumer/apple" \
			-B "$consumer_dir" -G Ninja \
			-DCMAKE_TOOLCHAIN_FILE="$toolchain" \
			-DUML_APPLE_PLATFORM="$platform" \
			-DUML_EMULATOR_PACKAGE_PREFIX="$location" \
			-DUML_EXPECTED_GUEST_PAGE_SIZE=16384 \
			-DCMAKE_BUILD_TYPE=Release \
			-DCMAKE_C_FLAGS:STRING= \
			-DCMAKE_CXX_FLAGS:STRING= \
			-DCMAKE_OBJC_FLAGS:STRING= \
			-DCMAKE_EXE_LINKER_FLAGS:STRING=
	else
		clean_build_environment cmake \
			-S "$project_root/tests/consumer/apple" \
			-B "$consumer_dir" -G Ninja \
			-DCMAKE_TOOLCHAIN_FILE="$toolchain" \
			-DUML_APPLE_PLATFORM="$platform" \
			-DUML_EMULATOR_LIBRARY="$location" \
			-DUML_EMULATOR_HEADERS="$headers" \
			-DUML_EXPECTED_GUEST_PAGE_SIZE=16384 \
			-DCMAKE_BUILD_TYPE=Release \
			-DCMAKE_C_FLAGS:STRING= \
			-DCMAKE_CXX_FLAGS:STRING= \
			-DCMAKE_OBJC_FLAGS:STRING= \
			-DCMAKE_EXE_LINKER_FLAGS:STRING=
	fi
	cmake --build "$consumer_dir" --config Release \
		--target uml-emulator-external-consumer \
			uml-emulator-external-c-consumer \
			uml-emulator-external-objc-consumer --parallel
	if [ "$platform" = macos ]; then
		"$consumer_dir/uml-emulator-external-consumer"
		"$consumer_dir/uml-emulator-external-c-consumer"
		"$consumer_dir/uml-emulator-external-objc-consumer"
	fi
}

build_slice()
{
	platform=$1
	build_dir="$work_root/$platform"
	network_provider=OFF
	if [ "$platform" = macos ]; then
		# Build the app-owned provider beside the neutral callback archive so the
		# package audit also verifies that the provider never enters the archive.
		network_provider=ON
	fi
	clean_build_environment cmake -S "$project_root" -B "$build_dir" -G Ninja \
		-DCMAKE_TOOLCHAIN_FILE="$toolchain" \
		-DUML_APPLE_PLATFORM="$platform" \
		-DUML_BUILD_CLI=OFF \
		-DUML_BUILD_KERNEL_RUNNER=OFF \
		-DUML_BUILD_UNIT_TESTS=OFF \
		-DUML_BUILD_INTEGRATION_TESTS=OFF \
		-DUML_BUILD_TOOLING_TESTS=ON \
		-DUML_BUILD_GPU=OFF \
		-DUML_BUILD_NETWORK_PROVIDER="$network_provider" \
		-DUML_DEPENDENCY_PREFIX="$dependency_prefix" \
		-DUML_GUEST_16K_PAGES=ON \
		-DUML_ENABLE_RUNTIME_PROFILING=OFF \
		-DDARWIN_UML_JIT_GPR_LINK_PROFILE=OFF \
		-DDARWIN_UML_JIT_GPR_PRESSURE_PROFILE=OFF \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_C_FLAGS:STRING= \
		-DCMAKE_CXX_FLAGS:STRING= \
		-DCMAKE_ASM_FLAGS:STRING= \
		-DCMAKE_EXE_LINKER_FLAGS:STRING= \
		-DCMAKE_STATIC_LINKER_FLAGS:STRING=
	verify_release_cache "$platform" "$build_dir" "$network_provider"

	set -- uml_emulator architecture-audit
	if [ "$platform" = macos ]; then
		set -- "$@" uml_network_provider
	fi
	cmake --build "$build_dir" --config Release --target "$@" --parallel
	ctest --test-dir "$build_dir" -C Release --output-on-failure \
		--no-tests=error -R '^engine-object-boundary-audit$'

	library="$build_dir/darwin/emulator/libuml_emulator.a"
	verify_archive "$platform" "$library" "$build_dir/package-audit/built"

	install_dir="$build_dir/install"
	cmake --install "$build_dir" --config Release --prefix "$install_dir"
	installed_library="$install_dir/lib/libuml_emulator.a"
	compare_static_archives "$library" "$installed_library"
	cmp "$project_root/darwin/api/include/uml/emulator.h" \
		"$install_dir/include/uml/emulator.h"
	cmp "$project_root/darwin/api/include/uml/abi_identity.h" \
		"$install_dir/include/uml/abi_identity.h"
	verify_public_headers "$install_dir/include" \
		"$build_dir/package-audit/installed-headers"
	[ -f "$install_dir/lib/cmake/UMLEmulator/UMLEmulatorConfig.cmake" ] ||
		fail "$platform install is missing its CMake package metadata"
	verify_archive "$platform" "$installed_library" \
		"$build_dir/package-audit/installed"
	build_consumer "$platform" package "$install_dir" "" \
		"$build_dir/installed-external-consumer"

	case "$platform" in
	macos) macos_install=$install_dir ;;
	ios) ios_install=$install_dir ;;
	ios-simulator) ios_simulator_install=$install_dir ;;
	esac
}

verify_final_slice()
{
	platform=$1
	identifier=$2
	install_dir=$3
	slice_root="$staged_output/$identifier"
	final_library="$slice_root/libuml_emulator.a"
	final_headers="$slice_root/Headers"
	verify_archive "$platform" "$final_library" \
		"$work_root/final-audit/$platform"
	compare_static_archives "$install_dir/lib/libuml_emulator.a" "$final_library"
	cmp "$install_dir/include/uml/emulator.h" \
		"$final_headers/uml/emulator.h"
	cmp "$install_dir/include/uml/abi_identity.h" \
		"$final_headers/uml/abi_identity.h"
	verify_public_headers "$final_headers" \
		"$work_root/final-audit/$platform-headers"
	build_consumer "$platform" raw "$final_library" "$final_headers" \
		"$work_root/final-consumer/$platform"
}

build_root=$(canonical_destination "$build_root")
output=$(canonical_destination "$output")
if [ "$(basename -- "$output")" != UMLEmulator.xcframework ]; then
	fail "XCFramework output must be named UMLEmulator.xcframework: $output"
fi
case "$build_root" in
"$output"|"$output"/*)
	fail "XCFramework output may not contain its slice build root: $output"
	;;
esac
mkdir -p -- "$build_root"

work_root=''
package_stage=''
cleanup()
{
	if [ -n "$work_root" ] && [ -d "$work_root" ]; then
		rm -rf -- "$work_root"
	fi
	if [ -n "$package_stage" ] && [ -d "$package_stage" ]; then
		rm -rf -- "$package_stage"
	fi
}
trap cleanup EXIT HUP INT TERM
work_root=$(mktemp -d "$build_root/.xcframework-slices.XXXXXX")
package_stage=$(mktemp -d "$build_root/.xcframework-output.XXXXXX")
staged_output="$package_stage/UMLEmulator.xcframework"

macos_install=''
ios_install=''
ios_simulator_install=''
build_slice macos
build_slice ios
build_slice ios-simulator

xcodebuild -create-xcframework \
	-library "$macos_install/lib/libuml_emulator.a" \
	-headers "$macos_install/include" \
	-library "$ios_install/lib/libuml_emulator.a" \
	-headers "$ios_install/include" \
	-library "$ios_simulator_install/lib/libuml_emulator.a" \
	-headers "$ios_simulator_install/include" \
	-output "$staged_output"

plist="$staged_output/Info.plist"
[ -f "$plist" ] || fail "XCFramework has no Info.plist: $staged_output"
plist_buddy=/usr/libexec/PlistBuddy
[ -x "$plist_buddy" ] || fail "PlistBuddy is required"
saw_macos=0
saw_ios=0
saw_simulator=0
for index in 0 1 2; do
	platform=$($plist_buddy -c \
		"Print :AvailableLibraries:$index:SupportedPlatform" "$plist")
	variant=$($plist_buddy -c \
		"Print :AvailableLibraries:$index:SupportedPlatformVariant" \
		"$plist" 2>/dev/null || true)
	architecture=$($plist_buddy -c \
		"Print :AvailableLibraries:$index:SupportedArchitectures:0" "$plist")
	identifier=$($plist_buddy -c \
		"Print :AvailableLibraries:$index:LibraryIdentifier" "$plist")
	library_path=$($plist_buddy -c \
		"Print :AvailableLibraries:$index:LibraryPath" "$plist")
	headers_path=$($plist_buddy -c \
		"Print :AvailableLibraries:$index:HeadersPath" "$plist")
	[ "$architecture" = arm64 ] ||
		fail "XCFramework slice $index is not arm64: $architecture"
	if $plist_buddy -c \
		"Print :AvailableLibraries:$index:SupportedArchitectures:1" \
		"$plist" >/dev/null 2>&1; then
		fail "XCFramework slice $index contains more than one architecture"
	fi
	[ "$library_path" = libuml_emulator.a ] ||
		fail "XCFramework slice $index has unexpected library path: $library_path"
	[ "$headers_path" = Headers ] ||
		fail "XCFramework slice $index has unexpected headers path: $headers_path"
	case "$platform:$variant:$identifier" in
	macos::macos-arm64) saw_macos=$((saw_macos + 1)) ;;
	ios::ios-arm64) saw_ios=$((saw_ios + 1)) ;;
	ios:simulator:ios-arm64-simulator)
		saw_simulator=$((saw_simulator + 1))
		;;
	*) fail "unexpected XCFramework slice $index: $platform:$variant:$identifier" ;;
	esac
done
if $plist_buddy -c "Print :AvailableLibraries:3" "$plist" \
	>/dev/null 2>&1; then
	fail "XCFramework contains more than the three supported slices"
fi
[ "$saw_macos" -eq 1 ] && [ "$saw_ios" -eq 1 ] &&
	[ "$saw_simulator" -eq 1 ] ||
	fail "XCFramework does not contain one exact supported slice per platform"

verify_final_slice macos macos-arm64 "$macos_install"
verify_final_slice ios ios-arm64 "$ios_install"
verify_final_slice ios-simulator ios-arm64-simulator "$ios_simulator_install"

rm -rf -- "$output"
mv -- "$staged_output" "$output"
printf '%s\n' "$output"
