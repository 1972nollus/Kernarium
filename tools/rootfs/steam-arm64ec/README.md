# Steam ARM64EC gaming rootfs

This producer builds a Debian 13 ARM64 Steam appliance with Gamescope,
Xwayland, the native ARM64 Steam client, and source-built Proton 11. It uses
the emulator's `xtajit64` ARM64EC and `xtajit` i386 WoW64 CPU bridges, including
ARM64EC DXVK and vkd3d-proton. It installs no desktop environment.

The default build retains Debian Mesa. An experimental Vulkan build imports
receipt-verified Mesa/Venus and Vulkan-Loader runtimes from the
[maintained Vulkan guest](../vulkan/README.md). The importer checks the source
epoch and each installed file, validates dynamic dependencies, and selects
only the Virtio ICD. Its receipt is included in the image provenance. This
qualifies neither game rendering nor Vulkan conformance.
The Vulkan gaming session selects Zink for Xwayland and OpenGL clients so
their native-heap buffers can be imported by Venus. VirGL buffers cannot cross
that sharing boundary. The native Steam UI includes GTK 2, GTK 3, OpenAL,
and libnm; the binary audit checks its required native library aliases.
`lsof` is required for Steam to authenticate its loopback WebUI connections.

The image uses the existing dedicated 4 KiB ARM64EC Wine kernel and emulator.
There is no 16 KiB Steam product. The Wine emulator exposes a dedicated Virtio
touchscreen and keyboard. Native clicks and VNC presses become touch contacts;
the guest has no mouse or hybrid tablet device.

Apple builds consume the accepted prebuilt kernel; the Vulkan variant requires
the current kernel source series built with the Wine profile, including the
shared-memory GPU transport. This
workflow changes neither maintained kernel line. iOS packaging
and automatic image discovery are not part of this producer.

## Build on native ARM64 Linux

Use an ordinary account with passwordless sudo on Debian 13. Preparation
installs host tools, retrieves the SDK and source closure, and prepares a
signed Debian package closure. Gamescope comes from `trixie-backports`.

```sh
dependencies/prepare.sh --product steam-arm64ec-rootfs --root /work/steam-inputs
tools/rootfs/steam-arm64ec/build-proton.sh /work/steam-inputs /work/proton-build 12
tools/rootfs/steam-arm64ec/build.sh --inputs /work/steam-inputs \
  --proton /work/proton-build/build/redist --work /work/steam-image \
  --output /work/steam-arm64ec-gaming.erofs
```

For the Vulkan variant, add `--vulkan-root` and `--vulkan-receipt` to the image
build. They accept either runtime the maintained Vulkan guest publishes, and
the receipt's own artifact identity selects its importer: the receipt-gated CTS
image (`/work/vulkan/stage/rootfs-kosmickrisp` with
`rootfs-vulkan-venus.receipt.json`), whose runtime is imported without the CTS
payload, or the relocatable per-architecture runtime `stage-runtime.py`
produces with its own receipt. Both inputs are read-only, both record what they
installed in the image, and the audit and provenance read whichever record the
image carries. Use fresh prepared inputs when the Debian package closure
changes.

Build work and output paths must be absent. Preparation reuses verified
inputs. `dependencies/prepare.sh --product steam-arm64ec-rootfs --root
/work/steam-inputs --verify` performs read-only source, payload, and SDK
verification; `--no-fetch` has the same verification-only behavior for this
product. The Proton and image builders run containers with networking
disabled and never pull images. Standard rootful Podman supports build hosts
where nested rootless containers cannot mount procfs; privileged containers
are not required.

[`sources.json`](sources.json) locks the SDK digest and Steam runtime snapshot.
[`downloads.json`](downloads.json) locks the additional build archives.
The unmodified upstream [`steam-client.manifest`](steam-client.manifest)
pins the Steam client package set and each package's SHA-256. The producer
installs the complete seed; Steam owns its subsequent updater state and
`.installed` manifest. No user account or credentials are included.
Debian package versions are resolved during preparation and recorded with
the base archive digest. Build timestamps and compiler identities remain
authentic.

