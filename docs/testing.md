# Testing and preparation

Use CMake/Ninja and CTest as the first-party source of truth. Start with the
narrowest relevant target or exact test, then run the single workflow that owns
the changed behavior.

List the current preset surface with:

```sh
cmake --list-presets=all
```

## Test suites

Every first-party CTest has exactly one primary suite:

- `unit`: native C++/assembly behavior; no Python, shell, kernel, network, CLI,
  or rootfs dependency;
- `tooling`: repository, build, manifest, and source-policy checks; and
- `integration`: guest, rootfs, GUI, network, and kernel behavior.

`tests/test-manifest.tsv` records every registration and semantic owner.
Performance and long external campaigns are not CTest correctness suites.

For one test, use an anchored expression:

```sh
ctest --test-dir .build/presets/macos-unit-16k \
  -R '^exact-test-name$' --output-on-failure
```

## Canonical host workflows

The ordinary 16 KiB and 4 KiB host checks are:

```sh
cmake --workflow --preset check-unit
cmake --workflow --preset check-unit-4k
```

Use both profiles for page-size-sensitive CPU, memory, ABI, kernel, or guest
changes. A change confined to a page-independent host tool usually needs only
its owning workflow.

Specialized workflows are:

```sh
cmake --workflow --preset check-gpu
cmake --workflow --preset check-gpu-asan-ubsan
cmake --workflow --preset check-loader-asan-ubsan
cmake --workflow --preset check-jitprof
```

The experimental macOS Vulkan lane is separately named and opt-in. It needs the
`apple-gpu-vulkan-experiment` dependency prefix and a private prefix built at
its raised deployment target, and it is required only when a change crosses the
Venus or KosmicKrisp boundary:

```sh
cmake --workflow --preset check-gpu-venus
cmake --workflow --preset check-integration-venus
```

The full sanitizer suites remain separate because of their runtime cost:

```sh
cmake --preset macos-unit-16k-asan-ubsan
cmake --build --preset asan-ubsan
ctest --preset unit-asan-ubsan

cmake --preset macos-unit-16k-tsan
cmake --build --preset tsan
ctest --preset unit-tsan
```

## Integration prerequisites and workflows

Integration needs three local inputs:

- the accepted kernel bundle for the selected page profile below
  `.build/kernel/artifacts`;
- the verified dependency prefix; and
- the unified `rootfs.erofs` image.

It does not require a kernel source checkout or compiler. Run:

```sh
cmake --workflow --preset check-integration
cmake --workflow --preset check-integration-4k
```

The workflows configure the product, build it, run tooling checks, and execute
the fast integration suite. `integration-full` and `integration-full-4k` are
explicit CTest presets when the complete registered guest set is required.

The shared serial harness sends commands through a nonblocking pipe and keeps
PTY output. This keeps host input pollable when every guest CPU is busy while
preserving terminal output behavior and bounded writes under backpressure.

The checked acceptance descriptors select complete kernel bundles. A local
override may select another whole bundle of the matching profile through
`UML_MAINLINE_618_KERNEL_ARTIFACT_DIR`; do not override individual files.

## Rootfs contract

`rootfs.erofs` is a unified immutable guest image. Ordinary runs use it
directly; authentication happens only when provisioning a replacement. Shared
runs map it with `ubd0c=` and boot `root=/dev/ubda rootfstype=erofs`. Never use
the root device as writable scratch space.

Writable guest state belongs in a disposable OverlayFS
layer. Rootfs updates replace the complete authenticated image through
`tools/rootfs/provision.py`; there is no in-place maintenance workflow.

The image owns accounts, credentials, login policy, gettys, desktop packages,
and supporting payloads under `/opt`. Launchers must not invent replacement
users or general login policy. Narrow compatibility overlays already owned by a
launcher must remain confined to that image/session combination.

The systemd smoke uses local source-built container content with no registry or
network dependency. Docker and containerd state are mounted separately from
the writable root overlay.

