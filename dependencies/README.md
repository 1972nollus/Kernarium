# External dependencies

All project-owned external-library source identities, patch series, platform
receipts, preparation logic, build recipes, and prefix verifiers live in this
directory. Kernel provenance remains separately owned by
[`../kernel/`](../kernel/README.md).

Production never selects third-party libraries from Homebrew, another package
prefix, `CMAKE_PREFIX_PATH`, or ambient `pkg-config` search paths. Every source
row in [`manifest.tsv`](manifest.tsv) names:

- one upstream HTTPS Git repository;
- one full immutable commit ID;
- the products that consume it;
- the source paths required from that commit; and
- either no project change or one ordered Git mail-patch series.

There is no source-archive URL, archive filename/hash, extracted-root name, or
prepared-tree hash. The pinned Git commit is the unmodified source identity;
stable patch IDs, patch byte hashes, subjects, order, and replay topology are
the maintained-change identity.

## Prepare and build

`prepare.sh` is the only external-library workflow allowed to access the
network. It fetches exact commits into `.build/dependency-cache/git`, publishes
prepared repositories below `external/sources`, removes their network remotes,
and applies declared topics with `git am --3way`. Verification requires a clean
repository, the configured repository/commit/sparse-path/series identities,
and exact commit subjects and stable patch IDs.

```sh
# macOS production libraries
dependencies/prepare.sh
dependencies/build.sh

# All products, including iOS inputs
dependencies/prepare.sh --product all

# Offline preparation or read-only verification
dependencies/prepare.sh --product macos --no-fetch
dependencies/prepare.sh --product macos --verify
```

An existing mismatched source repository or prefix is never replaced. Move it
aside after reviewing the diagnostic, then prepare or build again. Downstream
build scripts are offline and copy prepared source without `.git` metadata.

The macOS builder publishes one verified private non-graphics static prefix at
`.build/dependencies/macos-arm64`. Its ordered recipe list must exactly match
the `macos` manifest rows. Git checkouts of libffi and libiconv omit generated
Autotools outputs, so the builder regenerates them locally. Libiconv uses the
separately pinned Gnulib commit in the same manifest; neither bootstrap step
uses the network.

The private build environment restricts `pkg-config` to the staging prefix,
disables Meson’s optional ambient CMake fallback, and removes unused CMake and
libtool metadata before publication. The verifier checks the current manifest
and patch policy, receipt, required headers, static-only payload, arm64 format,
deployment target, and absence of symlinks or undeclared libraries.

## Apple GPU and iOS network prefixes

One `apple-gpu` product owns the ANGLE/Metal graphics source matrix for macOS,
iOS Simulator, and iOS device. The iOS network product remains separate:

```sh
dependencies/prepare.sh --product apple-gpu
dependencies/build-apple-gpu.sh --platform macos
dependencies/build-apple-gpu.sh --platform ios-simulator
dependencies/build-apple-gpu.sh --platform ios

dependencies/prepare.sh --product ios-network
dependencies/build-ios-network.sh
dependencies/build-ios-network.sh --platform ios

# Read-only installed-prefix checks
dependencies/build-apple-gpu.sh --platform macos --verify-only
dependencies/build-apple-gpu.sh --platform ios-simulator --verify-only
dependencies/build-apple-gpu.sh --platform ios --verify-only
dependencies/build-ios-network.sh --verify-only
dependencies/build-ios-network.sh --platform ios --verify-only
```

The published locations are:

```text
.build/dependencies/apple-gpu/macos-arm64
.build/dependencies/apple-gpu/ios-simulator-arm64
.build/dependencies/apple-gpu/ios-device-arm64
.build/dependencies/ios-simulator-arm64/network
.build/dependencies/ios-device-arm64/network
```

Graphics uses the exact UTM WebKit/ANGLE, libepoxy, and virglrenderer commits
from the central manifest. The WebKit checkout is sparse to the ANGLE,
configuration, and ccache paths used by the build. The prefix contains static
libepoxy/virglrenderer plus flat `EGL.framework` and `GLESv2.framework`
payloads. All three format-3 contracts select `gl_provider=angle-metal`, the
same renderer features, and one common offline recipe. Prefix verification
compares the installed manifest, ordered series, and patch bytes; rejects a
wrong architecture, Mach-O platform, deployment target, install name, payload,
runtime closure, symlink, `LC_SUB_CLIENT`, or legacy CGL/EAGL/Vulkan provider
symbol; and never replaces an invalid existing prefix. The macOS executable
stages exactly those two authenticated frameworks beside itself, while
Kernarium packaging revalidates the Simulator and device prefixes before
forming XCFrameworks. No UTM checkout, manifest, or graphics builder remains
under `Kernarium/`, and the retired `build-ios-gpu.sh` has no compatibility
entry point.

