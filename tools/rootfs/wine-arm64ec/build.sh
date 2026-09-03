#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build the ARM64-only XFCE root with ARM64EC Wine and PE-only i386 WoW64.
set -euo pipefail

PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export PATH

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPOSITORY=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
DEBIAN_SUITE=trixie
WINE_COMMIT=b073859675060c9211fcbccfd90e4e87520dc2c2
TOOLCHAIN_NAME=llvm-mingw-arm64ec
TOOLCHAIN_VERSION=20250920

fail()
{
	printf '%s\n' "tools/rootfs/wine-arm64ec/build.sh: $*" >&2
	exit 1
}

usage()
{
	cat <<'EOF'
Usage: tools/rootfs/wine-arm64ec/build.sh \
  --work PATH --output PATH.erofs --wine-source PATH --toolchain PATH [--jobs N]

Run as an ordinary user with passwordless sudo on native AArch64 Debian Linux.
All paths must be absolute. Work and output paths must be distinct and absent.
Wine source must be the prepared pristine Wine 10 commit; toolchain must be the
checksum-pinned llvm-mingw ARM64EC toolchain prepared by dependencies/prepare.sh.
EOF
}

work_dir=
output=
wine_source=
toolchain=
jobs=2
while [ "$#" -gt 0 ]; do
	case "$1" in
	--work) [ "$#" -ge 2 ] || fail "--work requires a path"; work_dir=$2; shift 2 ;;
	--output) [ "$#" -ge 2 ] || fail "--output requires a path"; output=$2; shift 2 ;;
	--wine-source) [ "$#" -ge 2 ] || fail "--wine-source requires a path"; wine_source=$2; shift 2 ;;
	--toolchain) [ "$#" -ge 2 ] || fail "--toolchain requires a path"; toolchain=$2; shift 2 ;;
	--jobs) [ "$#" -ge 2 ] || fail "--jobs requires a count"; jobs=$2; shift 2 ;;
	-h|--help) usage; exit 0 ;;
	*) fail "unknown option: $1" ;;
	esac
done

[ -n "$work_dir" ] && [ -n "$output" ] && [ -n "$wine_source" ] && \
	[ -n "$toolchain" ] || { usage >&2; exit 2; }