Proton source identities and generated topics live in
[`dependencies/steam-arm64ec`](../../../dependencies/steam-arm64ec/README.md).
Its Wine 11 module adapter restores private CPU-feature names removed since
Wine 10 while retaining the shared bridge implementation. Wine supplies a
readable ARM64 signal restorer so libgcc can unwind native thread shutdown
without reading the kernel's synthetic signal-return address. Piper's unmodified
upstream dependencies use their own CMake projects and prepared private
prefixes. ARM64 prefixes are created lazily in the guest because the bridge
requires the emulator's hostcall service.

## Image and startup contract

The output includes SHA-256, package and binary inventories, and input
provenance sidecars. The audit requires native ARM64 Steam and Wine launchers,
both CPU bridge DLLs, ARM64EC DXVK and vkd3d DLLs, and both Windows execution
probes. It rejects guest FEX and foreign system ELF binaries. Steam's upstream
package set also contains x86 compatibility assets; these are retained only
under its explicit bundled paths and do not provide an x86 Linux execution
engine.

`steam-gaming.service` starts a Gamescope session as the image-owned `uml`
user. That account has passwordless local sudo and serial autologin, matching
the existing development appliances. The EROFS root remains read-only. A separate raw GPT disk contains one ext4
partition, mounted at `/userdata`; its `home`, `var`, and `root` directories
are bind-mounted at `/home`, `/var`, and `/root`. `/run` and `/tmp` are tmpfs.
There is no root OverlayFS. Package metadata and the source-built Proton tool
remain image-owned, while Steam client updates, games, settings,
saves, and prefixes live on the data partition.
Proton's generated default prefixes and distribution lock also live in home,
keyed by its build identity so a replacement Proton build gets fresh templates.
A completion marker prevents a cancelled Steam probe from exposing a partial
template. A later launch finishes initialization before copying that template.

The image-owned init checks ext4 before mounting it. It formats only a new
partition carrying the launcher's explicit initialization marker. Existing
ext4 is repaired with `e2fsck -p`; unrecognized filesystems or failed repairs
stop boot with a visible recovery message and preserve the disk. The first
boot copies the bundled client through a staging directory and publishes it
only after the copy completes. Subsequent boots never reseed an existing home.
Replacing the rootfs therefore retains the client installation and user data.
This follows Steam Deck's separation of an immutable system and persistent
home; it does not implement the Deck's hardware-specific A/B boot layout.

Before starting Steam, the image selects its bundled `proton-uml-arm64ec`
compatibility tool globally and for existing game mappings. Its tool manifest
uses the image-supplied native Linux runtime without a Steam runtime download
dependency; the image audit rejects such dependencies. Queued downloadable Proton and
FEX tool manifests move into `steamapps/kernarium-disabled-tools`; their exact
metadata and downloaded files are retained. The original client configuration
is backed up before its compatibility entries change. Account settings and
game files are preserved. A second invocation leaves a running client's
configuration alone.

The image launcher also keeps Mesa's Vulkan window system enabled for the
client's web helper. The ARM64 client installs its own `steamwebhelper`
launcher that exports `LIBGL_KOPPER_DISABLE=true` and then runs `taskset`.
OpenGL would then present through X11 DRI3, whose buffer allocation this
renderer supports for no image format, so the Big Picture window would deliver
no frame at all and the compositor would keep showing the installation screen.
The client verifies the sizes of its installed files and reinstalls the whole
package when one differs, so its launcher is never edited; the image prepends
`/usr/local/libexec/kernarium-webhelper` to `PATH` instead, and the `taskset`
there removes the variable before running the real one. The x86-64 client
installs no such launcher and needs no correction.

The Kernarium splash appears on the boot framebuffer while storage is checked
and the seed is copied. Gamescope then displays a matching installation screen
with actual Steam download bytes and extraction/verification stages. Steam's
completed updater restart is handled automatically. Its C++20 renderer is built with CMake/Ninja in the verified Steam SDK.
OpenGL ES draws the background, cached glyph textures, and animated bar on the
GPU: GBM/EGL page flips provide the boot scanout and EGL swaps present inside
Gamescope. Glyph rasterization happens once at startup; progress frames do not
rasterize text or copy a CPU framebuffer. Busy animation is paced at 30 frames per
second; unchanged determinate progress does not redraw. Status is read at 5 Hz.
The renderer rejects software rendering. The screen yields when
the Big Picture window becomes visible. Console diagnostics remain available
on `ttyS0`; the graphical screen has no getty or distribution branding.

## macOS qualification

