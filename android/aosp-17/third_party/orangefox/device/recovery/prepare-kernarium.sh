#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu

root=${1:?recovery root is required}
[ -d "$root" ] || {
	printf '%s\n' "prepare-kernarium.sh: recovery root does not exist: $root" >&2
	exit 1
}
root=$(CDPATH= cd "$root" && pwd -P)
case "$root" in
*/target/product/darwin/recovery/root) ;;
*)
	printf '%s\n' "prepare-kernarium.sh: unsafe recovery root: $root" >&2
	exit 1
	;;
esac
[ -d "$root/FFiles" ] || {
	printf '%s\n' "prepare-kernarium.sh: OrangeFox FFiles is missing" >&2
	exit 1
}
[ -x "$root/system/bin/toybox" ] || {
	printf '%s\n' "prepare-kernarium.sh: recovery toybox is missing" >&2
	exit 1
}

# The upstream copies are 4 KiB-only. MagiskBoot cannot patch Kernarium's
# app-owned ACK ELF, and the built 64 KiB-aligned toybox implements ps.
rm -f -- \
	"$root/FFiles/ps" \
	"$root/sbin/magiskboot" \
	"$root/system/bin/magiskboot"
ln -s -- /system/bin/toybox "$root/FFiles/ps"

# OrangeFox generates these inventories immediately before the standard board
# hook. Keep them synchronized with the final ramdisk contents.
files="$root/ramdisk-files.txt"
hashes="$root/ramdisk-files.sha256sum"
for path in "$files" "$hashes"; do
	[ -f "$path" ] || {
		printf '%s\n' "prepare-kernarium.sh: missing $(basename -- "$path")" >&2
		exit 1
	}
done
sed -e '\#sbin/magiskboot$#d' -e '\#system/bin/magiskboot$#d' \
	"$files" > "$files.kernarium"
mv -- "$files.kernarium" "$files"
sed -e '\#FFiles/ps$#d' -e '\#sbin/magiskboot$#d' \
	"$hashes" > "$hashes.kernarium"
mv -- "$hashes.kernarium" "$hashes"
printf '%s\n' "prepared 16 KiB-compatible Kernarium OrangeFox ramdisk"
