# x86-family desktop qualification rootfs

This producer creates the Debian 13 userspace used to qualify the 4 KiB x86
execution boundary with Plasma Wayland, Wine, Steam, and Chromium. The primary
architecture is amd64. Debian's Steam installer requires an i386 runtime, so
the image contains amd64 and i386 ELF payloads and rejects every other ELF
machine type. It never contains arm64 userspace.

Run the producer on a native x86-64 Debian builder as an ordinary user with
passwordless sudo, `debootstrap`, and `erofs-utils`:

```sh
tools/rootfs/x86_64-desktop/build.sh \
  --work /path/to/new-work-directory \
  --output /path/to/new-rootfs.erofs
```

The output is immutable. Its adjacent package and ELF inventories describe
the installed inputs, and the adjacent checksum covers the EROFS image. The
GPU guest harness mounts it as a read-only lower filesystem with a disposable
tmpfs overlay. The image carries no machine ID: `/etc/machine-id` is empty,
and D-Bus's `/var/lib/dbus/machine-id` links to it, so every guest generates
its own at first boot. SDDM logs the `uml` account into the Plasma Wayland session;
that account has passwordless sudo for qualification work. The image does not
unconditionally enable an `hvc0` getty. The KDE launcher requests the stock
getty only when `--virtio-console-socket` attaches that device, while
`--serial-socket` exposes the existing `ttyS0` console. Both sockets are
reconnectable and owner-only; log in as `uml` (the qualification account has
no password). `wine64:amd64` and `wine32:i386` are installed so a win64 prefix
has Wine's complete Debian runtime rather than stalling after its missing-WoW64
warning.

For a long-running qualification guest, expose both independent consoles:

```sh
python3 tools/boot_kde_vnc.py \
  --serial-socket /private/tmp/uml-x86-ttyS0.sock \
  --virtio-console-socket /private/tmp/uml-x86-hvc0.sock \
  --emulator .build/presets/macos-x86_64/bin/uml-darwin \
  --rootfs .build/rootfs/x86_64-desktop.erofs \
  --rootfs-type erofs
```

Attach, detach, and later reconnect without restarting the guest using the
macOS Unix-socket client, for example
`nc -U /private/tmp/uml-x86-hvc0.sock`. The Virtio console is the quieter
command channel; `ttyS0` also carries kernel and systemd console output.

Debian's Chromium reaches the Virtio video device through VA-API. The producer
builds [`tools/video/va-v4l2`](../../video/va-v4l2/README.md) in the image's
own root against its libva and libdrm, runs the driver's unit test and probe
self-test there, installs `uml_drv_video.so` beside Mesa's drivers and the
`/etc/chromium.d/kernarium-video` file that selects it, and removes the build
tools again.

The Debian package installs Steam's bootstrap and its recommended amd64 and
i386 desktop-library closure. The first `steam` launch presents a Zenity
confirmation; select **Install** in the visible desktop session. It then
downloads Valve's per-user client into the disposable guest overlay, so this
step requires guest networking.
