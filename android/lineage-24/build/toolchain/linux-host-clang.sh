#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Kbuild's builder-host C compiler. As in Kleaf, host tools target the pinned
# ACK musl sysroot and link with lld and compiler-rt, so they need nothing from
# the builder's own libraries; resolve_btfids links the kernel-build-tools
# libelf whose headers the include directory supplies.
set -eu

: "${UML_ANDROID17_HOST_CLANG:?host Clang path is required}"
: "${UML_ANDROID17_HOST_SYSROOT:?host sysroot path is required}"
: "${UML_ANDROID17_HOST_INCLUDE:?host library header path is required}"

exec "$UML_ANDROID17_HOST_CLANG" \
	--target=x86_64-unknown-linux-musl \
	--sysroot="$UML_ANDROID17_HOST_SYSROOT" \
	--start-no-unused-arguments \
	-isystem "$UML_ANDROID17_HOST_INCLUDE" \
	-fuse-ld=lld -rtlib=compiler-rt \
	--end-no-unused-arguments "$@"
