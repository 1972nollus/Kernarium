# External dependencies

Production does not select third-party libraries from the host, a package
manager prefix, `CMAKE_PREFIX_PATH`, or the ambient `pkg-config` search path.
Every library is built as an arm64 static archive from an authenticated source
package recorded in [`manifest.tsv`](manifest.tsv). The resulting headers and
archives live in one project-private prefix.

The supported workflow on macOS is:

```sh
dependencies/prepare.sh
dependencies/build.sh
cmake --preset macos-integration
```

The checked integration presets already select the canonical source and
page-specific kernel build directories under `/Volumes/<case-sensitive-volume>`. See
[`../docs/testing.md`](../docs/testing.md) for explicit overrides when using a
different case-sensitive volume.

`prepare.sh` is the only step that accesses the network. It downloads each
HTTPS source archive into `.build/dependency-cache`, verifies its SHA-256,
safely extracts exactly one expected top-level directory, applies any
hash-pinned project patch, and verifies a canonical final-tree hash. Existing
source trees are never silently replaced; concurrent preparation publishes
with an exclusive rename and verifies whichever process wins. Use `--no-fetch`
with a populated archive cache, or `--verify` to audit prepared sources without
network access.

LibVNCServer follows the same release-package rule. The manifest pins the
pristine 0.9.15 release archive and its canonical unmodified tree hash. There
is no project-maintained post-release patch layer and no moving branch
snapshot.

The Kernarium iOS prototype has separate graphics prefixes at
`.build/kernarium-gpu-dependencies/ios-simulator-arm64` and
`.build/kernarium-gpu-dependencies/ios-device-arm64`. It follows UTM's iOS
graphics composition because upstream desktop libepoxy/virglrenderer branches
do not provide the complete Apple mobile loader and native Metal export path.
The exact UTM Git commits and repositories are recorded in
[`../Kernarium/dependencies/utm-gpu-sources.tsv`](../Kernarium/dependencies/utm-gpu-sources.tsv),
and the platform contracts are recorded in
[`../Kernarium/dependencies/ios-simulator-arm64.tsv`](../Kernarium/dependencies/ios-simulator-arm64.tsv)
and
[`../Kernarium/dependencies/ios-device-arm64.tsv`](../Kernarium/dependencies/ios-device-arm64.tsv).
`Kernarium/scripts/build-ios-gpu-dependencies.sh` performs no network access;
it accepts clean caller-provided checkouts only when origin and HEAD match,
copies them to a temporary build root, and publishes the prefix transactionally
after verifying its receipts and Apple binary shape.

Each prefix contains static UTM libepoxy and virglrenderer archives plus the
two ANGLE dynamic payloads repackaged as `EGL.framework` and
`GLESv2.framework`. The app embeds both frameworks, while only libepoxy loads
their APIs; the WebKit-derived GLES image retains its `LC_SUB_CLIENT` contract
and is not linked directly by the application. CMake never searches ambient
package prefixes for these libraries. This mobile prefix is not an input to
the macOS GPU graph, whose authenticated release-source prefix and link policy
remain unchanged.

The graphics receipts also authenticate two source patches. The ANGLE patch
backports upstream commit `d44204893a1725a74cd20b332fbb6f595b0975c3`, whose
Simulator capability correction raises the reported vertex-output limit to the
OpenGL ES 3 minimum required by Chromium WebGL 2. The virglrenderer patch adds
the ES 3 renderbuffer multisample path and preserves the existing desktop GL
behavior. Reviewing the latest upstream lines did not justify wholesale
updates: current UTM libepoxy remains the maintained iOS loader fork, UTM's
newer virglrenderer line contains additional iOS/Metal work, and upstream
virglrenderer still lacks UTM's `vrend_metal` composition. Compatibility is
therefore advanced through reviewable backports without changing Chromium's
library lookup or advertising a false GLES version.

Kernarium also has static network prefixes at
`.build/kernarium-network-dependencies/ios-simulator-arm64` and
`.build/kernarium-network-dependencies/ios-device-arm64`. Their receipts are
[`../Kernarium/dependencies/ios-simulator-network-arm64.tsv`](../Kernarium/dependencies/ios-simulator-network-arm64.tsv)
and
[`../Kernarium/dependencies/ios-device-network-arm64.tsv`](../Kernarium/dependencies/ios-device-network-arm64.tsv).
`Kernarium/scripts/build-ios-network-dependencies.sh` verifies and reuses the
authenticated PCRE2, libffi, proxy-libintl, GLib, and libslirp source trees,
applies the receipt-pinned iOS GLib availability patch, and publishes a static
platform prefix transactionally. Neither iOS prefix family is an input to the
macOS dependency graph.

