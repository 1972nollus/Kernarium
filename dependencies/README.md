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
UML_KERNEL_SOURCE_DIR=/Volumes/<case-sensitive-volume>/linux \
  cmake --preset macos-integration
```

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

`build.sh` copies those verified trees into a temporary build area and creates
only static arm64 libraries under `.build/dependencies/macos-arm64`. Its
ordered build recipes must exactly match every manifest name and release
version, and each copied source tree is rehashed before configuration. Its
`pkg-config` search is restricted to that staging prefix, and Meson's optional
CMake dependency fallback is disabled so probes cannot discover host package
manager libraries. Publication is transactional: CMake accepts the prefix only
after its receipt, complete payload hash, required headers, real static-archive
format and architecture, deployment target, safe symbolic links, and absence
of shared libraries have been verified. The payload digest is an observation
of that particular build, so it detects later drift without requiring two
independent builds to have identical bytes. Configure and ordinary compilation
never download or discover a third-party library from the host.

The final prefix deliberately omits upstream CMake package exports and libtool
metadata. They are not used by this project and can serialize build-only paths.
Production consumers use the exact imported static archives declared in
[`cmake/UmlThirdPartyTargets.cmake`](cmake/UmlThirdPartyTargets.cmake).

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

The prefix verifier rejects any additional archive, and inspects every Mach-O
member for arm64 macOS platform metadata and the configured deployment target.

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
