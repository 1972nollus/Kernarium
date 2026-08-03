# Testing and preparation

Use CMake/Ninja and CTest as the first-party source of truth. The `unit` suite
is host-only, contains no Python, and neither configures a kernel nor reads a
rootfs. Native emulator behavior tests are C++/assembly. The separately owned
`tooling` checks may use shell, CMake, or Python for build/source contracts;
they remain host-only and are selected alongside `unit` by the standard unit
workflow rather than becoming guest integration tests.

Reproducibility here means pinned official release archive and signature
identities, patch bytes and order, verified prepared source trees,
configuration inputs, ABI identity, and a recorded build procedure. Git,
Kbuild, compiler, linker, and Xcode-generated timestamps, commit IDs, build
IDs, user/host fields, package layouts, and output hashes stay authentic and
may differ between runs; record them as observations rather than rewriting
metadata or requiring byte equality. The signed `rootfs.img` digest is
intentionally stricter because it identifies an immutable input.

```sh
cmake --preset macos-unit-16k
cmake --build --preset unit-test-binaries
ctest --preset unit

cmake --preset macos-unit-4k
cmake --build --preset unit-test-binaries-4k
ctest --preset unit-4k
```

Prepare full integration explicitly. Linux must be replayed on a
case-sensitive filesystem. A local case-sensitive APFS sparse bundle is one
portable option when no suitable volume already exists:

```sh
hdiutil create -size 80g -type SPARSEBUNDLE -fs APFSX \
  -volname Kernel .build/Kernel.sparsebundle
hdiutil attach .build/Kernel.sparsebundle
```

Source preparation, dependency-prefix construction, and Linux replay are
explicit mutating setup steps. Kernel preparation authenticates the official
signed `linux-6.18.41.tar.xz` release archive and detached signature, then
creates a local Git replay with real generation timestamps. Their verification
paths are read-only and are what ordinary builds should use once those inputs
exist:

```sh
dependencies/prepare.sh
dependencies/build.sh
kernel/prepare-source.sh --source /Volumes/<case-sensitive-volume>/linux
# A fresh offline replay can use the authenticated files already in the cache.
kernel/prepare-source.sh --source /Volumes/<case-sensitive-volume>/linux-offline \
  --no-fetch

dependencies/prepare.sh --verify
dependencies/verify-prefix.sh \
  --prefix .build/dependencies/macos-arm64 \
  --manifest dependencies/manifest.tsv \
  --build-script dependencies/build.sh \
  --deployment-target 14.0
kernel/prepare-source.sh --source /Volumes/<case-sensitive-volume>/linux --verify
kernel/verify-replay.sh --work-root /Volumes/<case-sensitive-volume> --skip-build
python3 tools/rootfs/verify.py --image rootfs.img
cmake --preset macos-integration \
  -DUML_KERNEL_SOURCE_DIR=/Volumes/<case-sensitive-volume>/linux
cmake --build --preset macos-integration
ctest --preset integration-fast
```

`dependencies/prepare.sh --verify` authenticates the already prepared source
trees; it does not construct or verify the compiled dependency prefix.
`dependencies/build.sh` builds that private arm64 static prefix without network
access and publishes it transactionally. CMake configuration independently
checks the prefix receipt, manifest/build-policy identities, deployment target,
complete payload observation, real archive format, arm64 architecture, static
linkage, and absence of ambient/staging paths before integration targets can
consume it. The payload hash protects one built prefix against modification;
it is not a cross-build byte-reproducibility requirement.

Host build tools and the Apple SDK may come from the configured toolchain or
PATH. Production third-party library headers and binaries may come only from
the verified private prefix, never from system or package-manager locations.

`kernel/verify-replay.sh` creates and removes a fresh checkout, so its work root
must have several GiB free. Omit `--skip-build` to rebuild both page-size
kernels inside that replay. The materializer keeps its isolated GnuPG home on
a short secure temporary path to avoid Unix-domain socket limits under deeply
nested case-sensitive volumes; `UML_KERNEL_GPG_TEMP_ROOT` may override `/tmp`
for a restricted builder.

