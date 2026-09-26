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

Apple GPU builds reuse one Xcode DerivedData directory below their shared work
root; they do not populate the user's global Xcode DerivedData directory.

## Products

The dependency products are:

- `macos`: non-graphics static host libraries at
  `.build/dependencies/macos-arm64`;
- `apple-gpu`: the upstream libepoxy and virglrenderer sources alone. It
  publishes no host GL provider, so no product consumes it directly;
- `apple-gpu-vulkan-experiment`: the macOS and iOS device builds, which add the
  pinned Mesa, Vulkan-Headers, and Venus-protocol sources so that Mesa supplies
  both the KosmicKrisp Vulkan driver and the Zink Gallium OpenGL driver. There
  is no iOS Simulator build: KosmicKrisp requires Metal 4 and the Simulator
  exposes no Metal 4 device;
- `ios-network`: the iOS GLib/libslirp network closure;
- `ios-storage`: the patch-managed GPT fdisk core as one static arm64
  `GPTFDisk.xcframework` with iOS device and Simulator slices;
- `ios-compression`: the pinned, decode-only zstd library as one static arm64
  `LibZstd.xcframework` with iOS device and Simulator slices; and
- `x86-conformance`: the test-only FEX and K x86-64 semantics instruction
  corpora plus their pinned producer inputs, used to generate the checked-in
  self-contained x86 unit corpus and qualify x86 gadget
  semantics. The macOS 4 KiB x86 backend also consumes the Berkeley SoftFloat
  3e and 128-bit Cephes archives carried by the pinned FEX source; no external
  conformance runtime code is linked.

The Apple GPU prefixes contain static renderer libraries plus authenticated
flat framework payloads for exactly one host GL provider. The prefix carries
`MesaEGL.framework` and `MesaGallium.framework`, whose Zink Gallium driver
renders desktop OpenGL 4.6 core on the packaged `KosmicKrisp.framework`. Each
prefix's `gl_provider` contract field names the one it carries. Mesa's GLSL grammars declare two Bison 3 directives that
Apple's Bison 2 does not accept, so the macOS OpenGL build drives the host
generator through `dependencies/bison2-grammar-shim.sh` rather than forking
the grammars. Platform-specific patches remain scoped by the checked contracts
and series rather than being described as general hardware capability.

SwiftTerm remains an Xcode Swift Package dependency. It is not part of the
emulator library prefix.

## Prepare, build, and verify

`prepare.sh` is the only dependency command allowed to access the network. It
fetches pinned commits into the local cache and creates verified prepared
repositories. All build scripts are offline.

```sh
dependencies/prepare.sh
dependencies/build.sh

dependencies/prepare.sh --product apple-gpu-vulkan-experiment
dependencies/build-apple-gpu.sh \
  --product apple-gpu-vulkan-experiment --platform macos

dependencies/prepare.sh --product ios-network
dependencies/build-ios-network.sh
dependencies/build-ios-network.sh --platform ios

dependencies/prepare.sh --product ios-storage
dependencies/build-ios-storage.sh
dependencies/build-ios-storage.sh --verify-only

dependencies/prepare.sh --product ios-compression
dependencies/build-ios-zstd.sh
dependencies/build-ios-zstd.sh --verify-only

dependencies/prepare.sh --product x86-conformance
```

Use `--no-fetch` for offline preparation and `--verify` or `--verify-only` for
read-only checks. Each script's `--help` owns non-default paths and options.

Series replay commits with the Git identity the environment supplies and
refuses to run when none is configured. See
[`kernel/mainline-6.18/README.md`](../kernel/mainline-6.18/README.md) for the
shared contract.

A prepared source is identified by the tree its series records in
`series.tsv`. A prepared source whose tree no longer matches is replaced only
by `prepare.sh --recreate`, and only when it is clean and its branches point
at commits the Git cache already holds; anything else is refused with a
diagnostic. An installed prefix is never replaced automatically: build the new
one beside it. Prefix verification checks the manifest rows, each patched
source's tree in `share/uml-dependencies/source-trees.tsv` against its series,
architecture, Apple platform and deployment target, expected static payloads,
framework install names, and the absence of undeclared or legacy providers.
Every iOS prefix builds and verifies at an iOS 26.0 deployment target.

Product-specific prefixes compare only the manifest rows consumed by that
product. Adding an unrelated dependency does not invalidate an accepted
prefix, while any change to one of its own rows still requires a rebuild.

## Experimental Vulkan composition

`apple-gpu-vulkan-experiment` is a separately named product built for macOS
and for the iOS device, from one source matrix and one set of patches; its
`--platform ios` build differs only in SDK, deployment target, platform
contract, and its need for a native-machine pass for the compilers Mesa runs
during the build. It builds the `apple-gpu` source matrix -- upstream
virglrenderer and libepoxy --
and adds Mesa, which supplies both the KosmicKrisp Vulkan driver and the Zink
Gallium OpenGL driver, plus the standalone Vulkan SDK headers and the Venus
protocol generator.

