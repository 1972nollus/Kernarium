#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copy or verify the canonical ABI files at their host and Linux consumers.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
ABI_DIR="$ROOT_DIR/kernel/common/abi"
IDENTITY_PATH="$ABI_DIR/identity.env"
PUBLIC_IDENTITY_PATH="$ROOT_DIR/darwin/api/abi/identity.env"
HOST_DIR="$ROOT_DIR/darwin/include"
KERNEL_SOURCE=''
KERNEL_METADATA=''
VERIFY=0

fail()
{
	printf '%s\n' "kernel/common/abi/sync.sh: $*" >&2
	exit 1
}

usage()
{
	cat <<'EOF'
Usage: kernel/common/abi/sync.sh [--host-dir PATH]
       [--kernel-source PATH] [--kernel-metadata PATH] [--verify]

Without --verify, copy canonical ABI files to the requested destinations.
With --verify, require every requested destination to be byte-identical.
When --kernel-metadata is supplied, also require that one kernel line to name
the common ABI identity. The common ABI has no implicit kernel-line owner.
EOF
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--host-dir)
		[ "$#" -ge 2 ] || fail "--host-dir requires a path"
		HOST_DIR=$2
		shift 2
		;;
	--kernel-source)
		[ "$#" -ge 2 ] || fail "--kernel-source requires a path"
		KERNEL_SOURCE=$2
		shift 2
		;;
	--kernel-metadata)
		[ "$#" -ge 2 ] || fail "--kernel-metadata requires a path"
		KERNEL_METADATA=$2
		shift 2
		;;
	--verify)
		VERIFY=1
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

sync_file()
{
	source=$1
	destination=$2
	[ -f "$source" ] || fail "missing canonical source: $source"
	if [ "$VERIFY" -eq 1 ]; then
		[ -f "$destination" ] || fail "missing generated ABI file: $destination"
		cmp -s "$source" "$destination" ||
			fail "generated ABI file differs from canonical source: $destination"
		return
	fi
	mkdir -p -- "$(dirname -- "$destination")"
	if ! cmp -s "$source" "$destination" 2>/dev/null; then
		cp -- "$source" "$destination"
	fi
}

[ -r "$IDENTITY_PATH" ] || fail "missing ABI identity: $IDENTITY_PATH"
[ -r "$PUBLIC_IDENTITY_PATH" ] ||
	fail "missing public ABI identity: $PUBLIC_IDENTITY_PATH"
"$ABI_DIR/verify-declarations.py"
# Both files are tracked project data containing simple shell assignments.
# shellcheck disable=SC1090
. "$IDENTITY_PATH"
IDENTITY_VERSION=$DARWIN_UML_ABI_VERSION
IDENTITY_FINGERPRINT=$DARWIN_UML_ABI_FINGERPRINT
# shellcheck disable=SC1090
. "$PUBLIC_IDENTITY_PATH"
PUBLIC_IDENTITY_VERSION=$UML_EMULATOR_ABI_VERSION
PUBLIC_IDENTITY_FINGERPRINT=$UML_EMULATOR_ABI_FINGERPRINT
if [ -n "$KERNEL_METADATA" ]; then
	[ -r "$KERNEL_METADATA" ] ||
		fail "missing kernel metadata: $KERNEL_METADATA"
	# shellcheck disable=SC1090
	. "$KERNEL_METADATA"
	[ "$UML_KERNEL_ABI_VERSION" = "$IDENTITY_VERSION" ] ||
		fail "kernel metadata ABI version differs from identity.env"
	[ "$UML_KERNEL_ABI_FINGERPRINT" = "$IDENTITY_FINGERPRINT" ] ||
		fail "kernel metadata ABI fingerprint differs from identity.env"
fi
grep -q "DARWIN_UML_ABI_VERSION ${IDENTITY_VERSION}U" \
	"$ABI_DIR/darwin_uml_abi_identity.h" ||
	fail "ABI identity header version differs from identity.env"
grep -q "DARWIN_UML_ABI_FINGERPRINT ${IDENTITY_FINGERPRINT}ULL" \
	"$ABI_DIR/darwin_uml_abi_identity.h" ||
	fail "ABI identity header fingerprint differs from identity.env"
grep -q "UML_EMULATOR_ABI_VERSION ${PUBLIC_IDENTITY_VERSION}U" \
	"$ROOT_DIR/darwin/api/include/uml/abi_identity.h" ||
	fail "public ABI identity header version differs from identity.env"
grep -q "UML_EMULATOR_ABI_FINGERPRINT ${PUBLIC_IDENTITY_FINGERPRINT}ULL" \
	"$ROOT_DIR/darwin/api/include/uml/abi_identity.h" ||
	fail "public ABI identity header fingerprint differs from identity.env"

sync_file "$ABI_DIR/darwin_uml_abi.h" "$HOST_DIR/uml_darwin_abi.h"
sync_file "$ABI_DIR/darwin_uml_abi_identity.h" \
	"$HOST_DIR/uml_darwin_abi_identity.h"
sync_file "$ABI_DIR/darwin_uml_cpu.h" "$HOST_DIR/uml_darwin_cpu.h"
sync_file "$ABI_DIR/darwin_uml_host.h" "$HOST_DIR/uml_darwin_host.h"
sync_file "$ABI_DIR/darwin_uml_memory.h" "$HOST_DIR/uml_darwin_memory.h"
sync_file "$ABI_DIR/darwin_uml_network.h" "$HOST_DIR/uml_darwin_network.h"
sync_file "$ABI_DIR/darwin_uml_bridge.h" "$HOST_DIR/uml_darwin_bridge.h"
sync_file "$ABI_DIR/uml_tlb_sync_ranges.h" "$HOST_DIR/uml/tlb_sync_ranges.h"

if [ -n "$KERNEL_SOURCE" ]; then
	[ -d "$KERNEL_SOURCE" ] || fail "kernel source is not a directory: $KERNEL_SOURCE"
	sync_file "$ABI_DIR/darwin_uml_abi.h" \
		"$KERNEL_SOURCE/arch/um/os-Darwin/uml_darwin_abi.h"
	sync_file "$ABI_DIR/darwin_uml_abi_identity.h" \
		"$KERNEL_SOURCE/arch/um/os-Darwin/uml_darwin_abi_identity.h"
	sync_file "$ABI_DIR/darwin_uml_cpu.h" \
		"$KERNEL_SOURCE/arch/um/os-Darwin/uml_darwin_cpu.h"
	sync_file "$ABI_DIR/darwin_uml_host.h" \
		"$KERNEL_SOURCE/arch/um/os-Darwin/uml_darwin_host.h"
	sync_file "$ABI_DIR/darwin_uml_memory.h" \
		"$KERNEL_SOURCE/arch/um/os-Darwin/uml_darwin_memory.h"
	sync_file "$ABI_DIR/darwin_uml_network.h" \
		"$KERNEL_SOURCE/arch/um/os-Darwin/uml_darwin_network.h"
	sync_file "$ABI_DIR/darwin_uml_bridge.h" \
		"$KERNEL_SOURCE/arch/um/os-Darwin/uml_darwin_bridge.h"
	sync_file "$ABI_DIR/uml_tlb_sync_ranges.h" \
		"$KERNEL_SOURCE/arch/um/include/asm/tlb_sync_ranges.h"
fi
