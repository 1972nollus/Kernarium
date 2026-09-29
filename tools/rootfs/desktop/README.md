# Desktop images

This directory owns the derivation of the desktop images Kernarium boots:
KDE Plasma, Phosh greeted by Phrog, and postmarketOS with Sxmo. None of their
sources is built in this repository. The KDE source is the pinned unified
`rootfs.erofs`; the other two are user-supplied images. Each source lacks
some policy a Kernarium guest needs, and the derivation bakes that policy
into a new image, so a launcher never edits an image while it boots.

[`images.json`](images.json) records, for each image, the source file's size
and SHA-256, the setup script, the compressor that reproduces the source's
layout, and the accepted output with the digest of the setup that produced it.
[`derive.py`](derive.py) refuses a source that does not match and never
replaces an existing output. A change to a setup or to the derivation is
accepted only with a new release derived from it.

## What each image carries

- [`setup/kde`](setup/kde): the `uml` user's device groups, getty masks on
  the display manager's VTs (SDDM already conflicts with `getty@tty1`), XDG
  and Trash directories, SDDM autologin into Plasma, the Folder View default,
  the disabled idle lock, and no Lavapipe ICD. The macOS KDE launcher also
  runs this file at boot on the unified image and its sealed ext4
  derivatives, which predate it.
- [`setup/phrog`](setup/phrog): user 1000's device groups, empty Phrog and
  Phosh `phoc.ini` files, so the compositor takes the Virtio-GPU's preferred
  mode and density, and no development `phosh.service` beside the
  display-manager path to greetd and Phrog.
- [`setup/postmarketos`](setup/postmarketos): udev coldplug under UML, which
  OpenRC otherwise counts among containers, the Kernarium `deviceinfo`, udev
  classification of the Kernarium and VNC input devices, and an OpenRC
  service granting the display and input nodes to the desktop's groups.

The derivation also removes the machine ID each source was built with, so
every guest generates its own at first boot and keeps it in its writable
root. A systemd image carries an empty `/etc/machine-id`; an OpenRC image
carries none, because `dbus-uuidgen --ensure` rejects an empty file. D-Bus's
`/var/lib/dbus/machine-id` links to `/etc/machine-id`.

Each image keeps its own init, network, resolver, accounts and session;
Kernarium boots it through the generic per-run `/init` into `/sbin/init`
([Kernarium/README.md](../../../Kernarium/README.md#guest-boot)). The accepted
KDE image is the one Kernarium selects while its settings name none
([Kernarium/README.md](../../../Kernarium/README.md#guest-storage)), and
`Kernarium/scripts/run-ios-mac.sh` stages it by default from
`.build/rootfs/<release>/`.

## Deriving

The derivation runs as root on arm64 Linux with erofs-utils: it extracts the
source with owners, modes and extended attributes (file capabilities such as
KWin's), runs the setup in a chroot of the extracted tree with `/proc` and
`/dev` mounted, and writes the new image with the recorded compressor beside
a receipt naming the source, the setup and the output:

```sh
sudo tools/rootfs/desktop/derive.py --image kde \
  --source /path/to/rootfs.erofs --output /path/to/kde-<date>-r<n>.erofs
```

Publish a new release under a new name and record it in `images.json`; never
replace an accepted image or the source it came from. A new KDE release also
renames Kernarium's default image, `RootfsImage.defaultFilename` in
`Kernarium/Kernarium/EmulatorConfiguration.swift`, and its tests.
