#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Seal a gaming-only Steam root from prepared inputs and source-built Proton.
set -euo pipefail
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository=$(CDPATH= cd -- "$here/../../.." && pwd)
usage()
{
	printf '%s\n' 'Usage: build.sh --inputs PATH --proton PATH --work ABSENT_PATH --output ABSENT_PATH.erofs'
}
inputs= proton= work= output=
while [ "$#" -gt 0 ]; do
	case "$1" in
	--inputs) inputs=$2; shift 2 ;;
	--proton) proton=$2; shift 2 ;;
	--work) work=$2; shift 2 ;;
	--output) output=$2; shift 2 ;;
	-h|--help) usage; exit 0 ;;
	*) usage >&2; exit 2 ;;
	esac
done
case "$inputs:$proton:$work:$output" in /*:/*:/*:/*.erofs) ;; *) usage >&2; exit 2 ;; esac
case "$(basename "$output")" in steam-arm64ec-*.erofs) ;; *) exit 2 ;; esac
[ "$(uname -s)/$(uname -m)" = Linux/aarch64 ]
[ "$(id -u)" -ne 0 ]
sudo -n true
[ ! -e "$work" ] && [ ! -L "$work" ]
for artifact in "$output" "$output.sha256" "$output.packages.tsv" "$output.binaries.tsv" "$output.provenance.json"; do
	[ ! -e "$artifact" ] && [ ! -L "$artifact" ]
done
inputs=$(realpath "$inputs")
proton=$(realpath "$proton")
[ -x "$proton/proton" ] && [ -x "$proton/files/bin-arm64/wine" ]
"$repository/dependencies/prepare.sh" --product steam-arm64ec-rootfs --root "$inputs" --verify
mkdir -p "$work/root" "$(dirname "$output")"
work=$(realpath "$work")
root=$work/root
sudo -n tar -xf "$inputs/base.tar.zst" -C "$root"
sudo -n cp -a "$here/overlay/." "$root/"
sudo -n install -d "$root/opt"
sudo -n cp -a "$proton" "$root/opt/proton-uml-arm64ec"
sudo -n chroot "$root" useradd -m -s /bin/bash uml
sudo -n chroot "$root" passwd -d uml
for group in sudo video audio input render; do
	if sudo -n chroot "$root" getent group "$group" >/dev/null; then
		sudo -n chroot "$root" usermod -a -G "$group" uml
	fi
done
sudo -n python3 -B "$here/assemble-steam.py" --inputs "$inputs" \
	--steam "$root/home/uml/.local/share/Steam"
sudo -n mkdir -p "$root/home/uml/.steam" "$root/home/uml/.local/share/Steam/compatibilitytools.d"
for name in steam root; do
	sudo -n ln -s ../.local/share/Steam "$root/home/uml/.steam/$name"
done
sudo -n ln -s ../.local/share/Steam/linuxarm64 "$root/home/uml/.steam/sdkarm64"
sudo -n ln -s /opt/proton-uml-arm64ec "$root/home/uml/.local/share/Steam/compatibilitytools.d/proton-uml-arm64ec"
sudo -n chown -R 1000:1000 "$root/home/uml" "$root/opt/proton-uml-arm64ec"

write_file()
{
	destination=$1
	mode=$2
	temporary=$(mktemp "$work/.config.XXXXXX")
	cat > "$temporary"
	sudo -n install -D -m "$mode" "$temporary" "$root/$destination"
	rm "$temporary"
}
write_file etc/hostname 0644 <<'EOF'
uml-steam
EOF
write_file etc/hosts 0644 <<'EOF'
127.0.0.1 localhost
127.0.1.1 uml-steam
::1 localhost ip6-localhost ip6-loopback
EOF
write_file etc/fstab 0644 <<'EOF'
none /tmp tmpfs mode=1777,nosuid,nodev 0 0
EOF
write_file etc/sudoers.d/uml 0440 <<'EOF'
uml ALL=(ALL:ALL) NOPASSWD: ALL
EOF
write_file etc/systemd/network/20-wired.network 0644 <<'EOF'
[Match]
Name=eth* en*
[Network]
DHCP=yes
IPv6AcceptRA=yes
EOF
write_file etc/systemd/system/serial-getty@ttyS0.service.d/autologin.conf 0644 <<'EOF'
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin uml --noclear %I $TERM
EOF
write_file etc/locale.gen 0644 <<'EOF'
en_US.UTF-8 UTF-8
EOF
write_file etc/default/locale 0644 <<'EOF'
LANG=en_US.UTF-8
EOF
write_file usr/local/bin/wine 0755 <<'EOF'
#!/bin/sh
export LD_LIBRARY_PATH=/opt/proton-uml-arm64ec/files/lib/aarch64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
export WINESERVER=/opt/proton-uml-arm64ec/files/bin-arm64/wineserver
exec /opt/proton-uml-arm64ec/files/bin-arm64/wine "$@"
EOF
sudo -n chroot "$root" locale-gen en_US.UTF-8
sudo -n systemctl --root="$root" enable systemd-networkd systemd-resolved steam-gaming
sudo -n systemctl --root="$root" set-default graphical.target
sudo -n ln -sfn /run/systemd/resolve/stub-resolv.conf "$root/etc/resolv.conf"
sudo -n truncate -s 0 "$root/etc/machine-id"

sdk=$(python3 -B -c 'import json,sys; print(json.load(open(sys.argv[1]))["sdk"])' "$inputs/inputs.json")
"$here/offline-podman.sh" run --rm -u "$(id -u):$(id -g)" -e CCACHE_DISABLE=1 \
	-v "$repository:$repository:ro" -v "$work:$work" "$sdk" \
	x86_64-w64-mingw32-gcc -O2 "$repository/tools/rootfs/wine-arm64ec/smoke/x86_64_exit_42.S" -o "$work/x86_64-exit-42.exe"
"$here/offline-podman.sh" run --rm -u "$(id -u):$(id -g)" -e CCACHE_DISABLE=1 \
	-v "$repository:$repository:ro" -v "$work:$work" "$sdk" \
	i686-w64-mingw32-gcc -O2 "$repository/tools/rootfs/wine-arm64ec/smoke/i386_exit_43.S" -o "$work/i386-exit-43.exe"
sudo -n install -d "$root/opt/uml-wine-smoke"
sudo -n install -m 0755 "$work/"*-exit-*.exe "$root/opt/uml-wine-smoke/"
"$here/seal-image.sh" "$inputs" "$work" "$output"