Place the image at `.build/rootfs/steam-arm64ec-gaming.erofs`, configure the
accepted 4 KiB Wine kernel artifact and verified host prefixes through local
presets, then run:

```sh
cmake --workflow --preset check-steam-arm64ec-macos
```

The gate boots the image-owned init with a test-only GPT data disk and a virtio GPU, starts
Xvfb without a desktop, requires AMD64 exit code 42 and i386 exit code 43,
and repeats the AMD64 launch through Proton. It then starts the actual gaming
service and requires the known Vulkan limitation. Any illegal instruction,
missing runtime dependency, prefix failure, or other gaming-session failure
blocks the gate. This graphics-limitation check applies to the stock Mesa
image. Guest output is saved under the build's `reports/` directory.
The smoke reuses one temporary 16 GiB GPT disk across two boots and checks
filesystem and machine identities, home and system-state markers, and root
write rejection. It changes the rootfs pathname on the second boot. Only the
test-owned disk is deleted after the guest exits; interactive disks persist.

For experimental Venus, select `UML_APPLE_GPU_PRODUCT=apple-gpu-vulkan-experiment`,
its verified `UML_APPLE_GPU_DEPENDENCY_PREFIX`, and macOS deployment target
`26.0` in the local configure preset. Select the current Wine-profile kernel
bundle and the Vulkan Steam image as complete inputs. Launch the workflow or
interactive emulator with `UML_GPU_EXPERIMENTAL_VULKAN=1`. The smoke additionally
requires `vulkaninfo` to identify Venus, a Gamescope framebuffer with a valid
pitch, and a running native Steam bootstrap. These checks do not qualify
composed rendering. A running session still requires visual verification of
the login or setup screen.

For an interactive boot with the same development account:

```sh
python3 tools/rootfs/steam-arm64ec/boot.py \
  --emulator /path/to/uml-darwin-wine-arm64ec \
  --rootfs /path/to/steam-arm64ec-gaming.erofs \
  --userdata /path/to/steam-userdata.img --size-gib 32 \
  -- --video --gpu-display=native
```

The launcher explicitly selects the `gadgets` execution engine and enables
Virtio-Sound playback through the Apple audio output. It needs Python 3 and
`sgdisk`. It creates a sparse GPT disk only
when `--userdata` is absent on disk and holds an exclusive launch lock. The
size option applies only at creation; it never resizes or resets existing
data. Reuse the same `--userdata` path when selecting a replacement EROFS.
Keep the data disk backed up separately from the replaceable rootfs.

## Current Venus startup boundary

On macOS with Mesa 26.2.1 and the paired KosmicKrisp renderer, Gamescope can
register its XR24 framebuffer through Mesa's private scanout-image path.
The native allocation remains optimal and carries a logical pitch for DRM.
That scanout is a private Metal texture with no guest-visible Virgl resource,
so the display's native Metal readback is the only way it reaches a VNC
client, and it does so only while a client is connected: with none, each flush
is accepted and dropped. `UML_GPU_DISPLAY_ACTIVITY_LOG` names the path every
flush took; a `kind=blob` flush that reports `path=cpu` is publishing the
blob's own bytes, which name a host allocation and are not pixels.
AR24 client buffers may still be rejected for direct KMS scanout and require
composition. Shared Metal heaps use compatible texture storage across render
target and compositor usages; the composed Vulkan cube renders correctly.
Steam completes its client update and renders the Big Picture sign-in screen,
including the username/password and QR-code panels. The launcher uses
`-gamepadui`; Steam Deck hardware onboarding requires devices this guest does
not provide. The Virtio video service supplies the V4L2 decoder and the image's
udev rule provides `/dev/video-dec0`; this client's CEF asks for it with
`--enable-features=V4L2VideoDecode` and enumerates the node as the session
starts. Big Picture then plays its VP9 startup movie on the guest's CPU, because
Chromium finds no dma-buf-capable buffer allocator in this guest and so offers
no accelerated decoder at all; see the
[shared assets](../steam-common/README.md). A separate V4L2 probe verifies
hardware decoding and output pixels; a UI observation alone does not establish
that CEF used hardware decoding. This session presents only through Mesa's
Vulkan window system; a client that takes Mesa's X11 DRI3 path instead finds no
supported image format, delivers no frame, and leaves the compositor showing
its last one, which is what the web-helper hook above prevents. Account
login and a complete game download have been verified. Game launch
remains unqualified.