`tools/rootfs/starter` separately owns the writable raw-GPT starter disks used
with the macOS emulator. Their common memory-backed initramfs grows ext4 only
after a host tool has enlarged the disk and its sole partition. Growth happens
before the real root is mounted, is skipped for non-ext4 or multi-partition
disks, and is non-fatal. Each image owns its `hvc0` getty, DHCP/DNS service,
accounts, and optional Plasma session. See the
[producer README](../tools/rootfs/starter/README.md) for its single build and
grow entry points; these images do not replace the unified immutable product
rootfs.

`tools/rootfs/x86_64-smoke` owns the separate, pure x86-64 EROFS image used to
qualify the dedicated 4 KiB x86_64-userspace kernel slice. Kernel-mode
execution remains native arm64, but that slice cannot load arm64 userspace.
Build it on an x86-64 Linux host and run the bounded
`tests/integration/x86_64/run_guest_x86_64_smoke.py` boot check; it is not a
replacement for the accepted general-purpose rootfs.

`tools/rootfs/wine-arm64ec` owns the separate ARM64-only Debian/XFCE appliance
containing source-built ARM64EC/ARM64X Wine, PE-only i386 WoW64 modules, and
AMD64 and i386 execution probes. Its producer emits package, ELF, PE, and
provenance inventories and rejects any x86 ELF payload. Build it on native
AArch64 Linux with the pinned Wine source and checksum-verified ARM64EC
compiler described in its
[producer README](../tools/rootfs/wine-arm64ec/README.md).

The fast full-stack gate is
`cmake --workflow --preset check-wine-arm64ec-macos`. It boots the dedicated
4 KiB kernel and emulator, starts Xvfb and the XFCE window manager, and requires
Wine to return exit code 42 from the AMD64 PE probe and 43 from the i386 WoW64
probe. iOS Simulator and physical-device packaging and launch validation
follow only after this macOS gate passes.

## Apple builds and Kernarium

Build the general Apple libraries with:

```sh
cmake --preset ios-device
cmake --build --preset ios-device
cmake --preset ios-simulator
cmake --build --preset ios-simulator
scripts/build-xcframework.sh
```

Kernarium has separate runtime presets for device and Simulator, 16 KiB and
4 KiB Linux, 16 KiB Android, and the dedicated 4 KiB ARM64EC Wine profile.
`Kernarium/scripts/package-ios-frameworks.sh` packages all selected runtime and
graphics slices plus the static GPT fdisk and decode-only zstd XCFrameworks for
Xcode. A `wine-arm64ec-*.erofs` filename selects
`KRuntime_Wine_6_18_4K`; this reserved identity prevents the ordinary Linux
runtime from opening the Wine appliance with no x86 CPU service.
`dependencies/build-ios-storage.sh --verify-only` and
`dependencies/build-ios-zstd.sh --verify-only` check both arm64 Apple slices,
their provenance and deployment targets, required APIs, and the absence of
unused command-line providers. Apple builds
validate the kernel bridge ABI, page profile, product family, and `6.18` series
before embedding an artifact; they do not bind to its patch release or source
commit and never prepare sources or invoke a kernel/AOSP build. Rootfs bundles
are launch inputs, not emulator build inputs.

Kernarium defaults to 16 KiB Linux, 4 GiB on Simulator, 2 GiB on physical
devices, and the processor count reported by `NSProcessInfo`. Android fixes the
page profile at 16 KiB; the ARM64EC Wine image fixes it at 4 KiB. A `.kern`
bundle contains the selected rootfs release and its persistent writable state;
rebuilding the app or kernel framework must not erase it.

Simulator launch and bundle assembly are owned by
`Kernarium/scripts/run-ios-simulator.sh` and
`Kernarium/scripts/make-guest-bundle.py`. Use each script's `--help` rather than
copying a long invocation into documentation.

For an opt-in Simulator diagnostic, pass one SLiRP forwarding specification as
`SIMCTL_CHILD_KERNARIUM_HOST_FORWARD` when launching the app. For example,
`tcp:127.0.0.1:5567-5555` exposes guest ADB on host loopback port 5567. Normal
launches leave the variable unset and publish no host port.