case "$work_dir:$output:$wine_source:$toolchain" in /*:/*:/*:/*) ;; *) fail "all paths must be absolute" ;; esac
case "$jobs" in ''|*[!0-9]*|0) fail "--jobs must be a positive integer" ;; esac
[ "$work_dir" != "$output" ] || fail "work and output paths must differ"
case "$work_dir" in /|/home|/tmp|/usr|/var) fail "unsafe work path: $work_dir" ;; esac
case "$output" in /|/home|/tmp|/usr|/var) fail "unsafe output path: $output" ;; esac
case "$output" in *.erofs) ;; *) fail "--output must end in .erofs" ;; esac
[ ! -e "$work_dir" ] && [ ! -L "$work_dir" ] || fail "refusing to replace work path: $work_dir"
for artifact in "$output" "$output.packages.tsv" "$output.elf.tsv" \
	"$output.pe.tsv" "$output.provenance.tsv" "$output.sha256"; do
	[ ! -e "$artifact" ] && [ ! -L "$artifact" ] || fail "refusing to replace output: $artifact"
done
[ "$(uname -s)" = Linux ] || fail "the producer requires Linux"
[ "$(uname -m)" = aarch64 ] || fail "the producer requires native AArch64"
[ "$(id -u)" -ne 0 ] || fail "run as an ordinary user; sudo is used narrowly"
sudo -n true 2>/dev/null || fail "passwordless sudo is required"
for command in chroot debootstrap git make mkfs.erofs python3 sha256sum sort sudo; do
	command -v "$command" >/dev/null 2>&1 || fail "required command is unavailable: $command"
done
[ "$(git -C "$wine_source" rev-parse --is-inside-work-tree 2>/dev/null)" = true ] || \
	fail "Wine source is not a Git checkout: $wine_source"
[ "$(git -C "$wine_source" rev-parse HEAD)" = "$WINE_COMMIT" ] || fail "Wine source commit differs"
[ -z "$(git -C "$wine_source" status --porcelain --untracked-files=no)" ] || fail "Wine source has tracked changes"
[ -x "$toolchain/bin/clang" ] || fail "toolchain is missing bin/clang"
[ -x "$toolchain/bin/x86_64-w64-mingw32-clang" ] || fail "toolchain is missing the AMD64 compiler"
[ -x "$toolchain/bin/i686-w64-mingw32-clang" ] || fail "toolchain is missing the i386 compiler"
receipt=$toolchain/.uml-toolchain-receipt
[ -r "$receipt" ] || fail "toolchain receipt is absent"
grep -qx "name=$TOOLCHAIN_NAME" "$receipt" || fail "toolchain name differs"
grep -qx "version=$TOOLCHAIN_VERSION" "$receipt" || fail "toolchain version differs"

output_parent=$(dirname -- "$output")
output_name=$(basename -- "$output")
case "$output_name" in ''|.|..) fail "unsafe output name: $output_name" ;; esac
case "$output_name" in
wine-arm64ec-*.erofs) ;;
*) fail "the image name must use the reserved wine-arm64ec-*.erofs form" ;;
esac
mkdir -p -- "$output_parent"
output_parent=$(CDPATH= cd -- "$output_parent" && pwd -P)
output=$output_parent/$output_name
mkdir -p -- "$work_dir"
work_dir=$(CDPATH= cd -- "$work_dir" && pwd -P)
root=$work_dir/root
source=$work_dir/wine-source
build=$work_dir/wine-build
mkdir -p -- "$root" "$build"

owner_uid=$(id -u)
owner_gid=$(id -g)
staging=$(mktemp "$output_parent/.$output_name.partial.XXXXXX")
rm -f -- "$staging"
cleanup()
{
	[ -n "$staging" ] || return
	sudo rm -f -- "$staging" "$staging.packages.tsv" "$staging.elf.tsv" \
		"$staging.pe.tsv" "$staging.provenance.tsv" "$staging.sha256"
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

printf '%s\n' '==> Preparing the immutable Wine build overlay'
cp -a -- "$wine_source" "$source"
python3 "$SCRIPT_DIR/enable-wow64-bridge.py" --source "$source"
install -m 0644 "$SCRIPT_DIR/bridge/Makefile.in" "$source/dlls/xtajit64/Makefile.in"
install -m 0644 "$SCRIPT_DIR/bridge/cpu.c" "$source/dlls/xtajit64/cpu.c"
install -m 0644 "$SCRIPT_DIR/bridge/entry.S" "$source/dlls/xtajit64/entry.S"
install -m 0644 "$REPOSITORY/darwin/runtime/wine_cpu_protocol.h" \
	"$source/dlls/xtajit64/wine_cpu_protocol.h"
install -D -m 0644 "$SCRIPT_DIR/wow64-bridge/Makefile.in" \
	"$source/dlls/xtajit/Makefile.in"
install -m 0644 "$SCRIPT_DIR/wow64-bridge/xtajit.spec" \
	"$source/dlls/xtajit/xtajit.spec"
install -m 0644 "$SCRIPT_DIR/wow64-bridge/cpu.c" \
	"$source/dlls/xtajit/cpu.c"
install -m 0644 "$SCRIPT_DIR/wow64-bridge/entry.S" \
	"$source/dlls/xtajit/entry.S"
install -m 0644 "$REPOSITORY/darwin/runtime/wine_cpu_protocol.h" \
	"$source/dlls/xtajit/wine_cpu_protocol.h"

printf '%s\n' '==> Building ARM64EC/ARM64X Wine'
(
	cd "$build"
	env PATH="$toolchain/bin:$PATH" "$source/configure" \
		--prefix=/opt/wine-arm64ec \
		--enable-archs=i386,arm64ec,aarch64 \
		--disable-tests \
		--disable-wdscore \
		--with-mingw="$toolchain/bin/clang"
)
env PATH="$toolchain/bin:$PATH" make -C "$build" -j "$jobs"

printf '%s\n' '==> Building the AMD64 and i386 Windows execution probes'
"$toolchain/bin/x86_64-w64-mingw32-clang" -O2 \
	"$SCRIPT_DIR/smoke/x86_64_exit_42.S" \
	-o "$work_dir/x86_64-exit-42.exe"
"$toolchain/bin/i686-w64-mingw32-clang" -O2 \
	"$SCRIPT_DIR/smoke/i386_exit_43.S" \
	-o "$work_dir/i386-exit-43.exe"

printf '%s\n' '==> Bootstrapping Debian 13 arm64'
sudo debootstrap --arch=arm64 --variant=minbase \
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

printf '%s\n' '==> Installing the native ARM64 XFCE and Wine runtime closure'
sudo chroot "$root" /usr/bin/env DEBIAN_FRONTEND=noninteractive apt-get update
sudo chroot "$root" /usr/bin/env DEBIAN_FRONTEND=noninteractive \
	apt-get install -y --no-install-recommends \
	systemd-sysv systemd-resolved udev kmod dbus dbus-user-session \
	polkitd pkexec ca-certificates sudo procps util-linux e2fsprogs iproute2 \
	iputils-ping netbase locales \
	lightdm lightdm-gtk-greeter xfce4-session xfce4-panel xfce4-settings \
	xfwm4 xfdesktop4 thunar xfce4-terminal \
	xserver-xorg-core xserver-xorg-video-modesetting xauth xvfb \
	xserver-xorg-input-libinput \
	x11-xserver-utils x11-utils libgl1-mesa-dri mesa-utils fonts-dejavu-core \
	libasound2t64 libcups2t64 libdbus-1-3 libfontconfig1 libfreetype6 \
	libgl1 libgnutls30t64 libpulse0 libudev1 libunwind8 \
	libx11-6 libxcomposite1 libxcursor1 libxext6 libxfixes3 libxi6 \
	libxinerama1 libxrandr2 libxrender1 libxxf86vm1 \
	libwayland-client0 libwayland-egl1 libxkbcommon0 \
	libavcodec61 libavformat61 libavutil59
[ -z "$(sudo chroot "$root" dpkg --print-foreign-architectures)" ] || fail "rootfs gained a foreign Debian architecture"

printf '%s\n' '==> Installing the source-built Wine appliance'
sudo env PATH="$toolchain/bin:$PATH" make -C "$build" \
	DESTDIR="$root" install
sudo install -D -m 0755 "$work_dir/x86_64-exit-42.exe" \
	"$root/opt/uml-wine-smoke/x86_64-exit-42.exe"
sudo install -m 0755 "$work_dir/i386-exit-43.exe" \
	"$root/opt/uml-wine-smoke/i386-exit-43.exe"
sudo ln -s /opt/wine-arm64ec/bin/wine "$root/usr/local/bin/wine"

printf '%s\n' '==> Configuring the XFCE appliance'
write_root_file "$root/etc/hostname" 0644 <<'EOF'
uml-wine
EOF
write_root_file "$root/etc/hosts" 0644 <<'EOF'
127.0.0.1 localhost
127.0.1.1 uml-wine
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
EOF
write_root_file "$root/etc/lightdm/lightdm.conf.d/50-uml-autologin.conf" 0644 <<'EOF'
[Seat:*]
autologin-user=uml
autologin-user-timeout=0
user-session=xfce
EOF
write_root_file \
	"$root/etc/systemd/system/serial-getty@ttyS0.service.d/autologin.conf" \
	0644 <<'EOF'
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin uml --noclear %I $TERM
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
write_root_file "$root/home/uml/.config/xfce4/xfconf/xfce-perchannel-xml/xfce4-power-manager.xml" 0644 <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<channel name="xfce4-power-manager" version="1.0">
  <property name="xfce4-power-manager" type="empty">
    <property name="lock-screen-suspend-hibernate" type="bool" value="false"/>
  </property>
</channel>
EOF
sudo chown -R "$uml_uid:$uml_gid" "$root/home/uml/.config"
sudo install -d -m 0755 -o "$uml_uid" -g "$uml_gid" \
	"$root/home/uml/Desktop" "$root/home/uml/Documents" \
	"$root/home/uml/Downloads"
write_root_file "$root/etc/locale.gen" 0644 <<'EOF'
en_US.UTF-8 UTF-8
EOF
write_root_file "$root/etc/default/locale" 0644 <<'EOF'
LANG=en_US.UTF-8
EOF
sudo chroot "$root" locale-gen en_US.UTF-8
sudo systemctl --root="$root" enable \
	systemd-networkd.service systemd-resolved.service lightdm.service
sudo systemctl --root="$root" set-default graphical.target
sudo ln -sfn /run/systemd/resolve/stub-resolv.conf "$root/etc/resolv.conf"
sudo truncate -s 0 "$root/etc/machine-id"

printf '%s\n' '==> Auditing package, executable, and architecture state'
package_audit=$(sudo chroot "$root" dpkg --audit)
[ -z "$package_audit" ] || fail "dpkg --audit reported incomplete packages: $package_audit"
sudo chroot "$root" dpkg-query -W -f='${binary:Package}\t${Version}\n' | sort > "$staging.packages.tsv"
sudo chroot "$root" /usr/bin/env \
	WINEDLLPATH=/opt/wine-arm64ec/lib/wine \
	/opt/wine-arm64ec/bin/wine --version | grep -q '^wine-10\.0'
sudo python3 "$SCRIPT_DIR/verify-elf.py" --root "$root" --output "$staging.elf.tsv"
sudo python3 "$SCRIPT_DIR/verify-pe.py" --root "$root" --output "$staging.pe.tsv"
sudo chown "$owner_uid:$owner_gid" "$staging.elf.tsv" "$staging.pe.tsv"

toolchain_sha=$(sha256sum "$receipt" | awk '{print $1}')
bridge_sha=$(sha256sum \
	"$SCRIPT_DIR/bridge/Makefile.in" \
	"$SCRIPT_DIR/bridge/cpu.c" \
	"$SCRIPT_DIR/bridge/entry.S" \
	"$SCRIPT_DIR/wow64-bridge/Makefile.in" \
	"$SCRIPT_DIR/wow64-bridge/xtajit.spec" \
	"$SCRIPT_DIR/wow64-bridge/cpu.c" \
	"$SCRIPT_DIR/wow64-bridge/entry.S" \
	"$SCRIPT_DIR/enable-wow64-bridge.py" \
	"$REPOSITORY/darwin/runtime/wine_cpu_protocol.h" | awk '{print $1}' | \
	sha256sum | awk '{print $1}')
printf '%s\t%s\n' \
	component identity \
	debian_suite "$DEBIAN_SUITE" \
	debian_architecture arm64 \
	wine_commit "$WINE_COMMIT" \
	wine_architectures i386,arm64ec,aarch64 \
	toolchain "$TOOLCHAIN_NAME-$TOOLCHAIN_VERSION" \
	toolchain_receipt_sha256 "$toolchain_sha" \
	bridge_sha256 "$bridge_sha" \
	> "$staging.provenance.tsv"

sudo chroot "$root" apt-get clean
sudo rm -f -- "$root/usr/sbin/policy-rc.d"
sudo rm -rf -- "$root/var/lib/apt/lists"/* "$root/var/cache/apt"/* \
	"$root/tmp"/* "$root/var/tmp"/* "$root/run"/*
sudo find "$root/var/log" -type f -exec truncate -s 0 {} \;

printf '%s\n' '==> Building EROFS'
sudo mkfs.erofs --quiet -L uml-wine-ec -zzstd,level=3 -C1048576 \
	"$staging" "$root"
sudo chown "$owner_uid:$owner_gid" "$staging"
checksum=$(sha256sum "$staging" | awk '{print $1}')
printf '%s  %s\n' "$checksum" "$output_name" > "$staging.sha256"

mv -n -- "$staging" "$output"
mv -n -- "$staging.packages.tsv" "$output.packages.tsv"
mv -n -- "$staging.elf.tsv" "$output.elf.tsv"
mv -n -- "$staging.pe.tsv" "$output.pe.tsv"
mv -n -- "$staging.provenance.tsv" "$output.provenance.tsv"
mv -n -- "$staging.sha256" "$output.sha256"
staging=
trap - EXIT HUP INT TERM
printf '%s\n' "Built ARM64EC Wine XFCE rootfs: $output"
