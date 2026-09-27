#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build the x86-family desktop qualification root on native x86-64 Linux.
set -euo pipefail

PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export PATH

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEBIAN_SUITE=trixie

fail()
{
	printf '%s\n' "tools/rootfs/x86_64-desktop/build.sh: $*" >&2
	exit 1
}

usage()
{
	cat <<'EOF'
Usage: tools/rootfs/x86_64-desktop/build.sh --work PATH --output PATH.erofs

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
for command in chroot debootstrap mkfs.erofs python3 sha256sum sort sudo; do
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
staging=$(mktemp "$output_parent/.$output_name.partial.XXXXXX")
rm -f -- "$staging"
cleanup()
{
	[ -n "$staging" ] || return
	sudo rm -f -- "$staging" "$staging.packages.tsv" \
		"$staging.elf.tsv" "$staging.sha256"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

write_root_file()
{
	destination=$1
	mode=$2
	temporary=$(mktemp "$work_dir/.root-file.XXXXXX")
	cat > "$temporary"
	sudo install -D -m "$mode" "$temporary" "$destination"
	rm -f -- "$temporary"
}

printf '%s\n' '==> Bootstrapping Debian 13 amd64'
sudo debootstrap \
	--arch=amd64 \
	--variant=minbase \
	--components=main,contrib,non-free,non-free-firmware \
	"$DEBIAN_SUITE" "$root" http://deb.debian.org/debian

write_root_file "$root/etc/apt/sources.list" 0644 <<EOF
deb http://deb.debian.org/debian $DEBIAN_SUITE main contrib non-free non-free-firmware
deb http://deb.debian.org/debian $DEBIAN_SUITE-updates main contrib non-free non-free-firmware
deb http://security.debian.org/debian-security $DEBIAN_SUITE-security main contrib non-free non-free-firmware
EOF
write_root_file "$root/usr/sbin/policy-rc.d" 0755 <<'EOF'
#!/bin/sh
exit 101
EOF
sudo cp -L /etc/resolv.conf "$root/etc/resolv.conf"
sudo chroot "$root" dpkg --add-architecture i386

printf '%s\n' \
	'==> Installing the x86-family Plasma, Wayland, and Wine userspace'
sudo chroot "$root" /usr/bin/env DEBIAN_FRONTEND=noninteractive \
	apt-get update
sudo chroot "$root" /usr/bin/env DEBIAN_FRONTEND=noninteractive \
	apt-get install -y --no-install-recommends \
	systemd-sysv systemd-resolved udev kmod dbus dbus-user-session \
	polkitd pkexec ca-certificates sudo procps util-linux iproute2 \
	iputils-ping netbase locales \
	plasma-desktop plasma-workspace kwin-wayland sddm sddm-theme-breeze \
	konsole xwayland xserver-xorg-core xserver-xorg-video-modesetting xauth \
	libgl1-mesa-dri mesa-utils xdg-desktop-portal-kde fonts-dejavu-core \
	wine wine64 wine64-preloader wine32:i386 libwine:amd64 libwine:i386 xvfb \
	curl wget xz-utils file less lsof xdg-user-dirs zenity

printf '%s\n' '==> Adding the recommended amd64 and i386 Steam runtime (no ARM packages)'
printf '%s\n' \
	'steam steam/question select I AGREE' \
	'steam steam/license note' |
	sudo chroot "$root" debconf-set-selections
sudo chroot "$root" /usr/bin/env DEBIAN_FRONTEND=noninteractive \
	apt-get install -y --install-recommends \
	steam-installer steam-libs:amd64 steam-libs:i386 steam-libs-i386:i386

printf '%s\n' '==> Configuring the qualification desktop'
write_root_file "$root/etc/hostname" 0644 <<'EOF'
uml-x86
EOF
write_root_file "$root/etc/hosts" 0644 <<'EOF'
127.0.0.1 localhost
127.0.1.1 uml-x86
::1 localhost ip6-localhost ip6-loopback
EOF
write_root_file "$root/etc/fstab" 0644 <<'EOF'
none /tmp tmpfs mode=1777,nosuid,nodev 0 0
EOF
write_root_file "$root/etc/systemd/network/20-wired.network" 0644 <<'EOF'
[Match]
Name=eth* en*

[Network]
DHCP=yes
IPv6AcceptRA=yes

[DHCPv4]
UseDNS=yes
EOF
write_root_file "$root/etc/sddm.conf.d/uml-autologin.conf" 0644 <<'EOF'
[Autologin]
User=uml
Session=plasma
Relogin=false
EOF

sudo chroot "$root" useradd -m -s /bin/bash uml
sudo chroot "$root" passwd -d uml
uml_uid=$(sudo chroot "$root" id -u uml)
uml_gid=$(sudo chroot "$root" id -g uml)
for group in sudo video audio input render; do
	if sudo chroot "$root" getent group "$group" >/dev/null; then
		sudo chroot "$root" usermod -a -G "$group" uml
	fi
done
write_root_file "$root/etc/sudoers.d/uml" 0440 <<'EOF'
uml ALL=(ALL:ALL) NOPASSWD: ALL
EOF
write_root_file "$root/home/uml/.config/kscreenlockerrc" 0644 <<'EOF'
[Daemon]
Autolock=false
LockOnResume=false
Timeout=0
EOF
write_root_file "$root/home/uml/.config/user-dirs.dirs" 0644 <<'EOF'
XDG_DESKTOP_DIR="$HOME/Desktop"
XDG_DOWNLOAD_DIR="$HOME/Downloads"
XDG_TEMPLATES_DIR="$HOME/Templates"
XDG_PUBLICSHARE_DIR="$HOME/Public"
XDG_DOCUMENTS_DIR="$HOME/Documents"
XDG_MUSIC_DIR="$HOME/Music"
XDG_PICTURES_DIR="$HOME/Pictures"
XDG_VIDEOS_DIR="$HOME/Videos"
EOF
sudo chown -R "$uml_uid:$uml_gid" "$root/home/uml/.config"
for directory in Desktop Documents Downloads Music Pictures Public Templates Videos; do
	sudo install -d -m 0755 -o "$uml_uid" -g "$uml_gid" \
		"$root/home/uml/$directory"
done
sudo install -d -m 0755 -o "$uml_uid" -g "$uml_gid" \
	"$root/home/uml/.local" \
	"$root/home/uml/.local/share" \
	"$root/home/uml/.local/share/sddm"
write_root_file "$root/etc/locale.gen" 0644 <<'EOF'
en_US.UTF-8 UTF-8
EOF
write_root_file "$root/etc/default/locale" 0644 <<'EOF'
LANG=en_US.UTF-8
EOF
sudo chroot "$root" locale-gen en_US.UTF-8
sudo systemctl --root="$root" enable \
	systemd-networkd.service systemd-resolved.service sddm.service
sudo systemctl --root="$root" set-default graphical.target
sudo ln -sfn /run/systemd/resolve/stub-resolv.conf "$root/etc/resolv.conf"
sudo truncate -s 0 "$root/etc/machine-id"

printf '%s\n' '==> Auditing the installed package state'
package_audit=$(sudo chroot "$root" dpkg --audit)
[ -z "$package_audit" ] || fail "dpkg --audit reported incomplete packages:
$package_audit"

sudo chroot "$root" dpkg-query -W -f='${binary:Package}\t${Version}\n' |
	sort > "$staging.packages.tsv"

printf '%s\n' '==> Verifying that every ELF payload is x86-family'
sudo python3 "$SCRIPT_DIR/verify-elf.py" \
	--root "$root" --output "$staging.elf.tsv"
sudo chown "$owner_uid:$owner_gid" "$staging.elf.tsv"

sudo chroot "$root" apt-get clean
sudo rm -f -- "$root/usr/sbin/policy-rc.d"
sudo rm -rf -- "$root/var/lib/apt/lists"/* "$root/var/cache/apt"/* \
	"$root/tmp"/* "$root/var/tmp"/* "$root/run"/*
sudo find "$root/var/log" -type f -exec truncate -s 0 {} \;

printf '%s\n' '==> Building EROFS'
# Match the zstd/big-pcluster profile used by the accepted desktop rootfs.
sudo mkfs.erofs -L uml-x86-desktop -zzstd,level=3 -C1048576 \
	"$staging" "$root"
sudo chown "$owner_uid:$owner_gid" "$staging"
checksum=$(sha256sum "$staging" | awk '{print $1}')
printf '%s  %s\n' "$checksum" "$output_name" > "$staging.sha256"

mv -n -- "$staging" "$output"
mv -n -- "$staging.packages.tsv" "$output.packages.tsv"
mv -n -- "$staging.elf.tsv" "$output.elf.tsv"
mv -n -- "$staging.sha256" "$output.sha256"
staging=
trap - EXIT HUP INT TERM
printf '%s\n' "Built x86-family desktop rootfs: $output"
