# Steam guest sources

This inventory owns the pinned Proton tree, its Wine submodule, and their
generated patch series. Guest inputs are separate from the Apple static
library inventory, so a Steam update does not invalidate host prefixes.

The Wine topics select the emulator's ARM64EC and i386 WoW64 modules and
provide a readable ARM64 signal restorer for native thread unwinding. The
Proton topics omit guest FEX, defer prefix creation to the emulator, enforce
the prepared Cargo lock, and retain a useful Git identity in shallow builds.
The producer copies the shared CPU bridge sources into its build overlay;
the patches do not duplicate those sources.

Edit a clean topic checkout, then regenerate its series with:

```sh
dependencies/steam-arm64ec/rebase-patches.sh --dependency proton --source /path/to/topic
```

Use `proton-wine` for the Wine topic. The shared regenerator validates the
upstream base, linear topic history, patch IDs, and checksums. These topics
are maintained locally for this project.

The [rootfs producer](../../tools/rootfs/steam-arm64ec/README.md) owns the
SDK and payload locks, preparation, image assembly, and qualification.
