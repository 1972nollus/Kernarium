#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

: "${UML_ANDROID17_HOST_CLANG:?host Clang path is required}"
: "${UML_ANDROID17_HOST_SYSROOT:?host sysroot path is required}"
: "${UML_ANDROID17_HOST_GCC_LIB:?host GCC runtime path is required}"
: "${UML_ANDROID17_HOST_COMPAT:?host compatibility header is required}"

exec "$UML_ANDROID17_HOST_CLANG" \
	--sysroot="$UML_ANDROID17_HOST_SYSROOT" \
	-B"$UML_ANDROID17_HOST_GCC_LIB" \
	-include "$UML_ANDROID17_HOST_SYSROOT/usr/include/linux/types.h" \
	-include "$UML_ANDROID17_HOST_COMPAT" "$@"
