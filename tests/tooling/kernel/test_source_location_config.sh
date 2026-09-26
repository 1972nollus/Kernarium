#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
LINE_ROOT="$ROOT_DIR/kernel/mainline-6.18"
BUILD_SCRIPT="$LINE_ROOT/build/build-kernel.sh"
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/uml-kernel-source-config.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM

fail()
{
	printf '%s\n' "test_source_location_config.sh: $*" >&2
	exit 1
}

MISSING_CONFIG="$TEST_ROOT/missing.conf"
if UML_KERNEL_SOURCE_CONFIG="$MISSING_CONFIG" \
	"$BUILD_SCRIPT" --build "$TEST_ROOT/build" --page-size 16k \
	> "$TEST_ROOT/build.out" 2> "$TEST_ROOT/build.err"; then
	fail "kernel build accepted a missing source-location configuration"
fi
grep -F 'required configuration is missing' "$TEST_ROOT/build.err" >/dev/null ||
	fail "missing configuration failure did not identify the required file"
grep -F 'kernel build requires a valid kernel/mainline-6.18/source-location.conf' \
	"$TEST_ROOT/build.err" >/dev/null ||
	fail "build did not report its mandatory source-location contract"

printf '%s\n' "kernel source-location configuration tests passed"
