#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Resume image auditing and sealing after assembly has completed.
set -euo pipefail
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ "$#" -ne 3 ]; then
    printf '%s\n' 'Usage: seal-image.sh INPUTS ASSEMBLED_WORK ABSENT_OUTPUT.erofs' >&2
    exit 2
fi
inputs=$(realpath "$1")
work=$(realpath "$2")
output=$3
root=$work/root
case "$output" in /*/steam-arm64ec-*.erofs) ;; *) exit 2 ;; esac
for artifact in "$output" "$output.sha256" "$output.packages.tsv" "$output.binaries.tsv" "$output.provenance.json"; do
    [ ! -e "$artifact" ] && [ ! -L "$artifact" ]
done
[ ! -e "$work/rootfs.erofs" ]
test -z "$(sudo -n chroot "$root" dpkg --audit)"
test -z "$(sudo -n chroot "$root" dpkg --print-foreign-architectures)"
sudo -n python3 -B "$here/verify-image.py" --root "$root" --output "$work/binaries.tsv"
sudo -n chroot "$root" dpkg-query -W -f='${binary:Package}\t${Version}\n' | sort > "$work/packages.tsv"
sudo -n rm -f "$root/usr/sbin/policy-rc.d"
sudo -n find "$root/var/log" -type f -exec truncate -s 0 '{}' +
sudo -n mkfs.erofs --quiet -L uml-steam-ec -zzstd,level=3 -C1048576 "$work/rootfs.erofs" "$root"
sudo -n chown "$(id -u):$(id -g)" "$work/rootfs.erofs" "$work/binaries.tsv"
python3 -B "$here/provenance.py" --inputs "$inputs" --root "$root" --output "$work/provenance.json"
mv -n "$work/rootfs.erofs" "$output"
cp "$work/packages.tsv" "$output.packages.tsv"
cp "$work/binaries.tsv" "$output.binaries.tsv"
cp "$work/provenance.json" "$output.provenance.json"
(cd "$(dirname "$output")" && sha256sum "$(basename "$output")") > "$output.sha256"
printf '%s\n' "Built Steam ARM64EC gaming rootfs: $output"
