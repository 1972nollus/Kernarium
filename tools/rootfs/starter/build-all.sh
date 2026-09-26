#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Produce the mainline Linux starter artifacts on a native arm64 Linux host.
set -eu

PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export PATH

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BUILDROOT_VERSION=2026.05.3
BUILDROOT_SHA256=f43f1627082a45848f73083b3b8d59f32bb4c77a449a8ead8a2a1f91849dea00
BUSYBOX_VERSION=1.38.0
BUSYBOX_SHA256=34f9ea6ff8636f2c9241153b9114eefa9e65674a45318ae1ef95bb5f31c53bb2
ALPINE_VERSION=3.24.2
ALPINE_SHA256=9bf70a7f18ea44094cbb5f70c58f9af129c8214745743db0e68e5502cc2ce773
DEBIAN_SUITE=trixie

fail()
{
	printf '%s\n' "tools/rootfs/starter/build-all.sh: $*" >&2
	exit 1
}

usage()
{
	cat <<'EOF'
Usage: tools/rootfs/starter/build-all.sh --work PATH --output PATH [--jobs N]

Run as an ordinary user with passwordless sudo on native arm64 Linux. PATHs
must be absolute and distinct outside the source tree. The output path must
not exist; the completed release directory is published atomically.
EOF
}

work_dir=
output_dir=
jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1\n')
while [ "$#" -gt 0 ]; do
	case "$1" in
	--work)
		[ "$#" -ge 2 ] || fail "--work requires a path"
		work_dir=$2
		shift 2
		;;
	--output)
		[ "$#" -ge 2 ] || fail "--output requires a path"
		output_dir=$2
		shift 2
		;;
	--jobs)
		[ "$#" -ge 2 ] || fail "--jobs requires a positive integer"
		jobs=$2
		shift 2
		;;
	-h|--help)
		usage
		exit 0
		;;
	*) fail "unknown option: $1" ;;
	esac
done

