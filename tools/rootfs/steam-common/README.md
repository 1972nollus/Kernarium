# Shared Kernarium Steam appliance assets

Both Steam appliances — the [ARM64EC](../steam-arm64ec/README.md) and the
[pure x86-64](../steam-x86_64/README.md) one — install from this directory.
It holds only the parts that do not depend on the guest architecture, so a
change to the startup or storage contract reaches both images at once.

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

## Hardware video decode

The decoder rule gives both images `/dev/video-dec0`, where a Linux Chromium or
CEF build looks for a stateful V4L2 decoder. What each image does with it
differs, and that difference belongs to the Steam client, not to this project:

- The ARM64 client's CEF has that decoder and asks for it. Its `steamwebhelper`
  processes run with `--enable-features=PlatformHEVCDecoderSupport,V4L2VideoDecode`
  and open and enumerate the node while the session starts.
- The x86-64 client's CEF has no stateful V4L2 decoder and offers only VA-API,
  for which no driver over this device exists.

Neither image plays a Steam startup movie. The `steamui/movies/*.webm` clips a
Steam Deck shows are hardware-specific; on this profile Big Picture starts
without one, and every movie file's access time is still its packaged value
after a first boot through the client's self-update and after a later boot. The
visible startup animation is this directory's own Kernarium splash, drawn with
GBM/EGL, which is not a video stream. No startup movie is decoded, in hardware
or otherwise, and no change to these images would produce one.
