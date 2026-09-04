# ARM64EC Wine XFCE appliance

This producer builds the userspace for the dedicated `wine-arm64ec` kernel and
emulator profile. The Debian 13 root is ARM64-only and contains a minimal XFCE
session, Xorg, and source-built Wine 10 configured for
`i386,arm64ec,aarch64`. Windows x86-64 instructions cross the private
ARM64EC `xtajit64.dll` bridge into the emulator's in-process x86 gadget
engine. The native ARM64 `xtajit.dll` WoW64 bridge sends i386 contexts to the
same engine and dispatches Windows system and Wine Unix calls back to native
ARM64 Wine. The i386 tier is PE-only: there is no x86 Linux userspace,
foreign Debian architecture, or nested emulator.

Prepare the pinned Wine source and ARM64EC compiler through the dependency
workflow, then run the producer on native AArch64 Debian as an ordinary user
with passwordless sudo, `debootstrap`, Wine's build dependencies, and
`erofs-utils`:

```sh
dependencies/prepare.sh --product wine-arm64ec-rootfs
dependencies/prepare.sh --toolchain llvm-mingw-arm64ec
tools/rootfs/wine-arm64ec/build.sh \
  --work /path/to/new-work-directory \
  --output /path/to/wine-arm64ec-xfce.erofs \
  --wine-source /path/to/external/sources/wine \
  --toolchain /path/to/llvm-mingw-arm64ec-20250920-linux-aarch64
```

The build defaults to two compile jobs so it fits a 4 GiB ARM64 builder; pass
`--jobs` only when more memory is available. Upstream regression binaries are
disabled because this is a production rootfs build, while the appliance's own
AMD64 and i386 execution probes remain mandatory.

The adjacent package, ELF, PE, and provenance inventories are part of the
artifact. ELF verification permits only AArch64. PE verification recognizes
ARM64EC images from their validated CHPE metadata, permits native ARM64,
ARM64EC, ARM64X, and Wine-owned i386 PE images, and requires both CPU bridge
DLLs. The only pure AMD64 image is the installed exit-code-42 probe; it calls a
seven-argument WinAPI and receives a native-to-AMD64 callback so both
directions of the ARM64EC entry/exit-thunk stack contract are mandatory. The
installed i386 exit-code-43 probe independently qualifies WoW64. LightDM
automatically starts the `uml` account's XFCE session, and the UML serial
console automatically opens the same appliance-owned account. The account has
no password and has passwordless sudo for qualification.

Image names use the reserved `wine-arm64ec-*.erofs` form. Kernarium uses that
identity to select the separate 4 KiB Wine runtime before opening the image.
Wine's stub-only `wdscore` module is omitted because its legacy C++ exports
cannot be represented as ARM64EC linker thunks.
