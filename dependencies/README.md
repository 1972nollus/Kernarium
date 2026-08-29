# External dependencies

This directory owns external-library source identities, generated patch
series, preparation, offline build recipes, platform contracts, and installed
prefix verification. Kernel provenance is separate under
[`../kernel/`](../kernel/README.md).

[`manifest.tsv`](manifest.tsv) is the exact source inventory. Each row pins one
upstream Git commit, the products that consume it, the required source paths,
and any ordered local patch series.

Production must not discover libraries from Homebrew, ambient
`CMAKE_PREFIX_PATH`, or unrestricted `pkg-config`. Consumers use only verified
private prefixes through the CMake targets in this directory.

Dependency builds keep Xcode DerivedData inside their disposable work trees;
they do not populate the user's global Xcode DerivedData directory.

## Products

The dependency products are:

- `macos`: non-graphics static host libraries at
  `.build/dependencies/macos-arm64`;
- `apple-gpu`: common ANGLE/Metal, libepoxy, and virglrenderer inputs for
  macOS, Simulator, and device; and
- `ios-network`: the iOS GLib/libslirp network closure; and
- `ios-storage`: the patch-managed GPT fdisk core as one static arm64
  XCFramework with iOS device and Simulator slices.

The Apple GPU prefixes contain static renderer libraries plus authenticated
flat `EGL.framework` and `GLESv2.framework` payloads. All Apple platforms use
the same ANGLE/Metal provider contract. Platform-specific patches remain
scoped by the checked contracts and series rather than being described as
general hardware capability.

SwiftTerm remains an Xcode Swift Package dependency. It is not part of the
emulator library prefix.

## Prepare, build, and verify

`prepare.sh` is the only dependency command allowed to access the network. It
fetches pinned commits into the local cache and creates verified prepared
repositories. All build scripts are offline.

```sh
dependencies/prepare.sh
dependencies/build.sh

dependencies/prepare.sh --product apple-gpu
dependencies/build-apple-gpu.sh --platform macos
dependencies/build-apple-gpu.sh --platform ios-simulator
dependencies/build-apple-gpu.sh --platform ios

dependencies/prepare.sh --product ios-network
dependencies/build-ios-network.sh
dependencies/build-ios-network.sh --platform ios

dependencies/prepare.sh --product ios-storage
dependencies/build-ios-storage.sh
dependencies/build-ios-storage.sh --verify-only
```

Use `--no-fetch` for offline preparation and `--verify` or `--verify-only` for
read-only checks. Each script's `--help` owns non-default paths and options.

An invalid prepared tree or installed prefix is never replaced automatically.
Review the diagnostic and move the old path aside before rebuilding. Prefix
verification checks manifest/series identity, architecture, Apple platform and
deployment target, expected static payloads, framework install names, and the
absence of undeclared or legacy providers.

The iOS storage build exports only the two file-oriented C entry points used by
Kernarium. Its archive excludes every GPT fdisk command-line front end and
`popt`. The standalone macOS Android launcher invokes an installed Homebrew
`sgdisk` executable instead; it does not link or discover a Homebrew library.

## Patch maintenance

Patch files are generated mail artifacts. Modify the clean Git topic branch,
then regenerate the owning series:

```sh
dependencies/rebase-patches.sh \
  --dependency NAME --source /path/to/topic-checkout
```

The regenerator checks linear history against the pinned commit and updates the
series manifest, stable patch IDs, hashes, subjects, and order. Never edit patch
bytes or `series.tsv` by hand.

Changing `manifest.tsv`, a source identity, a patch series, or a build recipe
invalidates the corresponding installed receipt. Rebuild the affected prefix
and run its tooling checks in the same change.

## Host prerequisites

Dependency preparation and builds require the Apple command-line tools plus
CMake, Ninja, GNU Make, Meson, Python 3, `pkg-config`, Git, rsync, Autoconf,
Automake, gettext, GNU libtool, and gperf. Exact tool and SDK versions remain
observed build metadata; source identity comes from the manifest and generated
series.
