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
and 16 KiB Android. Their names stay stable across patch releases and kernel
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
limit a 4 GiB guest would cross. Android fixes the page profile at 16 KiB.

## Guest storage

A Linux guest boots an image file in the application's Documents directory,
`rootfs.erofs` unless the settings name another. An Android guest is a `.kern`
bundle directory in Documents; the
[Android README](../android/aosp-17/README.md#apple-consumers-and-guest-bundles)
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
[static disk contract](../android/aosp-17/README.md#static-disk-and-boot-contract).
`ZstdUtil` streams a zstd source into an adjacent temporary file, optionally
representing zero regions as sparse holes, and publishes the destination only
after a complete decode.

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
it on foregrounding. The runtime side of sync, quiesce, and resume is in
[`../docs/architecture.md`](../docs/architecture.md#guest-lifecycle).

## Scripts

Each script documents its options in `--help` or its header:

- `run-ios-mac.sh` builds, installs, provisions, and launches the application
  on an Apple silicon Mac in Designed for iPad mode;
- `package-ios-frameworks.sh` packages the selected runtime and graphics slices
  and the static GPT fdisk and zstd XCFrameworks for Xcode;
- `make-guest-bundle.py` assembles an Android `.kern` bundle from an installed
  release;
- `ipad-automation.py` drives UI automation on a connected iPad through the
  `KernariumAutomation` host; and
- `verify-signed-kernel-carrier.py` checks that a kernel image is safe for the
  signed carrier.
