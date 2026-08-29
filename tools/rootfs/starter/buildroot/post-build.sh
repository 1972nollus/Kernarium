#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Buildroot passes TARGET_DIR as the first argument.
set -eu

target=${1:?Buildroot TARGET_DIR is required}
[ -x "$target/bin/busybox" ] || {
	printf '%s\n' "minimal initramfs: BusyBox is missing" >&2
	exit 1
}
[ -x "$target/sbin/resize2fs" ] || {
	printf '%s\n' "minimal initramfs: resize2fs is missing" >&2
	exit 1
}
if readelf -l "$target/bin/busybox" "$target/sbin/resize2fs" |
	grep -q 'Requesting program interpreter'; then
	printf '%s\n' "minimal initramfs: retained programs must be static" >&2
	exit 1
fi
for program in "$target/bin/busybox" "$target/sbin/resize2fs"; do
	if ! readelf -lW "$program" | awk '
		$1 == "LOAD" { found = 1; if ($NF != "0x4000") exit 1 }
		END { if (!found) exit 1 }
	'; then
		printf '%s\n' \
			"minimal initramfs: $program lacks 16 KiB ELF segments" >&2
		exit 1
	fi
done

staging=$(mktemp -d "${target}.minimal.XXXXXX")
cleanup()
{
	rm -rf -- "$staging"
}
trap cleanup EXIT HUP INT TERM

install -d -m 0755 \
	"$staging/bin" "$staging/dev" "$staging/etc" "$staging/newroot" \
	"$staging/proc" "$staging/sbin" "$staging/sys"
install -m 0755 "$target/init" "$staging/init"
install -m 0755 "$target/bin/busybox" "$staging/bin/busybox"
install -m 0755 "$target/sbin/resize2fs" "$staging/sbin/resize2fs"
install -m 0644 "$target/etc/passwd" "$staging/etc/passwd"
install -m 0600 "$target/etc/shadow" "$staging/etc/shadow"
ln -s busybox "$staging/bin/sh"
ln -s busybox "$staging/bin/echo"
ln -s busybox "$staging/bin/mkdir"
ln -s busybox "$staging/bin/mount"
ln -s busybox "$staging/bin/sleep"
ln -s busybox "$staging/bin/sync"
ln -s busybox "$staging/bin/["
ln -s ../bin/busybox "$staging/sbin/blkid"
ln -s ../bin/busybox "$staging/sbin/switch_root"
ln -s ../proc/mounts "$staging/etc/mtab"

find "$target" -mindepth 1 -maxdepth 1 -exec rm -rf -- {} +
cp -a "$staging/." "$target/"