[ -n "$work_dir" ] && [ -n "$output_dir" ] || {
	usage >&2
	exit 2
}
case "$work_dir:$output_dir" in
/*:/*) ;;
*) fail "--work and --output must be absolute" ;;
esac
[ "$work_dir" != "$output_dir" ] || fail "work and output directories must differ"
case "$jobs" in ''|*[!0-9]*) fail "--jobs must be a positive integer" ;; esac
[ "$jobs" -gt 0 ] || fail "--jobs must be a positive integer"
[ "$(uname -s)" = Linux ] || fail "the producer requires Linux"
[ "$(uname -m)" = aarch64 ] || fail "the producer requires native aarch64"
[ "$(id -u)" -ne 0 ] || fail "run as an ordinary user; the script uses sudo narrowly"
sudo -n true 2>/dev/null || fail "passwordless sudo is required"
for command in chroot cpio curl debootstrap e2fsck gzip make mkfs.ext4 \
	python3 resize2fs sgdisk sha256sum sudo tar; do
	command -v "$command" >/dev/null 2>&1 || fail "required command is unavailable: $command"
done

requested_output_dir=$output_dir
output_parent=$(dirname -- "$requested_output_dir")
output_name=$(basename -- "$requested_output_dir")
case "$output_name" in ''|.|..|/) fail "unsafe output name: $output_name" ;; esac
mkdir -p -- "$work_dir" "$output_parent"
work_dir=$(CDPATH= cd -- "$work_dir" && pwd -P)
output_parent=$(CDPATH= cd -- "$output_parent" && pwd -P)
final_output_dir=$output_parent/$output_name
case "$work_dir" in /|/home|/Users|/tmp) fail "unsafe work directory: $work_dir" ;; esac
case "$final_output_dir" in
/|/home|/Users|/tmp) fail "unsafe output directory: $final_output_dir" ;;
esac
[ "$work_dir" != "$final_output_dir" ] || fail "work and output directories must differ"
[ ! -e "$final_output_dir" ] && [ ! -L "$final_output_dir" ] || \
	fail "refusing to replace output directory: $final_output_dir"
staging_output_dir=$(mktemp -d "$output_parent/.$output_name.partial.XXXXXX")
chroot_proc=
cleanup_output()
{
	unmount_chroot_proc || true
	[ -n "$staging_output_dir" ] || return
	rm -rf -- "$staging_output_dir"
}
trap cleanup_output EXIT
trap 'exit 1' HUP INT TERM
output_dir=$staging_output_dir

sources_dir=$work_dir/sources
roots_dir=$work_dir/roots
builds_dir=$work_dir/builds
mkdir -p -- "$sources_dir" "$roots_dir" "$builds_dir"
owner_uid=$(id -u)
owner_gid=$(id -g)

as_root()
{
	sudo -- "$@"
}

reset_root_directory()
{
	directory=$1
	case "$directory" in "$roots_dir"/*) ;;
	*) fail "refusing to reset a directory outside $roots_dir: $directory" ;;
	esac
	if [ -e "$directory" ]; then
		as_root rm -rf -- "$directory"
	fi
	as_root mkdir -p -- "$directory"
}

# Package maintainer scripts need /proc in the chroot. Without it,
# systemd-hwdb cannot replace hwdb.bin, and the udev hardware database
# silently misses the entries of every later package.
mount_chroot_proc()
{
	as_root mount -t proc proc "$1/proc"
	chroot_proc=$1/proc
}

unmount_chroot_proc()
{
	[ -n "$chroot_proc" ] || return 0
	as_root umount -- "$chroot_proc"
	chroot_proc=
}

write_root_file()
{
	destination=$1
	mode=$2
	temporary=$(mktemp "$work_dir/.root-file.XXXXXX")
	cat > "$temporary"
	as_root install -D -m "$mode" "$temporary" "$destination"
	rm -f -- "$temporary"
}

download_checked()
{
	url=$1
	expected=$2
	destination=$3
	if [ ! -f "$destination" ]; then
		partial=$destination.partial
		rm -f -- "$partial"
		curl -fL --retry 3 --output "$partial" "$url"
		mv -- "$partial" "$destination"
	fi
	actual=$(sha256sum "$destination" | awk '{print $1}')
	[ "$actual" = "$expected" ] || fail "checksum mismatch for $destination"
}

artifact_path()
{
	path=$output_dir/$1
	[ ! -e "$path" ] && [ ! -L "$path" ] || fail "refusing to replace artifact: $path"
	printf '%s\n' "$path"
}

build_initramfs()
{
	printf '%s\n' "==> Building minimal resize initramfs"
	buildroot_archive=$sources_dir/buildroot-$BUILDROOT_VERSION.tar.xz
	busybox_archive=$sources_dir/busybox-$BUSYBOX_VERSION.tar.bz2
	download_checked \
		"https://buildroot.org/downloads/buildroot-$BUILDROOT_VERSION.tar.xz" \
		"$BUILDROOT_SHA256" "$buildroot_archive"
	download_checked \
		"https://www.busybox.net/downloads/busybox-$BUSYBOX_VERSION.tar.bz2" \
		"$BUSYBOX_SHA256" "$busybox_archive"
	buildroot_source=$sources_dir/buildroot-$BUILDROOT_VERSION
	busybox_source=$sources_dir/busybox-$BUSYBOX_VERSION
	[ -d "$buildroot_source" ] || tar -C "$sources_dir" -xf "$buildroot_archive"
	[ -d "$busybox_source" ] || tar -C "$sources_dir" -xf "$busybox_archive"

	busybox_config=$builds_dir/busybox-minimal.config
	busybox_log=$builds_dir/busybox-allnoconfig.log
	make -s -C "$busybox_source" allnoconfig > "$busybox_log"
	while IFS= read -r symbol; do
		case "$symbol" in \#*|'') continue ;; esac
		if grep -qx "CONFIG_$symbol=y" "$busybox_source/.config"; then
			continue
		fi
		grep -qx "# CONFIG_$symbol is not set" "$busybox_source/.config" || \
			fail "BusyBox does not define required option CONFIG_$symbol"
		sed -i "s/^# CONFIG_$symbol is not set\$/CONFIG_$symbol=y/" \
			"$busybox_source/.config"
	done < "$SCRIPT_DIR/buildroot/busybox.fragment"
	make -s -C "$busybox_source" silentoldconfig
	while IFS= read -r symbol; do
		case "$symbol" in \#*|'') continue ;; esac
		grep -qx "CONFIG_$symbol=y" "$busybox_source/.config" || \
			fail "BusyBox rejected required option CONFIG_$symbol"
	done < "$SCRIPT_DIR/buildroot/busybox.fragment"
	cp -- "$busybox_source/.config" "$busybox_config"

	buildroot_output=$builds_dir/buildroot
	mkdir -p -- "$buildroot_output"
	if [ -d "$buildroot_output/build" ]; then
		# A failed post-build may leave the generated target deliberately
		# pruned while package stamps still describe the full tree. Reinstall
		# target payloads, but retain the expensive compiled toolchain cache.
		rm -rf -- \
			"$buildroot_output/images" \
			"$buildroot_output/staging" \
			"$buildroot_output/target"
		find "$buildroot_output/build" -name .stamp_target_installed \
			-delete
	fi
	generated_defconfig=$buildroot_output/uml-resize-defconfig
	sed \
		-e "s|@BUSYBOX_CONFIG@|$busybox_config|g" \
		-e "s|@OVERLAY@|$SCRIPT_DIR/buildroot/overlay|g" \
		-e "s|@POST_BUILD@|$SCRIPT_DIR/buildroot/post-build.sh|g" \
		"$SCRIPT_DIR/buildroot/defconfig.in" > "$generated_defconfig"
	make -C "$buildroot_source" O="$buildroot_output" \
		BR2_DL_DIR="$sources_dir/downloads" \
		BR2_DEFCONFIG="$generated_defconfig" defconfig
	make -C "$buildroot_source" O="$buildroot_output" \
		BR2_DL_DIR="$sources_dir/downloads" -j"$jobs"

	image=$buildroot_output/images/rootfs.cpio.gz
	[ -f "$image" ] || fail "Buildroot did not produce $image"
	gzip -t "$image"
	listing=$buildroot_output/initramfs.list
	gzip -dc "$image" | cpio -it > "$listing" 2>/dev/null
	for required in init bin/busybox sbin/blkid sbin/dumpe2fs sbin/e2fsck \
		sbin/resize2fs sbin/switch_root; do
		grep -qx "\./$required\|$required" "$listing" || \
			fail "initramfs is missing $required"
	done
	destination=$(artifact_path uml-resize-initramfs.cpio.gz)
	cp -- "$image" "$destination"

	# The native toolchain is several GiB and is no longer needed once the
	# self-contained cpio archive has been verified and copied into the release.
	rm -rf -- \
		"$buildroot_output/build" \
		"$buildroot_output/host" \
		"$buildroot_output/target" \
		"$buildroot_output/staging" \
		"$buildroot_output/per-package"
}

configure_alpine()
{
	root=$1
	write_root_file "$root/etc/apk/repositories" 0644 <<EOF
https://dl-cdn.alpinelinux.org/alpine/v3.24/main
https://dl-cdn.alpinelinux.org/alpine/v3.24/community
EOF
	as_root cp -L /etc/resolv.conf "$root/etc/resolv.conf"
	# The kernels have no uevent helper for BusyBox mdev, so mdevd, Alpine's
	# netlink device manager, owns /dev as setup-devd would configure it.
	as_root chroot "$root" /sbin/apk add --no-cache alpine-base ifupdown-ng \
		mdevd mdevd-openrc
	for service_level in \
		"devfs sysinit" "procfs sysinit" "sysfs sysinit" "mdevd sysinit" \
		"mdevd-init sysinit" "hostname boot" "bootmisc boot" \
		"networking boot" "local default" "killprocs shutdown" \
		"mount-ro shutdown"; do
		set -- $service_level
		if [ -x "$root/etc/init.d/$1" ]; then
			as_root chroot "$root" /sbin/rc-update add "$1" "$2"
		fi
	done
	# The kernel creates a virtio port's name attribute only for a named
	# port, and the console port is unnamed.
	mdev_conf=$root/etc/mdev.conf
	unguarded_rule='SUBSYSTEM=virtio-ports;vport.* root:root 0600 @mkdir -p virtio-ports; ln -sf ../$MDEV virtio-ports/$(cat /sys/class/virtio-ports/$MDEV/name)'
	guarded_rule='SUBSYSTEM=virtio-ports;vport.* root:root 0600 @if [ -r /sys/class/virtio-ports/$MDEV/name ]; then mkdir -p virtio-ports; ln -sf ../$MDEV virtio-ports/$(cat /sys/class/virtio-ports/$MDEV/name); fi'
	[ "$(as_root grep -cxF -- "$unguarded_rule" "$mdev_conf")" = 1 ] || \
		fail "unexpected virtio-ports rule in $mdev_conf"
	as_root cat "$mdev_conf" | \
		awk -v old="$unguarded_rule" -v new="$guarded_rule" \
			'$0 == old { print new; next } { print }' | \
		write_root_file "$mdev_conf" 0644
	as_root grep -qxF -- "$guarded_rule" "$mdev_conf" || \
		fail "could not guard the virtio-ports rule in $mdev_conf"
	# Nothing is loadable into the monolithic kernels; the default entries
	# (af_packet and ipv6) are built in.
	write_root_file "$root/etc/modules" 0644 <<'EOF'
# The Linux kernels for this image are monolithic; list no modules here.
EOF
	write_root_file "$root/etc/network/interfaces" 0644 <<'EOF'
auto lo
iface lo inet loopback

auto eth0
iface eth0 inet dhcp
EOF
	write_root_file "$root/etc/hostname" 0644 <<'EOF'
alpine
EOF
	write_root_file "$root/etc/fstab" 0644 <<'EOF'
/dev/ubda1 / ext4 defaults,noatime 0 1
EOF
	write_root_file "$root/sbin/autologin-root" 0755 <<'EOF'
#!/bin/sh
exec /bin/login -f root
EOF
	write_root_file "$root/etc/inittab" 0644 <<'EOF'
::sysinit:/sbin/openrc sysinit
::sysinit:/sbin/openrc boot
::wait:/sbin/openrc default
hvc0::respawn:/sbin/getty -n -l /sbin/autologin-root 115200 hvc0 vt100
::shutdown:/sbin/openrc shutdown
EOF
	if ! as_root grep -qx hvc0 "$root/etc/securetty"; then
		printf '%s\n' hvc0 | as_root tee -a "$root/etc/securetty" >/dev/null
	fi
	as_root chroot "$root" /usr/bin/passwd -d root
	as_root rm -rf -- "$root/var/cache/apk"/* "$root/tmp"/* "$root/var/tmp"/*
	: > "$work_dir/alpine.packages.tsv"
	as_root chroot "$root" /sbin/apk info -v | sort > "$work_dir/alpine.packages.tsv"
}

build_alpine()
{
	printf '%s\n' "==> Building Alpine $ALPINE_VERSION minimal GPT image"
	archive=$sources_dir/alpine-minirootfs-$ALPINE_VERSION-aarch64.tar.gz
	download_checked \
		"https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/aarch64/$(basename "$archive")" \
		"$ALPINE_SHA256" "$archive"
	root=$roots_dir/alpine
	reset_root_directory "$root"
	as_root tar --numeric-owner -C "$root" -xzf "$archive"
	configure_alpine "$root"
	destination=$(artifact_path alpine-$ALPINE_VERSION-minimal.img)
	as_root python3 "$SCRIPT_DIR/make-gpt-image.py" \
		--root "$root" --output "$destination" --label alpine \
		--headroom-mib 64
	as_root chown "$owner_uid:$owner_gid" "$destination"
	cp "$work_dir/alpine.packages.tsv" "$destination.packages.tsv"
}

configure_debian_base()
{
	root=$1
	hostname=$2
	autologin_user=$3
	write_root_file "$root/etc/apt/sources.list" 0644 <<EOF
deb http://deb.debian.org/debian $DEBIAN_SUITE main
deb http://deb.debian.org/debian $DEBIAN_SUITE-updates main
deb http://security.debian.org/debian-security $DEBIAN_SUITE-security main
EOF
	write_root_file "$root/usr/sbin/policy-rc.d" 0755 <<'EOF'
#!/bin/sh
exit 101
EOF
	as_root cp -L /etc/resolv.conf "$root/etc/resolv.conf"
	as_root chroot "$root" /usr/bin/env DEBIAN_FRONTEND=noninteractive \
		apt-get update
	# systemd loads libbpf at runtime for its cgroup BPF features and reports
	# them disabled at every boot when the library is absent.
	as_root chroot "$root" /usr/bin/env DEBIAN_FRONTEND=noninteractive \
		apt-get install -y --no-install-recommends \
		systemd-sysv systemd-resolved udev kmod dbus ca-certificates \
		libbpf1
	write_root_file "$root/etc/hostname" 0644 <<EOF
$hostname
EOF
	write_root_file "$root/etc/hosts" 0644 <<EOF
127.0.0.1 localhost
127.0.1.1 $hostname
::1 localhost ip6-localhost ip6-loopback
EOF
	write_root_file "$root/etc/fstab" 0644 <<'EOF'
/dev/ubda1 / ext4 defaults,noatime 0 1
EOF
	# Debian's systemd links /etc/default/locale to this file at boot, and
	# pam_env reads it for every login. C.UTF-8 needs no locales package.
	write_root_file "$root/etc/locale.conf" 0644 <<'EOF'
LANG=C.UTF-8
EOF
	# Match wired links by type rather than by kernel-assigned name, and keep
	# the image's own hostname: the DHCP-supplied one could only be applied
	# through hostnamed, which refuses networkd without polkit.
	write_root_file "$root/etc/systemd/network/20-wired.network" 0644 <<'EOF'
[Match]
Type=ether
Kind=!*

[Network]
DHCP=yes
IPv6AcceptRA=yes

[DHCPv4]
UseDNS=yes
UseHostname=no

[DHCPv6]
UseHostname=no
EOF
	write_root_file \
		"$root/etc/systemd/system/serial-getty@hvc0.service.d/autologin.conf" \
		0644 <<EOF
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin $autologin_user --noclear - \$TERM
EOF
	as_root systemctl --root="$root" enable \
		systemd-networkd.service systemd-resolved.service \
		serial-getty@hvc0.service
	as_root ln -sfn /run/systemd/resolve/stub-resolv.conf "$root/etc/resolv.conf"
	as_root truncate -s 0 "$root/etc/machine-id"
}

prune_debian()
{
	root=$1
	as_root chroot "$root" apt-get clean
	as_root rm -f -- "$root/usr/sbin/policy-rc.d"
	as_root rm -rf -- "$root/var/lib/apt/lists"/* "$root/var/cache/apt"/* \
		"$root/tmp"/* "$root/var/tmp"/* "$root/run"/*
	as_root find "$root/var/log" -type f -exec truncate -s 0 {} \;
	as_root find "$root/usr/share/doc" -type f ! -name copyright -delete
	as_root find "$root/usr/share/doc" -type l -delete
	as_root find "$root/usr/share/doc" -depth -type d -empty -delete
	as_root rm -rf -- "$root/usr/share/man"/* "$root/usr/share/info"/* \
		"$root/usr/share/lintian" "$root/usr/share/doc-base"
	as_root find "$root/usr/share/locale" -mindepth 1 -maxdepth 1 \
		! -name 'en' ! -name 'en_*' -exec rm -rf -- {} +
}

debootstrap_root()
{
	root=$1
	reset_root_directory "$root"
	as_root debootstrap --arch=arm64 --variant=minbase --components=main \
		"$DEBIAN_SUITE" "$root" http://deb.debian.org/debian
}

build_debian_minimal()
{
	printf '%s\n' "==> Building Debian 13 minimal GPT image"
	root=$roots_dir/debian-minimal
	debootstrap_root "$root"
	mount_chroot_proc "$root"
	configure_debian_base "$root" debian root
	as_root chroot "$root" /usr/bin/passwd -d root
	as_root systemctl --root="$root" set-default multi-user.target
	as_root chroot "$root" dpkg-query -W -f='${binary:Package}\t${Version}\n' | \
		sort > "$work_dir/debian-minimal.packages.tsv"
	unmount_chroot_proc
	cat "$root/etc/debian_version" > "$work_dir/debian.version"
	prune_debian "$root"
	destination=$(artifact_path debian-13-minimal.img)
	as_root python3 "$SCRIPT_DIR/make-gpt-image.py" \
		--root "$root" --output "$destination" --label debian-min \
		--headroom-mib 96
	as_root chown "$owner_uid:$owner_gid" "$destination"
	cp "$work_dir/debian-minimal.packages.tsv" "$destination.packages.tsv"
}

configure_plasma_user()
{
	root=$1
	as_root chroot "$root" /usr/bin/env DEBIAN_FRONTEND=noninteractive \
		apt-get install -y --no-install-recommends \
		plasma-desktop konsole chromium sudo fonts-dejavu-core \
		libgl1-mesa-dri mesa-va-drivers vainfo ffmpeg \
		xdg-desktop-portal-kde
	as_root chroot "$root" useradd -m -s /bin/bash debian
	as_root chroot "$root" passwd -d debian
	debian_uid=$(as_root chroot "$root" id -u debian)
	debian_gid=$(as_root chroot "$root" id -g debian)
	for group in sudo video audio input render; do
		if as_root chroot "$root" getent group "$group" >/dev/null; then
			as_root chroot "$root" usermod -a -G "$group" debian
		fi
	done
	write_root_file "$root/etc/sudoers.d/debian" 0440 <<'EOF'
debian ALL=(ALL:ALL) NOPASSWD: ALL
EOF
	write_root_file \
		"$root/etc/systemd/system/getty@tty1.service.d/autologin.conf" \
		0644 <<'EOF'
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin debian --noclear %I $TERM
EOF
	write_root_file "$root/home/debian/.profile" 0644 <<'EOF'
# Start the image-owned Plasma Wayland session from the autologin VT.
if [ "${XDG_VTNR:-}" = 1 ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    exec /usr/bin/dbus-run-session -- /usr/bin/startplasma-wayland
fi
EOF
	as_root chown "$debian_uid:$debian_gid" "$root/home/debian/.profile"
	write_root_file "$root/home/debian/.config/kscreenlockerrc" 0644 <<'EOF'
[Daemon]
Autolock=false
LockOnResume=false
EOF
	as_root chown -R "$debian_uid:$debian_gid" "$root/home/debian/.config"
	as_root systemctl --root="$root" enable getty@tty1.service
	as_root systemctl --root="$root" set-default graphical.target
}

build_debian_plasma()
{
	printf '%s\n' "==> Building Debian 13 minimal Plasma Wayland GPT image"
	root=$roots_dir/debian-plasma
	debootstrap_root "$root"
	mount_chroot_proc "$root"
	configure_debian_base "$root" debian debian
	configure_plasma_user "$root"
	as_root chroot "$root" dpkg-query -W -f='${binary:Package}\t${Version}\n' | \
		sort > "$work_dir/debian-plasma.packages.tsv"
	unmount_chroot_proc
	prune_debian "$root"
	destination=$(artifact_path debian-13-plasma-wayland.img)
	as_root python3 "$SCRIPT_DIR/make-gpt-image.py" \
		--root "$root" --output "$destination" --label debian-kde \
		--headroom-mib 256
	as_root chown "$owner_uid:$owner_gid" "$destination"
	cp "$work_dir/debian-plasma.packages.tsv" "$destination.packages.tsv"
}

build_initramfs
build_alpine
build_debian_minimal
build_debian_plasma
python3 "$SCRIPT_DIR/write-manifest.py" \
	--output "$output_dir" --debian-version "$(cat "$work_dir/debian.version")"
mv -T -n -- "$output_dir" "$final_output_dir"
[ ! -e "$output_dir" ] || fail "output directory appeared during publication"
staging_output_dir=
trap - EXIT HUP INT TERM
printf '%s\n' "Built starter release: $final_output_dir"
