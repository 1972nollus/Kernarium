# Mainline Linux starter images

This directory owns the producer for writable mainline Linux starter disks
and for the guest boot base archives. The products are two base archives and
three raw GPT disks:

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
attached as `ubd0`. Their static executables use 16 KiB ELF segment
boundaries, so one base boots with both supported guest page profiles.
Buildroot builds its own cross toolchain, so the arm64 builder also produces
the x86-64 base. The kernel releases the compressed initrd after unpacking it,
and BusyBox `switch_root` removes the initramfs contents before executing the
real init.

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
`e2fsprogs`, and GPT fdisk. It downloads pinned Buildroot and Alpine inputs and
current Debian 13 packages. A container host is sufficient only when its root
can create character device nodes and mount `proc` and `sysfs` inside the work
tree, because `debootstrap` and the chroot package installs need both; the
producer has no fakeroot path for a container that forbids them.
The canonical entry point is:

```sh
tools/rootfs/starter/build-all.sh \
  --work /path/to/work --output /path/to/release
```

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

The KDE convenience launcher expects an accepted Plasma artifact at
`.build/rootfs/starter/debian-13-plasma-wayland.img` by default. It attaches
the disk read-only, selects `/dev/ubda1`, and uses the image-owned network,
account, getty, and Plasma session. `--rootfs`, `--rootfs-type`, and
`--root-device` remain available for an explicit legacy-image launch. An ext4
filesystem selects the starter policy unless `--image-profile legacy` names an
ext4 derivative of the unified EROFS image, which keeps that image's
unpartitioned root, SDDM session, and getty masks.
