# Steam x86-64 gaming rootfs

This producer builds a Debian 13 amd64 Steam appliance with Gamescope,
Xwayland, and Valve's own native x86-64 Linux Steam client started with
`-gamepadui`. It installs no desktop environment and no compatibility tool:
the guest runs the client Valve ships for this architecture, and Steam manages
Proton itself. It is the pure x86-64 sibling of the
[ARM64EC appliance](../steam-arm64ec/README.md) and shares that appliance's
storage, session, splash and launch contract through
[`../steam-common`](../steam-common).

The image has no i386 multiarch and no 32-bit system loader. Valve's current
Linux client publishes a native 64-bit `steamrt64/steam`, so the image launcher
starts it directly and the audit rejects any i386 ELF outside Steam's own
bundled compatibility paths. Every non-x86 ELF is rejected outright.

The default build retains Debian Mesa, which Gamescope cannot use for a
Vulkan session. The Vulkan build imports a receipt-verified Mesa/Venus and
Vulkan-Loader runtime built for x86-64 through that guest's own importer,
`../vulkan/install-runtime.py`; the gaming session then selects Zink for
Xwayland and OpenGL clients exactly as the ARM64EC session does. This
qualifies neither game rendering nor Vulkan conformance.

`lsof` is required for Steam to authenticate its loopback WebUI connections.
The image uses the accepted 4 KiB x86-64 kernel and the `uml-darwin` emulator;
there is no 16 KiB Steam product.

## Input pinning

[`sources.json`](sources.json) locks the Debian suite, the Steam release
channel, the pinned client version, and the Steam runtime snapshot.
[`steam-client.manifest`](steam-client.manifest) is the unmodified upstream
`ubuntu12` client manifest for client version `1788652215` — the same client
release the ARM64EC appliance seeds, so a first-boot comparison measures the
guest and not a different starting point. Valve serves only the current
manifest, so the pinned one is retrieved from the SteamDB SteamTracking mirror
recorded in `steam_manifest_origin`; every package it names is then downloaded
from Valve's own content service and verified against the SHA-256 in the
manifest, and the manifest itself is verified against `steam_manifest_sha256`.
The mirror's copy of the *current* manifest is byte-identical to the one
Valve's service serves, which is how the mirror was qualified.

The client seeded in the image is therefore one release behind Valve's current
publication, and the first boot downloads that difference like the ARM64EC
appliance does. Debian package versions are resolved during preparation and
recorded with the base archive digest.

## Build on native x86-64 Linux

Use an ordinary account with passwordless sudo on Debian 13. Preparation is
the only network stage; it installs host tools, debootstraps the base, resolves
the Debian package closure (Gamescope from `trixie-backports`), and downloads
the pinned Steam payloads. This prototype runs preparation from the producer
directory instead of through a `dependencies/prepare.sh --product` selector.

```sh
tools/rootfs/steam-x86_64/prepare-inputs.py --root /work/steam-x86-inputs
tools/rootfs/steam-x86_64/build.sh --inputs /work/steam-x86-inputs \
  --work /work/steam-x86-image --output /work/steam-x86_64-gaming.erofs
```

For the Vulkan variant, add `--vulkan-root` and `--vulkan-receipt` pointing at
a sealed x86-64 Vulkan guest tree and its receipt. Build work and output paths
must be absent. `prepare-inputs.py --root PATH --verify` performs read-only
payload verification and is what `build.sh` runs before it touches anything;
`--no-fetch` has the same verification-only behaviour.

Preparation produces two roots: `base.tar.zst` is the image runtime closure,
and `builder.tar.zst` is that closure plus its development packages. The
splash renderer is compiled offline in the builder chroot, so it links against
exactly the libraries the image ships; the builder never enters the image.

## Image and startup contract

The output carries SHA-256, package and binary inventories, and input
provenance sidecars. The audit requires the native x86-64 Steam client and web
helper, Gamescope, the splash renderer, the GTK/OpenAL/libnm aliases the native
Steam UI needs, and the V4L2 decoder rule; it rejects foreign ELF payloads,
system i386 payloads, an installed desktop environment, a bundled compatibility
tool under `/opt`, and a guest FEX engine.

`steam-gaming.service` starts a Gamescope session as the image-owned `uml`
user, which has passwordless local sudo and serial autologin on `ttyS0`. The
EROFS root stays read-only; a separate raw GPT disk holds one ext4 partition
mounted at `/userdata`, whose `home`, `var` and `root` directories are bind
mounted at `/home`, `/var` and `/root`. `/run` and `/tmp` are tmpfs and there
is no root OverlayFS. Package metadata is image-owned, while the Steam client
installation, games, settings and saves live on the data partition. The
image-owned init checks ext4 before mounting, formats only a new partition
carrying the launcher's initialization marker, repairs existing ext4 with
`e2fsck -p`, and stops boot with a visible recovery message rather than erasing
an unrecognized disk. The first boot copies the bundled client through a
staging directory and publishes it only after the copy completes; later boots
never reseed an existing home.

The Kernarium splash appears on the boot framebuffer while storage is checked
and the seed is copied, and Gamescope then shows the matching installation
screen with real Steam download and extraction progress. The session is ready
when the X window `Steam Big Picture Mode` becomes viewable.

## Video decode boundary

Big Picture plays `steamui/movies/bigpicture_startup.webm` on every start here
as it does on the ARM64EC appliance, and the
[shared assets](../steam-common/README.md) record why neither image decodes it
in hardware. Two things are specific to this client:

- Valve's x86-64 CEF has no stateful V4L2 decoder, so the image's
  `/dev/video-dec0` is never a candidate for it. Its only hardware decode path
  is VA-API, which it does attempt: `libva.so.2` and `libva-drm.so.2` are
  mapped into its GPU process during a session.
- No VA driver exists for this device and the image ships none, so that attempt
  can only fail. Supplying one means writing a libva backend over the Virtio
  video stateful decoder — `VAProfileVP9Profile0` with `VAEntrypointVLD` —
  which neither Debian nor upstream provides.

Even with such a backend the client would still decode in software today,
because Chromium's decode pipeline first requires dma-buf-backed NV12 buffers
that this guest's Zink-on-Venus GBM cannot export. Fix that before investing in
a VA driver.

## Interactive boot

```sh
UML_GPU_EXPERIMENTAL_VULKAN=1 python3 tools/rootfs/steam-x86_64/boot.py \
  --emulator /path/to/uml-darwin \
  --rootfs /path/to/steam-x86_64-gaming.erofs \
  --userdata /path/to/your-userdata.img --size-gib 32 \
  -- --video --gpu-display=native
```

The launcher owns the storage, init, console, memory and engine arguments and
rejects them in the trailing pass-through list. It creates a sparse GPT disk
only when `--userdata` is absent and holds an exclusive launch lock; the size
option applies at creation only and never resizes or resets existing data.
Keep the data disk backed up separately from the replaceable rootfs.
