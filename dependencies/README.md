# External dependencies

This directory owns external-library source identities, generated patch
series, preparation, offline build recipes, platform contracts, and installed
prefix verification. Kernel provenance is separate under
[`../kernel/`](../kernel/README.md).

[`manifest.tsv`](manifest.tsv) is the exact source inventory. Each row pins one
upstream Git commit, the products that consume it, its `source_paths` (always
`.`: every source is complete, and `prepare.sh` rejects any other value), and
any ordered local patch series.

Production must not discover libraries from Homebrew, ambient
`CMAKE_PREFIX_PATH`, or unrestricted `pkg-config`. Consumers use only verified
private prefixes through the CMake targets in this directory.

## Products

The dependency products are:

- `macos`: non-graphics static host libraries at
  `.build/dependencies/macos-arm64`;
- `apple-gpu-vulkan-experiment`: the macOS and iOS device builds, which add the
  pinned Mesa, Vulkan-Headers, and Venus-protocol sources so that Mesa supplies
  both the KosmicKrisp Vulkan driver and the Zink Gallium OpenGL driver. There
  is no iOS Simulator build (see
  [`../docs/graphics.md`](../docs/graphics.md#current-path));
- `ios-network`: the iOS GLib/libslirp network closure;
- `ios-storage`: the patch-managed GPT fdisk core as one static arm64
  `GPTFDisk.xcframework` with iOS device and Simulator slices;
- `ios-compression`: the pinned, decode-only zstd library as one static arm64
  `LibZstd.xcframework` with iOS device and Simulator slices;
- `kernel-host-tools`: pahole and the static libelf that the Darwin kernel
  build uses to generate BTF, at `.build/dependencies/kernel-host-tools`; and
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
prefix's `gl_provider` contract field names the one it carries. Under Apple's
Bison 2, the OpenGL build generates Mesa's GLSL parsers through
[`bison2-grammar-shim.sh`](bison2-grammar-shim.sh). Platform-specific patches
remain scoped by the checked contracts and series rather than being described
as general hardware capability.

The kernel host tools are patch-managed ports of elfutils, musl-obstack,
libbpf, and dwarves to Darwin, built with argp-standalone. elfutils' Git
source carries only the generators of its DWARF name and x86 decoder tables,
so the build first builds the pinned gawk as a build-only tool. The libbpf
that pahole embeds needs Linux UAPI headers, which come from a pinned Debian
`linux-libc-dev` package, and the Linux host APIs Darwin lacks, which come from
[`kernel/mainline-6.18/build/toolchain/darwin-host`](../kernel/mainline-6.18/build/toolchain/darwin-host/)
shared with Kbuild's host tools. The prefix publishes only `bin/pahole` and
libelf; the tools target macOS 15.4, the first release with the `strchrnul`
that elfutils calls.

The `macos` and `ios-network` prefixes build libslirp from one patch-managed
series, [`patches/libslirp`](patches/libslirp/series.tsv), so both record its
tree in `source-trees.tsv`. Its topics let a host forward's listening socket
queue up to the host's `SOMAXCONN` connections instead of one, keep the
advertised receive window within the buffer when the peer has passed the last
advertised edge (an LP64 widening bug), and drop a connection whose peer stops
answering window probes. Upstream master at 62b29862 has none of them; drop a
topic when a libslirp update absorbs it. The user-network contract they
provide is in [`../darwin/README.md`](../darwin/README.md).

The `macos` product carries FEXCore for the macOS-only x86-64 JIT engine. The
`fex` row applies one patch-managed series,
[`patches/fex`](patches/fex/series.tsv), and the `fmt`, `xxhash`, and
`unordered-dense` rows pin the three submodules FEXCore needs at the commits
FEX-2608 records; no iOS product names any of them. The series' topics build
only FEXCore for an embedding Darwin host, make its utilities portable to
Darwin, add an embedding mode in which a host that owns guest memory drives
FEXCore, compat32 code included, and correct x86 semantics where FEXCore
differs from hardware. The series touches nothing the `x86-conformance` corpus
producers read, so one prepared FEX source serves both products and still
yields the pinned corpus.

The prefix installs FEXCore's static archives (`libFEXCore.a`,
`libFEXCore_Base.a`, and `libJemallocDummy.a`, its allocator entry points over
the C library's), `libfmt.a`, `libxxhash.a`, FEXCore's public and generated
headers, and the fmt, xxHash, and unordered_dense headers they include.
`UML::ThirdPartyFEXCore` links them with the definitions FEXCore's headers
were compiled with. FEXCore calls the prefix's SoftFloat, so both are built
without the `preserve_all` ABI. On Apple, FEXCore maps, frees, and
write-enables executable memory only through hooks its embedder must install;
it has no `MAP_JIT` path of its own, and prefix verification rejects a FEXCore
archive that toggles JIT write protection.

Kernarium's serial console uses the `GhosttyTerminal` product of
[libghostty-spm](https://github.com/Lakr233/libghostty-spm), an Xcode Swift
Package that the Kernarium project pins to exactly `1.6.20260922`. Its
libghostty is a prebuilt binary XCFramework, built from Ghostty commit
`3c47ca15`, which Swift Package Manager fetches from the package's GitHub
release during Xcode package resolution and verifies against the checksum in
the package manifest. Neither it nor the package's MSDisplayLink dependency is
part of the emulator dependency prefixes.

## Prepare, build, and verify

`prepare.sh` is the only dependency command allowed to access the network. It
fetches pinned commits into the local cache and creates verified prepared
repositories. All build scripts are offline.

```sh
dependencies/prepare.sh
dependencies/build.sh

dependencies/prepare.sh --product apple-gpu-vulkan-experiment
dependencies/build-apple-gpu.sh --platform macos

dependencies/prepare.sh --product ios-network
dependencies/build-ios-network.sh
dependencies/build-ios-network.sh --platform ios

dependencies/prepare.sh --product ios-storage
dependencies/build-ios-storage.sh
dependencies/build-ios-storage.sh --verify-only

dependencies/prepare.sh --product ios-compression
dependencies/build-ios-zstd.sh
dependencies/build-ios-zstd.sh --verify-only

dependencies/prepare.sh --product kernel-host-tools
dependencies/prepare.sh --archive-manifest dependencies/kernel-host-tools/archives.tsv
dependencies/build-kernel-host-tools.sh
dependencies/build-kernel-host-tools.sh --verify-only

dependencies/prepare.sh --product x86-conformance
```

Use `--no-fetch` for offline preparation and `--verify` or `--verify-only` for
read-only checks. A build verifies its inputs before it creates the prefix's
parent or its work root, so a build that rejects them writes nothing. Each
script's `--help` owns non-default paths and options. A relative path, in an
option or an environment override, is relative to the caller's working
directory.

Series replay commits as whoever Git resolves for the caller, and export
refuses a topic whose author is a placeholder. See
[`kernel/mainline-6.18/README.md`](../kernel/mainline-6.18/README.md) for the
shared contract.

A prepared source is identified by the tree its series records in
`series.tsv`. A prepared source whose tree no longer matches is replaced only
by `prepare.sh --recreate`, and only when it is clean and its branches point
at commits the Git cache already holds; anything else is refused with a
diagnostic. An installed prefix is never replaced automatically: build the new
one beside it. Prefix verification checks the manifest rows, the tree in
`share/uml-dependencies/source-trees.tsv` of each patched source (every row of
the product with a patch series) against its series, architecture, Apple
platform and deployment target, expected static payloads, framework install
names, and the absence of undeclared or legacy providers. In iOS and Apple GPU
prefixes, every object of a checked binary, each archive member included, is
Mach-O with exactly one build-version command, so an LLVM bitcode member is
rejected.
Every iOS prefix builds and verifies at an iOS 26.0 deployment target.

Every prefix installs the manifest it was built from and compares only the
rows its product consumes, ignoring the products column, which records only
which prefixes share a row. Adding, removing, or sharing another product's
dependency does not invalidate a prefix, while any change to one of its own
rows still requires a rebuild.

## Experimental Vulkan composition

`apple-gpu-vulkan-experiment` is a separately named product built for macOS
and for the iOS device, from one source matrix and one set of patches; its
`--platform ios` build differs only in SDK, deployment target, platform
contract, and its need for a native-machine pass for the compilers Mesa runs
during the build. It builds upstream virglrenderer and libepoxy and adds Mesa,
which supplies both the KosmicKrisp Vulkan driver and the Zink Gallium OpenGL
driver, plus the standalone Vulkan SDK headers and the Venus protocol
generator.

Its format-4 platform contract sets `gl_provider=zink-kosmickrisp` and an
independent `vk_provider=venus-kosmickrisp`. The build compiles KosmicKrisp as
`KosmicKrisp.framework`, injects its `vkGetInstanceProcAddr` directly into
Venus, and disables host Vulkan-loader discovery and downstream downloads.
Prefix and runtime verification require exactly that provider set and reject a
second Vulkan provider, an ambient loader, a `dlopen` or `dlsym` import in
the renderer or KosmicKrisp, a platform mismatch, a source tree its series no
longer records, or a duplicate framework copy. libepoxy keeps its `dlopen`,
which its Apple topic limits to the packaged `MesaEGL.framework`. Host builds
generate and test the protocol from the authenticated SDK registry before
compiling Venus; generated codecs are build outputs rather than installed
payload, and the prefix records the generator source tree and generated-header
digest.

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

Host and guest build Mesa from one topic branch on the same pinned commit.
The guest's Mesa series, `tools/rootfs/vulkan/patches/mesa`, is its lower
segment, and `patches/mesa` holds only the host topics that follow it; the
manifest names both layers, and the host series records the tree of the
whole stack. A topic both need belongs in the guest segment and a host-only
topic in the host segment, so a KosmicKrisp change never alters the guest
tree its CTS receipts name. Every guest segment change also changes the host
tree and needs new host prefixes.

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
Apple's emulated `util_barrier` has `util_tests --gtest_filter='util_barrier.*'`
in `--suite util`, which runs in any `-Dbuild-tests=true` tree.

KosmicKrisp's runtime behaviour is specified in
[`../docs/graphics.md`](../docs/graphics.md#kosmickrisp-driver-contracts).
Metal bridge constructors return one owned, initialized object. Callers
release that object once; constructors must not initialize it again after
`new`.

KosmicKrisp's device library starts as SPIR-V the build does not compile: the
Apple product keeps LLVM out of its closure, so Mesa's "embed LLVM-free build
shaders" topic carries `libkk.spv` and `libpoly.spv` as `mesa_clc` output for
the OpenCL sources at the head of the Mesa series.
[`kosmickrisp-spirv/generate.py`](kosmickrisp-spirv/generate.py) produces
them on an arm64 Debian host that already has exactly the packages
[`kosmickrisp-spirv/toolchain.tsv`](kosmickrisp-spirv/toolchain.tsv) pins: it
builds `mesa_clc` from a clean topic checkout there, compiles both blobs, and
rewrites [`kosmickrisp-spirv/record.tsv`](kosmickrisp-spirv/record.tsv), which
names each blob, its kernels, and the inputs it was compiled from. Its output
is byte-for-byte reproducible. The `dependency-kosmickrisp-spirv` tooling test
fails when a topic changes one of those inputs, the embedded bytes, the Mesa
pin, or the toolchain without a regeneration. After such a change, generate
the blobs, amend them into the embed topic, and regenerate the series:

```sh
dependencies/kosmickrisp-spirv/generate.py generate \
  --source external/sources/mesa --host HOST --output DIR
```

`generate.py verify` is the full comparison against a checkout of the
committed series.

The optional `MESA_KK_DEBUG=submit_timing` diagnostics are specified with the
tools that use them, in
[`../tests/perf/README.md`](../tests/perf/README.md#native-whole-submit-timing).

The iOS storage build exports only the two file-oriented C entry points used by
Kernarium. They run one at a time, and while one runs they redirect the whole
process's `std::cout` and `std::cerr`, where the core reports its reasons; a
failed call appends that output to its error message. The storage archive
excludes every GPT fdisk command-line front end and `popt`. The standalone
macOS Android launcher runs an installed `sgdisk` executable instead (see the
[static disk contract](../android/lineage-24/README.md#static-disk-and-boot-contract));
no Homebrew library is linked or discovered.
The zstd build contains the streaming decompressor but excludes compression,
dictionary-builder, legacy-frame, deprecated, and command-line code. Both
libraries use static framework slices so their Clang module maps remain
separate when Xcode processes the XCFrameworks. The two builders share
`ios-static-xcframework.sh` for packaging, verification, and publication, and
their verifiers share `cmake/VerifyIosStaticXCFramework.cmake`; each supplies
only its library build, headers, provenance, and symbol contract.

Additional build-only archives use
`dependencies/prepare.sh --archive-manifest PATH --cache CACHE`. The tab-separated
manifest has `name`, `url`, and `sha256` columns; HTTPS downloads are verified
before publication as `CACHE/archives/SHA256-NAME`. `--no-fetch` and `--verify`
require those same authenticated cache entries. Archive preparation installs
nothing; package installation belongs in an isolated build environment.
[`kernel-host-tools/archives.tsv`](kernel-host-tools/archives.tsv) is the first
production archive manifest: the kernel host tools build extracts headers from
its Debian package into a private build directory. Its URL names a fixed
snapshot.debian.org timestamp because Debian's pool drops a package version on
the next stable update, so a pool URL would stop resolving on a fresh machine.

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

A manifest series field may join ordered layers with `+`. Each layer applies
to the tree the layers before it produce, and the last layer, which
`patches/` owns, records the tree of the whole stack. `series-field.sh` holds
this grammar for `prepare.sh` and the regenerator. The only lower layer
allowed is the paired guest's Mesa series, named
`rootfs-vulkan:patches/mesa/series.tsv`, whose source pin must equal the
host's; Mesa's row uses it. Its topic checkout carries the guest topics
followed by the host topics. Regenerate a changed guest layer first with
`tools/rootfs/vulkan/rebase-patches.sh --head GUEST_END`. This regenerator
exports only the topics after the single commit whose tree the lower layer
records, and it refuses a checkout without that commit.

Changing `manifest.tsv` or a build recipe invalidates the corresponding
installed receipt, and so does a series change that produces a different tree.
A series change that keeps the tree, such as a reworded message or a folded
follow-up, leaves every prefix valid. Rebuild an invalidated prefix and run its
tooling checks in the same change.

## Host prerequisites

Dependency preparation and builds require the Apple command-line tools plus
CMake 3.27 or newer, Ninja, GNU Make, Meson, Python 3.11 or newer,
`pkg-config`, Git, rsync, Autoconf, Automake, gettext, GNU libtool, gperf, and
groff (GNU libiconv builds its manual pages). Exact tool and SDK versions
remain observed build metadata; source identity comes from the manifest and
generated series.

Configure requires Python 3.11 or newer and fails with CMake's version error
on an older interpreter. macOS's own `python3` is 3.9, so install a newer one
and put it first on `PATH`, or pass `-DPython3_EXECUTABLE=<python>`.

Git needs a configured identity with a real email address. The patch-series,
kernel, Android, and dependency tooling tests commit fixtures as whoever Git
resolves, and patch export rejects a host-derived placeholder address.
`check-tooling` also tests the dependency build scripts, so it needs the
dependency tools above, `autopoint` from gettext included.

Apple GPU builds also need Xcode's Metal Toolchain component
(`xcodebuild -downloadComponent MetalToolchain`), which compiles KosmicKrisp's
device library.

Some workflows need further tools. A workflow that needs one fails naming it;
the rest of the repository does not.

| Tool | Needed by |
| --- | --- |
| `ld.lld` (LLVM lld) | integration guest payloads, the x86 probe corpus |
| erofs-utils (`mkfs.erofs`, `fsck.erofs`) and e2fsprogs (`debugfs`, `e2fsck`, `resize2fs`) | `tests/perf/make_geekbench_images.py` |
| gptfdisk (`sgdisk`) | the Android launcher, `tools/boot_android.py` |
| GNU coreutils `timeout` | the oracle scripts under `tools/arm64-tests/` |
| OpenSSL (`openssl`) | `tools/rootfs/provision.py` |
| LLVM 22.1.8 `llvm-mc` | the instruction-table freshness tests, which skip without it |

Homebrew installs e2fsprogs and LLVM as keg-only; the tools that need them
also look in the Homebrew prefix when `PATH` lacks them. Homebrew's coreutils
installs `timeout` as `gtimeout`; add its `gnubin` directory to `PATH`.

Meson, Mako, PyYAML, and packaging may come from an optional virtual
environment at `.build/gpu-tools-venv`. When `.build/gpu-tools-venv/bin/meson`
exists, `build.sh`, `build-apple-gpu.sh`, and `build-ios-network.sh` use it in
preference to `PATH` and to `UML_DEPENDENCY_MESON`.
