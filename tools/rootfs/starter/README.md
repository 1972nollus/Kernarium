# Mainline Linux starter images

This directory owns the producer for writable mainline Linux starter disks
and for the guest boot base archives. The default products are two base
archives and three raw GPT disks:

- `uml-boot-base-aarch64.cpio.gz` and `uml-boot-base-x86_64.cpio.gz` are the
  static BusyBox and e2fsprogs base every Linux guest boots through, with a
  stub `/init` that only fails; the
  [boot ramdisk contract](../boot-ramdisk/README.md) owns their layout, the
  per-run `/init` appended to them and their acceptance;
- `alpine-3.24.2-minimal.img` is an Alpine OpenRC root with an automatically
  logged-in root shell on `hvc0`;
- `debian-13-minimal.img` is a Debian systemd root with an automatically
  logged-in root shell on `hvc0`; and
- `debian-13-plasma-wayland.img` starts Plasma on Wayland as `debian`, carries
  System Settings, Konsole, Chromium, the stock Debian Mesa VA-API driver,
  `vainfo`, FFmpeg, and a PipeWire audio server for the guest sound device,
  logs `debian` in on `hvc0`, and grants that user passwordless sudo. Its
  panel pins only installed applications, so Discover and its PackageKit
  daemon are omitted. The session's XDG directories exist before the first
  login, Folder View shows `~/Desktop` through the local-file worker, since
  the `desktop:/` worker crashes under UML, the idle lock never engages, and
  the image carries no Lavapipe ICD for KInfoCenter to mistake for the
  guest GPU.

Every disk has one Linux-filesystem GPT partition ending at the last usable
LBA. Networking belongs to the image: OpenRC or systemd starts a DHCP client
and installs DNS information without kernel IP autoconfiguration, and the image
keeps its own hostname rather than one offered by DHCP. The images
also own their account, getty, and desktop policy; launchers must not inject
replacement setup scripts. The kernels are monolithic and have no uevent
helper, so the Alpine image manages `/dev` with mdevd, Alpine's netlink device
manager, instead of BusyBox mdev, and lists no boot-time modules.

The base archives are compressed cpio initramfs parts, never another UBD
device or a legacy `/dev/ram0` block ramdisk; only the persistent GPT disk is
attached as `ubd0`. Their static executables keep the 16 KiB ELF segment
alignment the accepted bases are built with: it is valid on the 4 KiB guest
and matches the Android userspace's page-agnostic alignment. Buildroot builds
its own cross toolchain, so the arm64 builder also produces the x86-64 base.
The kernel releases the compressed initrd after unpacking it, and BusyBox
`switch_root` removes the initramfs contents before executing the real init.

A starter disk boots through the generic per-run `/init` with
[`boot.env`](boot.env): its sole ext4 partition becomes the read-write root.
Growth is deliberately conservative. `/init` considers growth only when `ubda`
exposes exactly one partition and BusyBox `blkid` identifies `ubda1` as ext4,
and grows only when the partition exceeds the filesystem by more than the tail
`resize2fs` itself leaves unused (a page remainder or a trailing group too
small for its metadata). It then runs `e2fsck -f -p` before `resize2fs`, which
refuses an unmounted filesystem that was mounted after its last full check.
Multiple-partition, non-ext4, and already-grown disks bypass both steps, so a
normal boot neither checks nor resizes. A check that leaves errors or a failed
resize is only a warning: `/init` still mounts the unchanged root and enters
its init. A root that cannot itself be mounted or an entry that cannot run
remains a boot error.

The producer runs natively on arm64 Linux and requires `sudo`, Buildroot's
ordinary build prerequisites (including `cpio` and `bc`), `debootstrap`,
`e2fsprogs`, and GPT fdisk, plus `qemu-user` for the armhf image. It
downloads pinned Buildroot and Alpine inputs and current Debian 13 packages. A
container host is sufficient only when its root can create character device
nodes and mount `proc` and `sysfs` inside the work tree, because `debootstrap`
and the chroot package installs need both; the producer has no fakeroot path
for a container that forbids them.
The canonical entry point is:

```sh
tools/rootfs/starter/build-all.sh \
  --work /path/to/work --output /path/to/release
```

`--product NAME` (repeatable) builds only the named products.

`release-manifest.json` and the adjacent package inventories record the exact
inputs and results. Build trees and release artifacts stay outside the source
tree.

Disk capacity is host policy. `grow-image.py` transactionally enlarges a copy
and its sole GPT partition; it never changes the filesystem. The next boot
through `/init` grows ext4 before mounting it:

```sh
python3 tools/rootfs/starter/grow-image.py \
  --image alpine-3.24.2-minimal.img --size-gib 4
```

Boot the resulting disk with
`uml-darwin --guest-file=/boot.env=tools/rootfs/starter/boot.env ubd0=<disk>`
and attach the host Virtio-console service to reach the image-owned `hvc0`
getty.

The KDE convenience launcher, `tools/boot_kde_vnc.py`, boots the Plasma
disk of release `starter-20260928-kde-setup` from `.build/rootfs/<release>/`
by default. It attaches the disk read-only, so the host file must not be
writable, selects `/dev/ubda1` beneath a tmpfs overlay, and has the per-run
initramfs start the image's own `/sbin/init`: the network, account, getty,
and Plasma session are the image's, and the launcher adds no setup.
`--rootfs`, `--rootfs-type`, and `--root-device` remain available for an
explicit legacy-image launch. An ext4 filesystem selects the starter policy
unless `--image-profile legacy` names an ext4 derivative of the unified EROFS
image, which keeps that image's unpartitioned root, SDDM session, getty masks
and [seed](../boot-ramdisk/README.md#harness-session).

## The armhf image

`debian-13-minimal-armhf.img` is the 32-bit Arm twin of
`debian-13-minimal.img`, for AArch32 guests. It is built only on request
(`--product debian-13-minimal-armhf`), into a release of its own; the default
release does not hold it. It installs the same package list through
`debootstrap` alone, so no ARM program needs the network, and the twin has one
package more: `adduser`, which `debootstrap` resolves from a dependency
alternative where apt keeps the installed `systemd-sysusers`.

The first stage runs natively. The second stage, which runs the packages'
maintainer scripts, is the only ARM code the build runs, and
[`armhf-run.sh`](armhf-run.sh) runs it under Debian's `qemu-user` with no
network. It never uses the host's `binfmt_misc`, which a container usually
cannot register in: the command runs in new user, mount, PID and network
namespaces, with a private `binfmt_misc` instance (Linux 6.7 or newer) that
holds the one `qemu-arm` format and ends with the command. `armhf-run.sh
--check` is the producer's preflight.

Inputs are pinned by the Debian archive's own chain, checked by
[`debootstrap-pins.py`](debootstrap-pins.py): the signed `InRelease` names the
`Packages` index, which names every downloaded package. The release manifest
records the index digests and the download totals, and
`debian-13-minimal-armhf.img.packages.tsv` lists each installed package with
its version, architecture, size and SHA-256.

[`audit-elf.py`](audit-elf.py) checks every ELF file of the image against the
4 KiB guest page, and `debian-13-minimal-armhf.img.elf.tsv` reports each file.
A file the page cannot map fails the build.
[`armhf-verify.sh`](armhf-verify.sh) then runs the image's systemd, getty and
core programs under `qemu-arm` and resolves the libraries of every dynamic
program.
