# Shared Kernarium Steam appliance assets

The [x86-64 Steam appliance](../steam-x86_64/README.md) installs from this
directory. It holds the parts that do not depend on the guest architecture:
the startup, storage and launch contract that the boot benchmark and the
producer share.

- `overlay/` is copied into the image root before the producer's own overlay:
  the image-owned init and its storage preparation, the Gamescope session
  supervisor, the splash hand-off unit and service, and the V4L2 decoder
  discovery rule.
- `splash/` is the C++20 Kernarium boot and installation renderer. Each
  producer compiles it natively inside its own verified toolchain; the build
  refuses to run outside one.
- `launcher.py` owns the persistent GPT user-data disk and the kernel command
  line. Each producer's `boot.py` adds only the emulator options its guest
  needs, and the launcher rejects those and the storage, init, console and
  memory arguments in a caller's pass-through list.

## The startup movie is decoded in software

Big Picture plays `steamui/movies/bigpicture_startup.webm`, a VP9 Profile 0
movie, on every start of this device profile, and plays it with CEF's software
decoder. Chromium's VA-API decode pipeline needs dma-buf-backed NV12 frames,
and the guest's Zink-on-Venus GBM does not export dma-bufs, so it advertises no
decode configuration; the decoder node, its udev rule and the device's VP9
advertisement are not the blocker. The session supervisor in `overlay/` starts
the client with Valve's `-cef-enable-vaapi` when the session sets
`KERNARIUM_CEF_VAAPI=1`; the VA-API backend and how the x86-64 image selects it
are in [`tools/video/va-v4l2`](../../video/va-v4l2/README.md#in-the-steam-x86-64-image).
