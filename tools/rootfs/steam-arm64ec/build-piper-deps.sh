#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Runs inside the offline Proton SDK; retains each dependency's CMake build.
set -euo pipefail
export CCACHE_DISABLE=1
inputs=$1
work=$2
jobs=$3
mkdir -p "$work/source" "$work/prefix"
if [ ! -f "$work/source/.unpacked" ]; then
	unzip -qo "$inputs/fmt-10.0.0.zip" -d "$work/source"
	unzip -qo "$inputs/spdlog-1.12.0.zip" -d "$work/source"
	unzip -qo "$inputs/espeak-ng.zip" -d "$work/source"
	tar -xf "$inputs/piper-phonemize.tar.gz" -C "$work/source"
	tar -xf "$inputs/onnxruntime.tgz" -C "$work/source"
	touch "$work/source/.unpacked"
fi
if [ ! -d "$work/source/sonic-fbf75c3d6d846bad3bb3d456cbc5d07d9fd8c104" ]; then
	tar -xf "$inputs/sonic.tar.gz" -C "$work/source"
fi
build()
{
	name=$1
	source=$2
	shift 2
	cmake -S "$work/source/$source" -B "$work/build-$name" \
		-DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
		-DCMAKE_INSTALL_PREFIX="$work/prefix/$name" \
		-DCMAKE_INSTALL_LIBDIR=lib "$@"
	cmake --build "$work/build-$name" -j"$jobs"
	cmake --install "$work/build-$name"
}
build fmt fmt-10.0.0 -DFMT_TEST=OFF
build spdlog spdlog-1.12.0 -DSPDLOG_BUILD_TESTS=OFF
build espeak espeak-ng-0f65aa301e0d6bae5e172cc74197d32a6182200f \
	-DUSE_ASYNC=OFF -DBUILD_SHARED_LIBS=ON -DUSE_MBROLA=OFF \
	-DUSE_LIBSONIC=OFF -DUSE_LIBPCAUDIO=OFF -DUSE_KLATT=OFF \
	-DUSE_SPEECHPLAYER=OFF -DEXTRA_cmn=ON -DEXTRA_ru=ON \
	-DFETCHCONTENT_SOURCE_DIR_SONIC-GIT="$work/source/sonic-fbf75c3d6d846bad3bb3d456cbc5d07d9fd8c104" \
	-DCMAKE_C_FLAGS=-D_FILE_OFFSET_BITS=64
build phonemize piper-phonemize-5ecbb9b59abcffe03a9775ae82491c9ac5037f4d \
	-DESPEAK_NG_DIR="$work/prefix/espeak" \
	-DONNXRUNTIME_DIR="$work/source/onnxruntime-linux-aarch64-1.14.1"