`integration-fast` runs host fixtures plus short real guest boots: the
production overlay/systemd path, interpreter, gadgets, JIT, and deterministic
SLiRP. The interpreter smoke intentionally stops after shell arithmetic and
CPU/HWCAP checks; Node/V8/Wasm coverage is restricted to gadget and JIT runs.
Do not add heavyweight Node.js, Go, or similar workloads to interpreter
acceptance. `integration-full` additionally selects the 10-vCPU SMP and
shared-MM pressure tests. Shell-oriented workloads boot an explicit writable tmpfs
OverlayFS shell; that is intentional and avoids relying on a systemd getty for
PTY interaction, while `guest-overlay-systemd-smoke` validates the production
systemd path separately.  The shared-MM workload is a CMake-built freestanding
arm64/Linux executable, so the immutable rootfs does not need a guest compiler.

Guest tests validate the lower image before and after each campaign, serialize
it with the rootfs lock, obtain dynamic VNC ports through LibVNCServer's
5900-and-up auto-port search, and terminate guest process groups.  The VNC
smoke is mandatory when registered: it does not convert a headless graphics
failure into a skip.

External network, KDE/VNC, and rendered WebGL workflows are opt-in because
they need network/GUI permission:

```sh
cmake --preset macos-integration \
  -DUML_KERNEL_SOURCE_DIR=/Volumes/<case-sensitive-volume>/linux \
  -DUML_ENABLE_EXTERNAL_NETWORK_TESTS=ON
ctest --preset integration-full
```

Full LTP is independently selected, never a unit test.  It requires an
immutable image that contains `/usr/lib/ltp-testsuite`; run it separately per
engine and preserve the rootfs wrapper:

```sh
cmake --build .build/presets/macos-integration --target guest-ltp-gadgets
cmake --build .build/presets/macos-integration --target guest-ltp-jit
# Convenience aggregate after both independent campaigns are desired:
cmake --build .build/presets/macos-integration --target guest-ltp-verified
python3 tests/integration/ltp/run_guest_ltp.py --help
```

Provisioning an image is explicit and verifies a detached signature over a
small digest statement plus the manifested image SHA-256; it refuses to
replace an existing image without `--replace`. Use publisher-supplied local
artifacts, or set the four `UML_ROOTFS_*` release variables named in the
manifest and pass `--allow-network`. The repository intentionally cannot invent
the release URL or trusted public key. Authorized writable-image maintenance is
`tools/rootfs/maintenance.py` and is intentionally outside all presets.

Use stock macOS commands when manually inspecting the image:

```sh
stat -f 'mode=%Sp size=%z bytes' rootfs.img
shasum -a 256 rootfs.img
```

Sanitizer, iOS, XCFramework, benchmark, and guest-workload runs are separate
acceptance campaigns.  Typical host-product commands are:

```sh
cmake --preset macos-unit-16k-asan-ubsan
cmake --build --preset asan-ubsan
ctest --preset unit-asan-ubsan -j 10 \
  --resource-spec-file cmake/ctest-resources.json

cmake --preset macos-unit-16k-tsan
cmake --build --preset tsan
ctest --preset unit-tsan

cmake --preset ios-device
cmake --build --preset ios-device
cmake --preset ios-simulator
cmake --build --preset ios-simulator
scripts/build-xcframework.sh
```

`unit-tsan` intentionally runs the tests labelled `tsan`: the static MMU gate,
static runtime, JIT publication concurrency, runtime context lifecycle, and
instruction-evidence reset fixture. The full normal and ASan/UBSan suites
retain exhaustive instruction coverage.

`scripts/build-xcframework.sh` builds a complete aggregate archive and links an
external consumer for macOS, iOS device, and iOS simulator. It also rejects
non-JIT executable-memory publication references in either iOS archive.

The current refactor evidence index and the local raw-result locations are in
[`refactor-evidence.md`](refactor-evidence.md).

They require the permissions and host hardware described by their commands; do
not convert an unavailable GUI, network, physical device, or performance host
into a passing result.