Its format-4 platform contract sets `gl_provider=zink-kosmickrisp` and an
independent `vk_provider=venus-kosmickrisp`. The build compiles KosmicKrisp as
`KosmicKrisp.framework`, injects its `vkGetInstanceProcAddr` directly into
Venus, and disables host Vulkan-loader discovery and downstream downloads.
Prefix and runtime verification require exactly that provider set and reject a
second Vulkan provider, an ambient loader, a platform mismatch, a source tree
its series no longer records, or a duplicate framework copy. Host builds generate and test
the protocol from the authenticated SDK registry before compiling Venus;
generated codecs are build outputs rather than installed payload, and the
prefix records the generator source tree and generated-header digest.

The maintained virglrenderer series and the guest Mesa series under
`tools/rootfs/vulkan/` form one compatibility epoch. The host translates
KosmicKrisp's unified native memory type into distinct guest-visible
device-local and host-visible choices, and the paired guest exposes a shared
host allocation through virtio-gpu opaque-fd and dma-buf handles without a CPU
staging copy. Changing either side invalidates the experimental prefix and the
Vulkan rootfs receipt. This pairing is local to this project and is not a
formal Vulkan conformance claim.

The Venus protocol series is owned by
`tools/rootfs/vulkan/patches/venus-protocol`. The manifest's explicit
`rootfs-vulkan:` namespace references that single owner without permitting
parent-directory paths, unknown owners, or symlink traversal; no second copy
exists under `patches/`.

That series is patch-managed like every other dependency, and its editable
source is a replay clone of the pinned generator repository whose history is
the pinned commit followed by the maintained topic commits.
`tools/rootfs/vulkan/rebase-patches.sh --component venus-protocol --source
CHECKOUT` regenerates the checked-in series from that clone; amend a topic in
the clone rather than editing a generated `.patch` file. The series carries
generator inputs only - the extension list, the command-id registry and the
templates - never generated output: both halves of the encoder are produced at
build time from the one pin, so a topic that checked in a generated header
would be rejected by `tools/rootfs/vulkan/verify-inputs.py`. Because the guest
driver and the renderer generate their halves from the same pin, a protocol
topic changes both sides at once and needs a matching guest epoch.

KosmicKrisp's Mesa regressions are enabled with `-Dbuild-tests=true` and
`-Db_ndebug=false` and selected with
`meson test -C BUILD --suite kosmickrisp`; the common Vulkan command-lowering
regressions use `--suite vulkan`. Assertions must stay active in those builds.

KosmicKrisp never hands Metal a texture view it cannot make, because Metal
answers one with an assertion that ends the process. An image view whose type,
levels or layers its image cannot provide fails with
`VK_ERROR_OUT_OF_DEVICE_MEMORY`, except that a 3D view of a non-3D image is made
as the 2D or 2D-array texture the image is: Venus creates views without waiting
for the result, so a refusal would surface later as a lost device. Metal still
decides pixel-format compatibility. The same invalid views through
`vkCreateImageView` trip the common runtime's assertions, so
`kosmickrisp_invalid_view_type` checks them there only in a build without
assertions and checks the texture bridge directly in the regression suite.

Programmable primitive IDs cross the Metal vertex/fragment interface as flat
user varyings. Layer and viewport indices use unsigned native Metal attributes
without interpolation qualifiers.

KosmicKrisp gates unsatisfied timeline waits on the CPU before native Metal
submission, using the common Vulkan runtime's on-demand submit worker. This
keeps delayed host signals from outliving Metal's finite GPU event-wait window;
`MESA_VK_ENABLE_SUBMIT_THREAD=false` still preserves that required fallback.
Only a wait whose signal has not been submitted is gated. A value already
signalled on the CPU or enqueued on the queue's Metal timeline is pending, so
ordinary dependencies keep the immediate path, the worker stays off, and
`vkQueueSubmit` never waits for the GPU work its batch depends on. A binary
semaphore wait is pending by definition because its signal must have been
submitted first. That distinction is what keeps a gated batch from blocking
the caller that has to release it. A timeline wait whose signal is submitted
later to the same queue remains unsupported: the device exposes one queue and
submits in order. CPU waits preserve their absolute deadlines and observe
device loss so pending submissions and queue drains can shut down before
device resources are released.

A render pass clear depth reaches Metal already rounded to the attachment
format's nearest representable value and offset inside that texel's interval.
Metal converts the clear into a normalized-integer depth attachment by
truncating, so an unrounded value would clear `D16_UNORM` one step low and
disagree with the rasterizer's own depth writes, which round. Depth formats
Metal stores as floats keep their exact value.

