#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository=$(CDPATH= cd -- "$here/../../.." && pwd)
if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
	printf '%s\n' 'Usage: build-proton.sh INPUTS ABSENT_WORK [JOBS]' >&2
	exit 2
fi
inputs=$(realpath "$1")
work=$2
jobs=${3:-8}
case "$work" in /*) ;; *) exit 2 ;; esac
case "$jobs" in ''|*[!0-9]*|0) exit 2 ;; esac
[ ! -e "$work" ] && [ ! -L "$work" ]
"$repository/dependencies/prepare.sh" --product steam-arm64ec-rootfs --root "$inputs" --verify
mkdir -p "$work/build"
cp -a --reflink=auto "$inputs/proton" "$work/source"
source=$work/source
for component in proton proton-wine; do
	checkout=$source
	[ "$component" != proton-wine ] || checkout=$source/wine
	while IFS=$'\t' read -r order patch rest; do
		case "$order" in ''|\#*|order) continue ;; esac
		git -C "$checkout" apply "$repository/dependencies/steam-arm64ec/patches/$component/$patch"
	done < "$repository/dependencies/steam-arm64ec/patches/$component/series.tsv"
done
for pair in bridge:xtajit64 wow64-bridge:xtajit; do
	bridge=${pair%:*}
	module=${pair#*:}
	mkdir -p "$source/wine/dlls/$module"
	cp "$repository/tools/rootfs/wine-arm64ec/$bridge/"* "$source/wine/dlls/$module/"
	cp "$repository/darwin/runtime/wine_cpu_protocol.h" "$source/wine/dlls/$module/"
	cp "$here/wine-compat.h" "$source/wine/dlls/$module/wine_compat.h"
	printf '\ncpu_EXTRADEFS = -include wine_compat.h\n' >> "$source/wine/dlls/$module/Makefile.in"
done
sdk=$(python3 -B -c 'import json,sys; print(json.load(open(sys.argv[1]))["sdk"])' "$inputs/inputs.json")
"$here/offline-podman.sh" run --rm -u "$(id -u):$(id -g)" \
	-v "$repository:$repository:ro" -v "$inputs/piper-inputs:$inputs/piper-inputs:ro" \
	-v "$work:$work" "$sdk" bash "$here/build-piper-deps.sh" \
	"$inputs/piper-inputs" "$work/piper" "$jobs"
cd "$work/build"
"$source/configure.sh" --target-arch=arm64 --build-name=proton-uml-arm64ec \
	--proton-sdk-image="$sdk" --container-engine="$here/offline-podman.sh"
export CARGO_HOME=$inputs/cargo
make -j"$jobs" redist \
	"PIPER_CMAKE_ARGS=-DFMT_DIR=$work/piper/prefix/fmt -DSPDLOG_DIR=$work/piper/prefix/spdlog -DPIPER_PHONEMIZE_DIR=$work/piper/prefix/phonemize" \
	"DOCKER_OPTS=-v $work/piper:$work/piper:ro"