The maintained UTM WebKit series maps GLSL boolean `mix` to Metal component
selection. This preserves exact selected bit patterns, including subnormal
float values used to carry integer bits, instead of passing them through
arithmetic interpolation.

For the maintained arm64 Simulator slice, the series also enables Metal
swizzled texture views even though Simulator omits the ordinary GPU-family
flags. This preserves GLES legacy alpha, luminance, and depth sampling. Its
R/RG/RGBA 32-bit-float filterability follows Metal's device query on hardware
and the validated native Simulator path; the latter reports the query as false
but successfully executes linear filtering. Keep both exceptions scoped to the
arm64 Simulator product rather than generalizing them to other architectures.

The maintained UTM virglrenderer series also keeps the GLES format inventory
truthful: it does not advertise `VIRGL_FORMAT_Z32_UNORM`, whose GLES fallback
would otherwise be only `GL_DEPTH_COMPONENT24`. Genuine floating-point
32-bit depth remains available through `VIRGL_FORMAT_Z32_FLOAT`, and the
native desktop `Z32_UNORM` mapping is unchanged.

Network uses the pinned PCRE2, libffi, proxy-libintl, GLib, GVDB, and libslirp Git
sources. Its GLib source is a separately named prepared repository because the
iOS availability topic is applied only to that product. Platform contracts are
under [`platforms/`](platforms/) and every installed product prefix carries
the central manifest, applicable series and patch bytes, and its platform
contract or receipt.

Kernarium’s SwiftTerm dependency remains an exact Swift Package Manager input
to the Xcode application target. Xcode requires its declaration and
`Package.resolved` in the project/workspace; those are consumer project state,
not a project-owned source preparation or library build recipe. SwiftTerm does
not enter the emulator libraries or macOS dependency graph.

## Patch maintenance

Checked-in `.patch` files are generated artifacts. Never edit one manually,
even for a hunk count, subject, or context adjustment. Git commits are the
only editing authority.

For a dependency with a maintained series:

1. Check out its pinned manifest commit and create a topic branch.
2. Replay the current series with `git am` if the branch was not prepared by
   `prepare.sh`.
3. Use `git rebase -i <pinned-commit>` to reorder, squash, edit, or drop topic
   commits, and amend the commits themselves.
4. Regenerate the checked-in series and its stable IDs/hashes/subjects:

   ```sh
   dependencies/rebase-patches.sh \
     --dependency utm-webkit --source /path/to/topic-checkout
   ```

The regenerator requires a clean linear branch, performs an explicit rebase
onto the pinned upstream commit, invokes `git format-patch`, replaces the old
mail files, and rewrites `series.tsv`. `prepare.sh --validate-manifest-only`
then verifies the complete central patch inventory.

## Prerequisites and overrides

Apple builds require CMake 3.27+, Ninja, GNU Make, Clang/Xcode command-line
tools, Meson, Python 3, `pkg-config`, Git, rsync, Autoconf, Automake, gettext
(`autopoint`), GNU libtool, and gperf. Build-tool versions and the real SDK,
compiler, and build timestamp remain observations rather than normalized
source identities.

```sh
dependencies/prepare.sh --root /path/to/sources --cache /path/to/cache
dependencies/build.sh --source-root /path/to/sources --prefix /path/to/prefix
cmake -S . -B build -DUML_DEPENDENCY_PREFIX=/path/to/prefix
```

Production consumers import exact headers and static libraries through
[`cmake/UmlThirdPartyTargets.cmake`](cmake/UmlThirdPartyTargets.cmake) and the
GPU-only [`cmake/AppleGpuDependencies.cmake`](cmake/AppleGpuDependencies.cmake).
The unit-only 16 KiB and 4 KiB presets do not require the private desktop
prefix; GPU presets require both the non-graphics macOS prefix and the matching
Apple-GPU prefix, but still require neither a kernel build nor a rootfs.
