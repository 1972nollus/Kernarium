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
case "$output" in /*/steam-x86_64-*.erofs) ;; *) exit 2 ;; esac
for artifact in "$output" "$output.sha256" "$output.packages.tsv" "$output.binaries.tsv" "$output.provenance.json"; do
    [ ! -e "$artifact" ] && [ ! -L "$artifact" ]
done
mkdir -p "$(dirname "$output")"
# Every artifact is written into a directory this user owns beside the output
# and is complete before the first one is published. Each rename must take its
# own absent name, so a lost race fails instead of pairing these sidecars with
# another run's image.
stage=$(mktemp -d "$(dirname "$output")/.$(basename "$output").partial.XXXXXX")
trap 'sudo -n rm -rf -- "$stage"' EXIT
trap 'exit 1' HUP INT TERM
test -z "$(sudo -n chroot "$root" dpkg --audit)"
test -z "$(sudo -n chroot "$root" dpkg --print-foreign-architectures)"
sudo -n python3 -B "$here/verify-image.py" --root "$root" --output "$stage/binaries.tsv"
sudo -n chroot "$root" dpkg-query -W -f='${binary:Package}\t${Version}\n' | sort > "$stage/packages.tsv"
sudo -n rm -f "$root/usr/sbin/policy-rc.d"
sudo -n find "$root/var/log" -type f -exec truncate -s 0 '{}' +
sudo -n mkfs.erofs --quiet -L uml-steam-x86 -zzstd,level=3 -C1048576 "$stage/rootfs.erofs" "$root"
sudo -n chown "$(id -u):$(id -g)" "$stage/rootfs.erofs" "$stage/binaries.tsv"
python3 -B "$here/provenance.py" --inputs "$inputs" --root "$root" --output "$stage/provenance.json"
checksum=$(sha256sum "$stage/rootfs.erofs" | awk '{print $1}')
printf '%s  %s\n' "$checksum" "$(basename "$output")" > "$stage/rootfs.erofs.sha256"
publish()
{
    mv -T -n -- "$1" "$2"
    if [ -e "$1" ]; then
        printf '%s\n' "seal-image.sh: output appeared during publication: $2" >&2
        exit 1
    fi
}
publish "$stage/rootfs.erofs" "$output"
publish "$stage/packages.tsv" "$output.packages.tsv"
publish "$stage/binaries.tsv" "$output.binaries.tsv"
publish "$stage/provenance.json" "$output.provenance.json"
publish "$stage/rootfs.erofs.sha256" "$output.sha256"
printf '%s\n' "Built Steam x86-64 gaming rootfs: $output"
