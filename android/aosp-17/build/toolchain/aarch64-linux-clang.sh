#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

: "${UML_ANDROID17_TARGET_CLANG:?target Clang path is required}"
exec "$UML_ANDROID17_TARGET_CLANG" \
	--target=aarch64-unknown-linux-gnu "$@"