Kernarium's app-only terminal UI uses SwiftTerm 1.18.0 through Xcode Swift
Package Manager. The project requirement is exact, and the checked-in
`Package.resolved` authenticates revision
`7691f85b222a67a66b58499e1b2647443cf0dda7`; do not replace it with an
unversioned branch or an ambient checkout. SwiftTerm is built only into the
iOS application target and does not enter the emulator libraries, runtime
frameworks, or macOS graph. Resolve the package once with Xcode, then pass
`-disableAutomaticPackageResolution` in unattended builds so the lock cannot
move during compilation.

`build.sh` copies those verified trees into a temporary build area and creates
only static arm64 libraries under `.build/dependencies/macos-arm64`. Its
ordered build recipes must exactly match every manifest name and release
version, and each copied source tree is rehashed before configuration. Its
`pkg-config` search is restricted to that staging prefix, and Meson's optional
CMake dependency fallback is disabled so probes cannot discover host package
manager libraries. Publication is transactional: CMake accepts the prefix only
after its receipt, required headers, real static-archive
format and architecture, deployment target, current manifest/patch validation,
and absence of shared libraries have been verified. Git owns changes to the
preparation and build scripts; the prefix receipt does not duplicate their
identity or hash the complete installed product.
Configure and ordinary compilation never download or discover a third-party
library from the host.

The final prefix deliberately omits upstream CMake package exports and libtool
metadata. They are not used by this project.
Production consumers use the exact imported static archives declared in
[`cmake/UmlThirdPartyTargets.cmake`](cmake/UmlThirdPartyTargets.cmake).

The desktop virglrenderer patch adapts generated shaders to Apple’s core-only
OpenGL 4.1 implementation. Features that are already core in the selected GLSL
version, including uniform buffer objects in GLSL 1.40, use the core spelling
instead of requiring legacy extension names that Apple is not required to
advertise. Compatibility and GLES hosts retain the upstream extension path.
The same patch keeps hardware-video capabilities empty on Apple hosts, but
accepts the validated video-buffer create/destroy lifecycle that Mesa emits for
its guest-side, shader-based `VAProfileNone` post-processing path. No host codec
backend is claimed: codec and frame commands remain unsupported. This prevents
a harmless GStreamer capability probe from poisoning its virgl context while
preserving fail-closed behavior for actual encode and decode requests.

The prefix also retains the following archives installed by the verified GLib,
PCRE2, and libffi recipes. They are authenticated payload, but are deliberately
not imported into the production link graph:

| Archive | Reason retained |
| --- | --- |
| `libffi.a` | GLib build feature dependency; no current emulator target uses its public ABI |
| `libgio-2.0.a` | GLib companion output; SLiRP uses only the base GLib archive |
| `libgirepository-2.0.a` | GLib companion output; introspection is disabled in production |
| `libgmodule-2.0.a` | GLib companion output; dynamic modules are not loaded |
| `libgobject-2.0.a` | GLib companion output; the current SLiRP ABI needs no GObject layer |
| `libgthread-2.0.a` | GLib compatibility archive; threading resolves through base GLib and pthreads |
| `libpcre2-posix.a` | PCRE2 companion output; only the native PCRE2-8 API is linked |

The prefix verifier rejects symlinks, shared libraries, and undocumented static
archives, and checks each archive's arm64 format. The production application
also rejects non-system dynamic-library dependencies after linking. Neither
check inventories or attributes individual object/archive members.

The paths can be overridden explicitly:

```sh
dependencies/prepare.sh --root /path/to/sources --cache /path/to/cache
dependencies/build.sh --source-root /path/to/sources --prefix /path/to/prefix
cmake -S . -B build -DUML_DEPENDENCY_PREFIX=/path/to/prefix
```

Build tools remain host prerequisites: CMake 3.27+, Ninja, GNU Make, Clang,
Meson, Python 3, `pkg-config`, `curl`, `patch`, and Xcode command-line tools.
The Apple SDK and its system libraries/frameworks are platform APIs, not
third-party package dependencies. The Linux kernel continues to use Kbuild and
the documented cross-tool wrappers; its Darwin host tools use the
project-owned ELF compatibility header rather than a host libelf package.

Source identity is the manifest, archive bytes, patch bytes, and prepared-tree
hash. Compiler/SDK versions and the real build timestamp are retained in the
prefix receipt as observations. Binary equality across builds is neither
claimed nor required.

The core `macos-unit-16k` and `macos-unit-4k` presets do not need the private
desktop dependency prefix, a kernel build, or a rootfs image. The focused GPU
unit presets do use the private graphics/display dependencies, but still need
neither a kernel nor a rootfs.