Subgroup operations reach Metal's SIMD groups, which exist in kernel and
fragment functions only, so the driver advertises compute and fragment as its
subgroup stages. A vertex, tessellation or geometry shader that reads the
subgroup builtins or uses a group operation still compiles: those stages
present the single-invocation subgroup their properties imply, with size and
count one, invocation zero, and every group operation returning the value that
one invocation contributed.

The shader outputs Metal names itself keep the shape Metal gives them, not
the shape the shader's stores imply: the clip coordinate is four components
even when a shader writes only `gl_Position.xy`, point size is one float, the
layer and viewport indices are unsigned, and the clip and cull distances are
float arrays. A component no store reaches keeps the value the output is
initialized with, which for the clip coordinate is the identity `(0, 0, 0, 1)`
rather than a degenerate zero `w`.

Linked graphics pipelines share native programs and dynamic vertex-input
variants only while a live Vulkan pipeline owns them. The lookup cache drops
its entry and releases those resources when the last pipeline owner is
destroyed, so obsolete programs do not accumulate until device teardown.
Pipeline statistics use draw-time guards in the same executable, including
its bounded raster variants; query begin and end never compile or retain a
second shader tree. A discard variant preserves application shader effects and
transform feedback without raster-only materialization or provoking-vertex
conversion. Dynamic discard changes select the corresponding precompiled
variant, including after pipeline-cache reload.
Metal bridge constructors return one owned, initialized object. Callers release
that object once; constructors must not initialize it again after `new`.

One-time command submissions transfer native command buffers and allocators to
completion-owned storage, which releases their capacity after the GPU finishes.
Vulkan command-buffer reset with `RELEASE_RESOURCES` also drops allocator
capacity; later recording recreates it. Feedback reporting remains valid after
Vulkan device teardown through a separate reference-counted context.

`MESA_KK_DEBUG=submit_timing` enables optional raw Metal commit-feedback
diagnostics on stderr: an opaque device identity, GPU start and end host
seconds, and the commit error code for the whole committed command-buffer
batch. It is not a Vulkan timestamp-query result, adds no GPU commands, and is
absent from the disabled path.

The iOS storage build exports only the two file-oriented C entry points used by
Kernarium. Its archive excludes every GPT fdisk command-line front end and
`popt`. The standalone macOS Android launcher invokes an installed Homebrew
`sgdisk` executable instead; it does not link or discover a Homebrew library.
The zstd build contains the streaming decompressor but excludes compression,
dictionary-builder, legacy-frame, deprecated, and command-line code. Both
libraries use static framework slices so their Clang module maps remain
separate when Xcode processes the XCFrameworks.

Additional build-only archives use
`dependencies/prepare.sh --archive-manifest PATH --cache CACHE`. The tab-separated
manifest has `name`, `url`, and `sha256` columns; HTTPS downloads are verified
before publication as `CACHE/archives/SHA256-NAME`. `--no-fetch` and `--verify`
require those same authenticated cache entries. Archive preparation installs
nothing; package installation belongs in an isolated build environment.

To discover a release tag's immutable source identity, use
`dependencies/prepare.sh --resolve-ref REPOSITORY refs/tags/TAG`. The result
peels annotated tags; record that object ID in the source manifest before
preparation. Ref discovery does not update a manifest or prepared checkout.
When provenance requires the annotated tag object itself, use
`dependencies/prepare.sh --materialize-tag REPOSITORY REF OBJECT COMMIT CHECKOUT`.
It accepts an exact `refs/tags/` ref, verifies the pinned tag object and peeled
commit, and publishes only that tag without changing the checkout's HEAD or files.

## Patch maintenance

Patch files are generated mail artifacts. Modify the clean Git topic checkout,
then regenerate the owning series:

```sh
dependencies/rebase-patches.sh \
  --dependency NAME --source /path/to/topic-checkout
```

The regenerator exports the pinned commit's descendants through
`scripts/patch_series.py`, which rewrites only the patches whose commits
changed and records the resulting tree in `series.tsv`. Never edit patch bytes
or `series.tsv` by hand.

Changing `manifest.tsv` or a build recipe invalidates the corresponding
installed receipt, and so does a series change that produces a different tree.
A series change that keeps the tree, such as a reworded message or a folded
follow-up, leaves every prefix valid. Rebuild an invalidated prefix and run its
tooling checks in the same change.

## Host prerequisites

Dependency preparation and builds require the Apple command-line tools plus
CMake, Ninja, GNU Make, Meson, Python 3, `pkg-config`, Git, rsync, Autoconf,
Automake, gettext, GNU libtool, and gperf. Exact tool and SDK versions remain
observed build metadata; source identity comes from the manifest and generated
series.
