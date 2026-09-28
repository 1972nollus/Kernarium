#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build the x86-64 LTP qualification root on native x86-64 Linux.
set -euo pipefail

PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export PATH

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEBIAN_SUITE=trixie
LTP_TAG=20260529
LTP_REPOSITORY=https://github.com/linux-test-project/ltp.git

fail()
{
	printf '%s\n' "tools/rootfs/ltp-x86_64/build.sh: $*" >&2
	exit 1
}

usage()
{
	cat <<'EOF'
Usage: tools/rootfs/ltp-x86_64/build.sh --work PATH --output PATH.erofs

Run as an ordinary user with passwordless sudo on native x86-64 Debian Linux.
Both paths must be absolute, distinct, and absent. The completed EROFS image,
package inventory, ELF inventory, and checksum are published after validation.
EOF
}

work_dir=
output=
while [ "$#" -gt 0 ]; do
	case "$1" in
	--work)
		[ "$#" -ge 2 ] || fail "--work requires a path"
		work_dir=$2
		shift 2
		;;
	--output)
		[ "$#" -ge 2 ] || fail "--output requires a path"
		output=$2
		shift 2
		;;
	-h|--help)
		usage
		exit 0
		;;
	*) fail "unknown option: $1" ;;
	esac
done

[ -n "$work_dir" ] && [ -n "$output" ] || {
	usage >&2
	exit 2
}
case "$work_dir:$output" in
/*:/*) ;;
*) fail "--work and --output must be absolute" ;;
esac
[ "$work_dir" != "$output" ] || fail "work and output paths must differ"
case "$work_dir" in /|/home|/tmp|/usr|/var) fail "unsafe work path: $work_dir" ;; esac
case "$output" in /|/home|/tmp|/usr|/var) fail "unsafe output path: $output" ;; esac
case "$output" in *.erofs) ;; *) fail "--output must end in .erofs" ;; esac
[ ! -e "$work_dir" ] && [ ! -L "$work_dir" ] || \
	fail "refusing to replace work path: $work_dir"
for artifact in "$output" "$output.packages.tsv" "$output.elf.tsv" \
	"$output.sha256"; do
	[ ! -e "$artifact" ] && [ ! -L "$artifact" ] || \
		fail "refusing to replace output: $artifact"
done
[ "$(uname -s)" = Linux ] || fail "the producer requires Linux"
[ "$(uname -m)" = x86_64 ] || fail "the producer requires native x86-64"
[ "$(id -u)" -ne 0 ] || fail "run as an ordinary user; sudo is used narrowly"
sudo -n true 2>/dev/null || fail "passwordless sudo is required"
for command in chroot debootstrap git mkfs.erofs mountpoint python3 \
	sha256sum sort sudo; do
	command -v "$command" >/dev/null 2>&1 || \
		fail "required command is unavailable: $command"
done

output_parent=$(dirname -- "$output")
output_name=$(basename -- "$output")
case "$output_name" in ''|.|..) fail "unsafe output name: $output_name" ;; esac
mkdir -p -- "$output_parent"
output_parent=$(CDPATH= cd -- "$output_parent" && pwd -P)
output=$output_parent/$output_name
mkdir -p -- "$work_dir"
work_dir=$(CDPATH= cd -- "$work_dir" && pwd -P)
root=$work_dir/root
mkdir -p -- "$root"

owner_uid=$(id -u)
owner_gid=$(id -g)
# Stage in a directory this user owns beside the output, so the name root's
# mkfs.erofs writes stays reserved and publication is a same-directory rename.
staging_dir=$(mktemp -d "$output_parent/.$output_name.partial.XXXXXX")
staging=$staging_dir/$output_name
unmount_root()
{
	for mount_point in dev sys proc; do
		if mountpoint -q "$root/$mount_point"; then
			sudo umount -R "$root/$mount_point"
		fi
	done
}
cleanup()
{
	unmount_root || true
	[ -n "$staging_dir" ] || return
	sudo rm -rf -- "$staging_dir"
}
publish()
{
	mv -T -n -- "$1" "$2"
	[ ! -e "$1" ] || fail "output appeared during publication: $2"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

# The runtime set matches the LTP-relevant userspace of the arm64 product
# rootfs. The build set stays installed because several LTP command
# tests exercise the toolchain, archivers, and gdb.
runtime_packages='adduser bash bc bzip2 ca-certificates coreutils curl dash
diffutils dmsetup e2fsprogs file findutils gdb grep gzip hostname iproute2
iptables iputils-ping kmod less libacl1 libaio1t64 libblkid1 libbpf1
libcap2-bin libcrypt1 libfuse3-4 libibverbs1 libkeyutils1 libmnl0 libmount1
libnuma1 libselinux1 libssl3t64 libtirpc3t64 login mawk mount mtr-tiny nano
net-tools nftables openssh-client parted passwd pciutils perl procps psmisc
python3 quota sed sudo systemd systemd-sysv tar udev unzip util-linux
xz-utils'
build_packages='autoconf automake build-essential git libacl1-dev libaio-dev
libattr1-dev libcap-dev libnuma-dev libselinux1-dev linux-libc-dev make
pkgconf'

printf '%s\n' '==> Bootstrapping Debian 13 amd64'
sudo debootstrap --arch=amd64 --variant=minbase \
	--include="$(printf '%s\n' $runtime_packages $build_packages | paste -sd, -)" \
	"$DEBIAN_SUITE" "$root" http://deb.debian.org/debian

printf '%s\n' "==> Building LTP $LTP_TAG"
git clone --quiet --depth 1 --branch "$LTP_TAG" "$LTP_REPOSITORY" \
	"$work_dir/ltp"
sudo install -d -m 0755 "$root/usr/src"
sudo cp -a "$work_dir/ltp" "$root/usr/src/ltp"
for mount_point in proc sys dev; do
	sudo mount --rbind "/$mount_point" "$root/$mount_point"
done
sudo chroot "$root" /bin/bash -euc '
	cd /usr/src/ltp
	make autotools
	./configure --prefix=/opt/ltp
	make -j"$(nproc)"
	make install
'
unmount_root
[ "$(cat "$root/opt/ltp/Version")" = "$LTP_TAG" ] || \
	fail "installed LTP does not report version $LTP_TAG"
sudo rm -rf -- "$root/usr/src/ltp"

printf '%s\n' '==> Auditing the installed package state'
package_audit=$(sudo chroot "$root" dpkg --audit)
[ -z "$package_audit" ] || fail "dpkg --audit reported incomplete packages:
$package_audit"
sudo chroot "$root" dpkg-query -W -f='${binary:Package}\t${Version}\n' |
	sort > "$staging.packages.tsv"

printf '%s\n' '==> Verifying that every ELF payload is x86-family'
sudo python3 "$SCRIPT_DIR/../x86_64-desktop/verify-elf.py" \
	--root "$root" --output "$staging.elf.tsv"
sudo chown "$owner_uid:$owner_gid" "$staging.elf.tsv"

sudo chroot "$root" apt-get clean
sudo rm -rf -- "$root/var/lib/apt/lists"/* "$root/var/cache/apt"/* \
	"$root/tmp"/* "$root/var/tmp"/* "$root/run"/*
sudo find "$root/var/log" -type f -exec truncate -s 0 {} \;
printf '%s\n' uml | sudo tee "$root/etc/hostname" >/dev/null
sudo truncate -s 0 "$root/etc/machine-id"

printf '%s\n' '==> Building EROFS'
sudo mkfs.erofs -L uml-ltp-x86-64 -zlz4hc "$staging" "$root"
sudo chown "$owner_uid:$owner_gid" "$staging"
checksum=$(sha256sum "$staging" | awk '{print $1}')
printf '%s  %s\n' "$checksum" "$output_name" > "$staging.sha256"

publish "$staging" "$output"
publish "$staging.packages.tsv" "$output.packages.tsv"
publish "$staging.elf.tsv" "$output.elf.tsv"
publish "$staging.sha256" "$output.sha256"
rmdir -- "$staging_dir"
staging_dir=
trap - EXIT HUP INT TERM
printf '%s\n' "Built x86-64 LTP rootfs: $output"
