# Guest boot ramdisk

Linux guests boot through a throwaway initramfs instead of a startup script
typed onto the kernel command line or edited into the image. This directory
owns that contract: the accepted base archives, the generic per-run `/init`
and the parameters it reads.

## Archive layout

The initrd is `pad4(base) || segment`:

- the **base** is the accepted `uml-boot-base-<arch>.cpio.gz` for the guest's
  userspace architecture, built by the
  [starter producer](../starter/README.md). It holds static BusyBox with the
  applets `/init` uses, e2fsprogs (`mkfs.ext4` stays e2fsprogs' `mke2fs`,
  since BusyBox's has no `-E`) and a stub `/init` that prints
  `UML_BOOT_BASE_ERROR:` and exits;
- the **segment** is an uncompressed newc archive that `UML::BootInitramfs`
  writes for each run: `/init`, `/boot.env` and any `/payload/...` files.
  Parents come before their children and every file is a single link.

The kernel unpacks every part into the same ramfs root, so a segment file
replaces the base file of the same name, and a further segment appended after
the first, padded the same way, replaces the first one's files. After a gzip member it looks for the
next header at the member's exact length and reads a newc header only at a
four-byte aligned offset; the zero padding makes the segment readable, and a
segment appended without it is reported as an invalid compressed archive and
dropped. UML appends `root=98:0`, and `root=`/`rootfstype=` are inert once
`/init` runs. The whole initrd sits in boot memory while it unpacks, and one
larger than the free boot memory panics the guest, so payloads stay small.

The base `/init` makes a boot without a per-run segment stop: exiting PID 1
panics the kernel, which aborts the emulator with a nonzero status.

## Per-run `/init`

[`init`](init) is the default segment `/init`. It runs under the base's ash,
which has no `command` builtin, and calls only applets the base verifier
requires. It mounts devtmpfs, proc and sysfs, waits up to ten seconds for the
root device, mounts the new root at `/newroot`, installs `/payload` into it,
moves `/dev`, `/proc` and `/sys` across, prints
`UML_BOOT_SWITCH_ROOT:<entry>` and `switch_root`s into the entry as PID 1.
Any failure prints `UML_BOOT_ERROR:<reason>` and stops the guest.

Each payload file is copied with `cp -af`, keeping its mode, and replaces a
file the image carries at the same path. Over a symlink it writes through to
the target when the target exists and replaces the link otherwise. A directory
the image already has keeps its own mode and owner, so a payload beneath
`/var/tmp` leaves it sticky and world-writable; a missing one is created
`0755`. A payload directory must not pass through a symlink in the image:
images with a merged `/usr` link `/lib` to `usr/lib`, so a payload names
`/usr/lib/...` instead.

`/boot.env` holds `KEY=VALUE` lines; blank lines and `#` comments are ignored
and an unknown key is an error.

| Key | Values | Default |
| --- | --- | --- |
| `UML_BOOT_ROOT` | root block device | `/dev/ubda` |
| `UML_BOOT_ROOT_TYPE` | `erofs`, `ext4` | `erofs` |
| `UML_BOOT_UPPER` | `tmpfs`, `ext4`, `none` | `tmpfs` |
| `UML_BOOT_UPPER_DEVICE` | block device (only, and required, with `ext4`) | none |
| `UML_BOOT_GROW` | `0`, `1` (only with `UML_BOOT_UPPER=none`) | `0` |
| `UML_BOOT_ENTRY` | absolute path in the new root | `/sbin/init` |

With `tmpfs` the image is mounted read-only beneath a tmpfs overlay that ends
with the run, and the payload lands in that upper layer. The new root keeps
both layers visible: the image at `/.uml-lower` and the tmpfs, with its
`upper` and `work` directories, at `/.uml-state`. `ext4` is the same overlay
on an ext4 filesystem that `/init` formats afresh on `UML_BOOT_UPPER_DEVICE`
with the base's `mkfs.ext4`, so the device's previous contents are lost.
Docker's overlay2 and containerd's overlay snapshotter refuse an overlayfs
backing, so on ext4 `/var/lib/docker` and `/var/lib/containerd` are bound from
`docker` and `containerd` directories (mode `0711`) on that filesystem, beside
`upper` and `work`. Kernarium boots every Linux desktop this way. With `none` the image
itself is the read-write root, as a starter disk or a one-image machine uses
it, and the payload is written into it. `UML_BOOT_GROW=1` first grows a sole
ext4 partition that a host tool has already enlarged, reporting
`UML_BOOT_GROW_*` markers; every growth problem is only a warning.
[`../starter/boot.env`](../starter/boot.env) selects that mode for starter
disks.

## Harness session

[`session`](session) prepares a disposable run on an image that does not
configure one itself. A launcher installs it as a payload at
`/usr/local/libexec/uml-session` with its parameters at
`/etc/uml-session.env` and names it as `UML_BOOT_ENTRY`. It runs as PID 1
under the image's own shell, prepares what the parameters ask for, prints
`UML_SESSION_READY:<entry>` and replaces itself with the entry. Any failure
prints `UML_SESSION_ERROR:<reason>` and ends the run. An entry other than
`/bin/sh` and `systemd` is a workload, an executable payload such as a
benchmark script: PID 1 may not exit, so the session runs it as a child,
prints `UML_SESSION_EXIT:<status>`, syncs and powers the guest off.

`/etc/uml-session.env` uses the `/boot.env` syntax:

| Key | Values | Default |
| --- | --- | --- |
| `UML_SESSION_ENTRY` | `/bin/sh`, `systemd`, a workload's absolute path | `/bin/sh` |
| `UML_SESSION_RESOLVER` | `image`, `user-network` | `image` |
| `UML_SESSION_CONTAINER_STATE` | `image`, `tmpfs` | `image` |
| `UML_SESSION_RUNTIME_MOUNTS` | `0`, `1` | `0` |
| `UML_SESSION_NETWORK` | `image`, `user-network` | `image` |
| `UML_SESSION_WORKSPACE` | `tmpfs`, `ext2` | `tmpfs` |
| `UML_SESSION_SWAP` | a block device | none |
| `UML_SESSION_CGROUP` | `v2-nsdelegate`, `v1:<controller>` | none |

`user-network` resolves through the SLiRP resolver and public ones, and gives
`eth0` the SLiRP provider's fixed lease, since the kernel has no IP
autoconfiguration. `tmpfs` container state keeps Docker and containerd data
out of the overlay's upper layer. Runtime mounts are what an init would
otherwise provide for a shell: `/run`, `/tmp`, `/dev/shm`, `/dev/pts`,
securityfs and shared propagation. `ext2` puts a 512 MiB memory-backed ext2
filesystem on `/tmp`. `systemd` finds the image's systemd, which mounts,
networks and manages everything but the resolver and container state itself,
so the other keys must keep their defaults.

The session mounts over `/tmp` and `/run`, and the kernel owns `/dev`, `/proc`
and `/sys`, so a payload a session run reads lives elsewhere, conventionally
under `/usr/local/libexec`. The integration and performance harness
(`tests/integration/support/guest.py`) boots every guest this way, the GPU
runner (`tests/integration/gpu/run_gpu_guest.py`) boots with `systemd`, and
`scripts/boot-overlay-systemd.sh` boots an EROFS image interactively with the
fixed parameters in [`overlay-systemd/`](overlay-systemd).

## Seeds

[`seeds/unified-kde-setup`](seeds/unified-kde-setup) is the desktop policy
that the unified KDE image (`rootfs.erofs` and the ext4 derivatives sealed
from it) does not carry itself: the `uml` user's device groups, getty masks
on the display manager's VTs, XDG and Trash directories, SDDM autologin, the
Folder View default, the disabled idle lock and the hidden Lavapipe ICD.
`scripts/boot_kde_vnc.py` installs it as a payload and runs it from the
harness test unit, which runs before the display manager. The file holds this
policy until the image does.

## Launchers

`uml-darwin` embeds the accepted base for its kernel slice's userspace
architecture and the default `init`. It writes the initrd into a private
`$TMPDIR/uml-guest-initrd-<pid>-*` directory, removes it when the run returns
and removes directories left by runs that have since exited.

- `--guest-init=<host file>` replaces `/init`;
- `--guest-file=<archive path>=<host file>` adds a file, such as `/boot.env`
  or `/payload/usr/local/bin/tool`.

Without either option the launcher still boots its base and the default init
(an overlay on the image, then the image's `/sbin/init`) unless the command
line names `init=` or `initrd=`, which leaves the whole boot to the caller.
When it supplies the initrd it also passes
`init=/uml-per-run-initramfs-did-not-run`, so an initrd the kernel never
loads, such as one behind a mistyped `initrd_offset=`, panics with that name
instead of starting the image's own init. `initrd=` together with a
`--guest-*` option is an error. The Android launcher boots Android's own
ramdisk and has none of this.

`uml-initramfs --output=<new file> [--base=<base>] [--init=<file>]
[--file=<archive path>=<file>]...` writes the same segment, or with `--base`
the whole initrd, for a host that boots the guest elsewhere.

## Accepting a base

`accepted-<arch>.env` names the release, file, size and SHA-256 of each base.
Builds read the archive from `.build/rootfs/<release>/<file>`, or from
`UML_BOOT_BASE_ARCHIVE`, and refuse one that does not match. Before changing a
descriptor, check the new archive:

```sh
tools/rootfs/boot-ramdisk/verify_base.py --descriptor accepted-aarch64.env \
  --archive /path/to/uml-boot-base-aarch64.cpio.gz
```

The verifier requires one gzip member holding a newc archive, the failing stub
`/init`, the applet links and e2fsprogs programs `/init` calls, and static ELF
programs for the descriptor's machine whose load segments are 16 KiB aligned,
so one base serves both guest page profiles. Publish a new release under a new
name; never replace an accepted one.

The unit test `boot-initramfs-archive`, the tooling tests
`rootfs-boot-base-contract` and `rootfs-initramfs-host-tool` and the
integration test `guest-boot-initramfs` cover this contract;
[docs/testing.md](../../../docs/testing.md) runs the integration runner on the
x86_64 slice.
