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