Android recovery qualification uses a fresh disposable bundle and sets
`SIMCTL_CHILD_KERNARIUM_ANDROID_BOOT_MODE=recovery`. Verify the OrangeFox UI,
touch input, ADB, fixed-GPT partition discovery, read-only payload mounts,
userdata/metadata mount and format behavior, backup/restore, file manager,
terminal, battery display, and normal/recovery reboot paths. Never run wipe or
restore checks against an existing user bundle.

When copying a `.kern` bundle to a device, target the complete
`Documents/<name>.kern` destination. Never copy into or replace the Documents
directory itself, because doing so can flatten the bundle or destroy unrelated
persistent guests.

## LTP

The full LTP campaign is opt-in and runs only on gadget or JIT engines.
`tests/integration/ltp/verified-tests.tsv` and `disabled-tests.tsv` are the
current exact manifests. A disabled case represents an intentional product or
kernel-policy exclusion; do not enable a kernel feature solely to increase the
test count.

The runner uses `/opt/ltp` from `rootfs.erofs` and the selected kernel bundle's
`modules.builtin`. The shipped kernels have `CONFIG_MODULES=n`; disposable
guest helpers may recognize exact built-ins but must reject unknown module
operations. See the runner's current options with:

```sh
python3 tests/integration/ltp/run_guest_ltp.py --help
```

## A64 instruction evidence

The ordinary instruction suites exercise all inventory entries with
representative semantic contracts. They are not exhaustive variant proof. The
machine-readable source of truth and current completeness boundary are
documented in [`a64-instruction-coverage.md`](a64-instruction-coverage.md).

Instruction evidence workflows are:

```sh
cmake --workflow --preset check-instruction-evidence
cmake --workflow --preset check-instruction-evidence-4k
```

Full input materialization is off by default and consumes several GiB per
profile. Enable it only for an explicit official-semantics campaign. Such a
campaign can run for days and is not ordinary acceptance.

## x86 instruction conformance

The 4 KiB x86 gadget backend uses both handwritten x86-64 and i386 ASM suites
from the pinned FEX `x86-conformance` dependency as its external instruction
campaign. FEX remains a test input and is not linked into the runtime.
Materialize each mode with the pinned NASM producer and
`tools/fex-tests/build-asm-corpus.py`; generated corpus data stays outside the
repository. Run every binary through the test-only gadgets adapter with
`tools/fex-tests/run-asm-corpus.py`. Generic upstream-disabled cases remain
explicit exclusions, while FEX backend- and host-specific exclusions do not
apply to gadgets. Acceptance requires every applicable materialized case in
both modes to pass with no decoder or semantic failures; each JSON report
records the pinned source identity, mode, applicable count,
upstream-disabled cases, and per-test result.

## Performance and external campaigns

Benchmarks, full LTP, external networking, KDE/VNC/WebGL desktop campaigns,
GPU conformance, SMP stress, official A64 semantics, and connected-device runs
are opt-in. Run them only when the change or question crosses that boundary.

Heavy guest campaigns use gadgets or JIT. The interpreter is a reference
oracle and has only a short registered guest smoke; answer interpreter-specific
questions with a focused payload.

Geekbench uses the image-local payload with `--no-upload` and requires no
Internet preflight. Manual performance tools and the probe inventory are under
[`../tests/perf/README.md`](../tests/perf/README.md). Record conclusions in
[`optimization-history.md`](optimization-history.md), not raw logs or a new
investigation document.

GPU capabilities, qualification boundaries, and unsupported Vulkan work are
summarized in [`graphics.md`](graphics.md).

## Claiming completion

A passing structural audit does not prove generated-code shape, performance,
kernel boot, or guest behavior. A unit test does not prove an external
campaign. Report only checks actually run and clearly identify any external
input or campaign that was unavailable.

Do not normalize compiler metadata or require byte-identical independent
builds. For a measured A/B, record the revision and dirty state, relevant
configuration, command, result, and exit status; Git and the owning manifests
provide the remaining provenance.
