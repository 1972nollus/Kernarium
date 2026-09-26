# Mainline Linux starter images

This directory owns the producer for writable mainline Linux starter disks.
The products are one common initramfs and three raw GPT disks:

- `uml-resize-initramfs.cpio.gz` checks `/dev/ubda1`, grows a sole ext4
  filesystem to the already enlarged partition while it is still unmounted,
  mounts it, and enters the image's init;
- `alpine-3.24.2-minimal.img` is an Alpine OpenRC root with an automatically
  logged-in root shell on `hvc0`;
- `debian-13-minimal.img` is a Debian systemd root with an automatically
  logged-in root shell on `hvc0`; and
- `debian-13-plasma-wayland.img` starts Plasma on Wayland as `debian`, carries
  Konsole, Chromium, the stock Debian Mesa VA-API driver, `vainfo`, and
  FFmpeg, logs `debian` in on `hvc0`, and grants that user passwordless sudo.

Every disk has one Linux-filesystem GPT partition ending at the last usable
LBA. Networking belongs to the image: OpenRC or systemd starts a DHCP client
and installs DNS information without kernel IP autoconfiguration. The images
also own their account, getty, and desktop policy; launchers must not inject
replacement setup scripts.

The resize artifact is a compressed cpio initramfs passed with `initrd=`. The
kernel unpacks it as the initial memory-backed root; it is neither another UBD
device nor a legacy `/dev/ram0` block ramdisk. Only the persistent GPT disk is
attached as `ubd0`. Its static executables use 16 KiB ELF segment boundaries,
so the same cpio boots with both supported guest page profiles.

The kernel releases the compressed initrd input after unpacking it. BusyBox
`switch_root` removes the old initramfs contents before executing the real
init, which releases their file-backed memory once nothing still references
them.

Growth is deliberately conservative. The initramfs considers growth only when
`ubda` exposes exactly one partition and BusyBox `blkid` identifies `ubda1` as
ext4, and grows only when the partition exceeds the filesystem by more than the
tail `resize2fs` itself leaves unused (a page remainder or a trailing group too
small for its metadata). It then runs `e2fsck -f -p` before `resize2fs`, which
refuses an unmounted filesystem that was mounted after its last full check.
Multiple-partition, non-ext4, and already-grown disks bypass both steps, so a
normal boot neither checks nor resizes. A check that leaves errors or a failed
resize is only a warning: the initramfs still attempts to mount the unchanged
root and enter its init. A root that cannot itself be mounted or does not
contain `/sbin/init` remains a boot error.

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
through the common initramfs grows ext4 before mounting it:

```sh
python3 tools/rootfs/starter/grow-image.py \
  --image alpine-3.24.2-minimal.img --size-gib 4
```

Boot the resulting disk as `ubd0`, load the common initramfs, and use
`root=/dev/ubda1 rootfstype=ext4 init=/sbin/init ip=off`. Attach the host
Virtio-console service to reach the image-owned `hvc0` getty. The `ip=off`
override is harmless with the shipped configuration and also suppresses a
legacy launcher's kernel-DHCP default.

The KDE convenience launcher expects an accepted Plasma artifact at
`.build/rootfs/starter/debian-13-plasma-wayland.img` by default. It attaches
the disk read-only, selects `/dev/ubda1`, and uses the image-owned network,
account, getty, and Plasma session. `--rootfs`, `--rootfs-type`, and
`--root-device` remain available for an explicit legacy-image launch.
