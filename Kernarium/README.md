# Kernarium

Kernarium is the iOS application that runs one Linux or Android guest on the
Darwin runtime. It is an iOS device product only;
[`graphics.md`](../docs/graphics.md#current-path) gives the Metal 4 reason and
how the same device slice runs on an Apple silicon Mac in Designed for iPad
mode. Build, launch, and test commands are in
[`testing.md`](../docs/testing.md#apple-builds-and-kernarium).

## Ownership

```text
Kernarium/            SwiftUI application: settings, guest selection, disk
                      capacity, decompression, console, shader cache
Runtime/              runtime framework target, one per kernel flavour and page
                      profile
KernariumTests/       hosted application and runtime tests
KernariumUITests/     UI tests
KernariumAutomation/  long-lived UI-automation host for a connected device
scripts/              launch, packaging, bundle assembly, and automation tools
```

## Runtimes and runs

Kernarium uses `6.18`-series runtime frameworks for 16 KiB Linux, 4 KiB Linux,
and 4 KiB Android. Their names stay stable across patch releases and kernel
commits, and each carries its kernel ELF in signed Mach-O `__TEXT` (see the
[architecture](../docs/architecture.md#memory-and-executable-code)). The
application embeds every framework, links none at launch, and loads exactly
one for the process lifetime.

The library runs one guest per process, so a launch runs its guest once. When
the run returns, whether the guest powered off or the runtime failed, the
application says how it ended and asks to be relaunched; it never starts a
second run in the same process. The guest runs on the named `uml-launcher`
thread, which leaves the main thread to UIKit and MetalKit.

Kernarium defaults to 16 KiB Linux, the processor count `NSProcessInfo`
reports, and 4 GiB of guest memory where the process is given the host's
memory (Designed for iPad on a Mac) or 2 GiB on a physical device, whose jetsam
limit a 4 GiB guest would cross. Android fixes the page profile at 4 KiB.

## Guest storage

A Linux guest boots an image file in the application's Documents directory:
the one the settings name, or, while they name none, the accepted KDE desktop
image `kde-20260929-r2.erofs`
([`tools/rootfs/desktop`](../tools/rootfs/desktop/README.md)). A name the
settings already store, `rootfs.erofs` included, is kept, and every image in
Documents stays selectable. An Android guest is a `.kern`
bundle directory in Documents; the
[Android README](../android/lineage-24/README.md#apple-consumers-and-guest-bundles)
owns its layout. A bundle holds the guest's persistent, directly writable image,
and every launch reopens that same image, so rebuilding the application or a
runtime framework must not erase it and a bundle is never recreated merely to
relaunch it. When copying a bundle to a device, target the complete
`Documents/<name>.kern` destination; never copy into or replace the Documents
directory itself, which can flatten the bundle or destroy other guests.

Launch does not compare kernel, runtime, or rootfs release identities; missing
or malformed inputs fail at their ordinary file or image boundary. Disk
capacity and compressed delivery are application concerns, so runtime
frameworks receive only an ordinary disk image and no capacity policy.
`DiskUtil` owns a raw file's capacity before launch and dispatches any
format-specific partition-table update; Android GPT growth is in the
[static disk contract](../android/lineage-24/README.md#static-disk-and-boot-contract).
`ZstdUtil` streams a zstd source into an adjacent temporary file, optionally
representing zero regions as sparse holes, and publishes the destination only
after a complete decode.

## Guest boot

A Linux run boots the base archive the runtime embeds
([`tools/rootfs/boot-ramdisk`](../tools/rootfs/boot-ramdisk/README.md))
followed by a segment holding the generic per-run `/init` and its
`/boot.env`. The runtime writes that initrd to its temporary directory as
`kernarium-initrd-<pid>.cpio`, removes it when the run returns, and removes
any an earlier, killed process left. The parameters mount the image read-only
on `ubd0` beneath an overlay whose writable layer is an ext4 filesystem
formatted afresh on `ubd1`: the sparse `kernarium-overlay.ext4` the
application recreates in its temporary directory at each launch, at the size
the overlay setting names. Docker and containerd state is bound from that
filesystem beside the overlay, because neither accepts an overlayfs backing.
A sealed ext4 image is attached read-only as well. `/init` then switches root
into the image's own `/sbin/init`, which boots `graphical.target`.

Nothing is typed on the console and the runtime edits no image: accounts,
device groups, getty and display-manager policy, network and resolver belong
to the image. The KDE, Phrog and postmarketOS images Kernarium boots carry
that setup ([`tools/rootfs/desktop`](../tools/rootfs/desktop/README.md)); the
KDE image masks the VT gettys, since SDDM runs the session on a VT of its own.
The unified `rootfs.erofs` carries none of it and boots to its own defaults:
SDDM without autologin, beside the image's `getty@tty0`.

## Automation runs

The release runtime accepts no host payload and carries no test campaign. The
`kernarium-ios-device-automation` and `kernarium-ios-device-automation-4k`
presets build the Linux runtimes with `UML_KERNARIUM_AUTOMATION_HARNESS`, and
`scripts/package-ios-frameworks.sh --automation` packages them in place of
the release ones. Only these read:

- `KERNARIUM_BOOT_PAYLOAD`, a plain file name in Documents: a padded newc
  segment of at most 64 MiB, written by `uml-initramfs`, that the runtime
  appends after its own, so its files, `/boot.env` included, replace the
  runtime's; and
- `KERNARIUM_SYSTEMD_UNIT`, the `*.target` the guest boots instead of
  `graphical.target`.

A payload also attaches the artifact relay, a Virtio serial port whose
acknowledged frames the runtime prints on standard output. The runtime prints
`Kernarium: automation boot with a <n>-byte payload and target <unit>`; a
host that does not see that line is talking to the release runtime.
`tests/integration/support/kernarium.py` writes payloads for the conformance
lanes, which boot `uml-cts.target`, the WebGL Aquarium probe, which boots
`uml-aquarium.target`, and the device workload runner, whose script the
harness session runs before powering the guest off. The lanes are in
[`testing.md`](../docs/testing.md#apple-builds-and-kernarium) and
[`tests/perf/README.md`](../tests/perf/README.md).

## Devices and display

Kernarium attaches:

- an absolute touchscreen and evdev keyboard;
- one signed-16-bit stereo 48 kHz playback stream through Virtio-Sound;
- an Android-only Virtio battery reflecting the Apple host's percentage and
  charging state, with a 100-percent charging fallback when the host has no
  readable battery;
- SLiRP networking from the verified iOS prefix;
- a Virtio-GPU Metal framebuffer; and
- the guest's `ttyS0` stream mirrored to a terminal view.

Point display mode maps safe-area points to guest pixels at 160 DPI; Retina
mode multiplies both geometry and density by `nativeScale`. The GPU's EDID, the
touchscreen, and Android's logical density on the kernel command line all use
that launch geometry. The Metal shader cache policy is in
[`graphics.md`](../docs/graphics.md#metal-shader-cache).

## Lifecycle

Kernarium asks the guest to write back when the application loses focus,
quiesces it before the application is backgrounded or terminated, and resumes
it on foregrounding. Its Power key also wakes a guest that suspended itself,
such as Android once its display is off. The runtime side of sync, quiesce,
resume, and wake is in
[`../docs/architecture.md`](../docs/architecture.md#guest-lifecycle).

iOS revokes the host GPU within seconds of `didEnterBackground`, and a guest
request that reached Metal afterwards would make the guest's Vulkan device
lost for good. The application therefore holds the guest's GPU requests inside
`didEnterBackground`, before it quiesces, and releases them when it returns to
the foreground. The hold and the quiesce share the handler's four-second
budget, and the hold itself waits at most one second for a request already
running. Requests the guest queues meanwhile wait in their rings, through the
suspend, and run in order after the release.

iOS may still end a suspended application, and the guest goes with it. The
application records the background edge in a marker file and removes it on
foregrounding; a launch that finds one says the guest restarted because
Kernarium was closed in the background, once, until it is dismissed.

## Scripts

Each script documents its options in `--help` or its header:

- `run-ios-mac.sh` builds, installs, provisions, and launches the application
  on an Apple silicon Mac in Designed for iPad mode;
- `package-ios-frameworks.sh` packages the selected runtime and graphics slices
  and the static GPT fdisk and zstd XCFrameworks for Xcode, with
  `--automation` the automation Linux runtimes, and the host battery archive
  only `KernariumTests` links;
- `make-guest-bundle.py` assembles an Android `.kern` bundle from an installed
  release;
- `ipad-automation.py` drives UI automation on a connected iPad through the
  `KernariumAutomation` host; and
- `verify-signed-kernel-carrier.py` checks that a kernel image is safe for the
  signed carrier.
