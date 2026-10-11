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

- `unit`: native C++/assembly behavior; no runtime Python, shell, kernel,
  network, CLI, or rootfs dependency;
- `tooling`: repository, build, manifest, and source-policy checks; and
- `integration`: guest, rootfs, GUI, network, and kernel behavior.

The helper that registers a test sets its suite label; `ctest -N -L <suite>`
lists a configured tree's registrations. A unit test without its own `TIMEOUT`
gets a 600 s bound, so a wedged binary fails the suite instead of stalling it.
At a test's timeout CTest sends it SIGINT, which a Python runner turns into
`KeyboardInterrupt` to unwind its `with` and `finally` blocks, and SIGKILL 20 s
later; the test fails as a timeout either way. `unittest` runs no `tearDown`
or `addCleanup` callback after a `KeyboardInterrupt`, so a Python test holds
its scratch in a `with` block or a `tempfile.TemporaryDirectory` object, whose
finalizer runs at exit. Performance and long external campaigns are not CTest
correctness suites.

Tooling tests run tools against fixtures; they do not grep tracked source for
expected tokens or restate values that another file owns. A shell tooling
test's `$TMPDIR` is its `test-scratch/<test>` in the build tree, which
configure creates: a shell defers its traps while a child runs, so a shell
test that times out in one cannot clean up, and what it leaves names it.
Python tooling tests keep the host `$TMPDIR` for `sun_path`.

`repository-environment-portability` is the one tooling test that scans
tracked source, and it looks for forbidden values rather than expected ones:
host paths and account names, private addresses, ssh endpoints and Apple
signing identifiers outside the project's own. It also scans new unignored
files, so a leak fails before it is committed. A site adds its private tokens
to a local denylist, found at
`${XDG_CONFIG_HOME:-$HOME/.config}/linux-um-darwin/portability-denylist` or
through `UML_PORTABILITY_DENYLIST`; the test runs with and without it. A
reviewed exception goes in
`tests/tooling/cmake/environment-portability-allow.tsv` under one of its fixed
reasons, and an entry that matches nothing fails.
`python3 tests/tooling/cmake/check_environment_portability.py --list-rules`
describes the rules and the denylist format.

Unit tests create scratch files through `tests/unit/scratch/`, never at a
host-global path unless that path is the production contract under test, as
for the management console's run directory. Each registration owns
`test-scratch/<test>` in the build tree; a binary run outside CTest uses
`$TMPDIR`. Unix-domain socket paths use `$TMPDIR` because a build-tree path
may not fit `sun_path`. Tests remove what they create, so a leftover file
names the test that leaked it.

A unit test that reads data, such as a generated instruction corpus or the
checked-in video fixtures, takes its path as a registration argument rather
than a compiled-in source or build path. A copy of the binary then runs
wherever its data is copied, and a binary run without the argument prints its
usage.

CPU instruction assets are generated in the build tree from checked-in source
tables and independent golden expectations. Their consumers depend on the
generation targets, and the tooling aggregate builds the A64 asset as well.
Generation requires Python's standard library, with no network, Git history,
assembler, upstream checkout, or reference hardware. The
[source contract](../tools/instruction-corpus/README.md) owns their format and
promotion; the binary assets are not tracked.

Unit tests assert with `require`, `fail` and `requireThrows` from
`tests/unit/require/require.hpp`, which every unit binary links. A failure
prints the caller's `file:line` and its message, then aborts, so no `catch` in
the test or in the code under test can swallow it.

For one test, use an anchored expression:

```sh
ctest --test-dir .build/presets/macos-unit-16k \
  -R '^exact-test-name$' --output-on-failure
```

## Canonical host workflows

Every workflow needs the tools in
[Host prerequisites](../dependencies/README.md#host-prerequisites), among them
Python 3.11 or newer, which configure requires, and a Git identity. A test
that boots ten vCPUs, such as `guest-mmu-stress`, needs ten host CPUs and
skips on a smaller host. The LTP campaigns default to half the host's physical
cores, at most 16.

The ordinary 16 KiB and 4 KiB host checks are:

```sh
cmake --workflow --preset check-unit
cmake --workflow --preset check-unit-4k
```

Use both profiles for page-size-sensitive CPU, memory, ABI, kernel, or guest
changes. A change confined to a page-independent host tool usually needs only
its owning workflow; for tooling tests alone that is:

```sh
cmake --workflow --preset check-tooling
```

Specialized workflows are:

```sh
cmake --workflow --preset check-gpu
cmake --workflow --preset check-gpu-asan-ubsan
cmake --workflow --preset check-loader-asan-ubsan
```

The `unit-gpu` test preset's name filter is the one list of GPU workflow unit
tests, and the other `unit-gpu*` test presets inherit it. Configure derives the
`gpu-unit-test-binaries` target from that filter, so adding a test to the
filter also adds its binary to every GPU build preset. Configure fails when a
test preset that inherits `unit-gpu` sets its own filter.

The experimental macOS Vulkan lane is separately named and opt-in. It needs the
`apple-gpu-vulkan-experiment` dependency prefix and a private prefix built at
its raised deployment target, and it is required only when a change crosses the
Venus or KosmicKrisp boundary:

```sh
cmake --workflow --preset check-gpu-venus
cmake --workflow --preset check-integration-venus
```

KosmicKrisp's prebuilt SPIR-V has a cheap guard in the tooling suite and an
opt-in full comparison that rebuilds `mesa_clc` on the pinned Debian host and
compiles both blobs again from a checkout of the committed Mesa series
(`external/sources/mesa` unless `UML_KOSMICKRISP_SPIRV_MESA_SOURCE` names
another). It registers only when that SSH host is named:

```sh
cmake --preset macos-tooling -DUML_KOSMICKRISP_SPIRV_BUILD_HOST=<host>
ctest --test-dir .build/presets/macos-tooling \
  -R '^dependency-kosmickrisp-spirv-regeneration$' --output-on-failure
```

The iOS GPU lane runs the unmodified iOS device build on an Apple silicon Mac
in Designed for iPad mode; [`graphics.md`](graphics.md#current-path) says why
the iOS Simulator has no GPU build. Kernarium's own suite carries the narrowest
check of that provider: it creates a surfaceless OpenGL 4.6 core context in the
application process and requires Zink on the packaged KosmicKrisp driver, so a
broken iOS graphics closure fails in seconds rather than inside a guest.

A whole CTS campaign runs in that application through the opt-in
`apple-ios-mac-vk-gl-cts` lane. It needs Xcode's local install service and a
sealed rendered image, so it registers only when the image is named:

```sh
cmake --preset macos-integration-venus \
    -DUML_IOS_MAC_CTS_ROOTFS=<image>.ext4 \
    -DUML_IOS_MAC_CTS_ROOTFS_RECEIPT=<image>.receipt.json \
    -DUML_IOS_MAC_CTS_CAMPAIGN=gl46-matrix-cfg00-xml01-001of032-zink-v1
ctest --test-dir .build/presets/macos-integration-venus \
    -R apple-ios-mac-vk-gl-cts
```

The lane builds the campaign's guest launcher, service, and `uml-cts.target`
into a boot payload with the macOS lanes' own drivers
(`tests/integration/gpu/kernarium_campaigns.py`), copies the image and the
payload into the application's container, launches it, and decodes the
artifact frames the runtime relays. Only the automation application boots a
payload ([`Kernarium/README.md`](../Kernarium/README.md#automation-runs)), so
package the `kernarium-ios-device-automation` runtimes with
`package-ios-frameworks.sh --automation` first.

The same automation application runs a Vulkan campaign on a connected iOS
device through the opt-in `apple-ios-device-vk-cts` lane. It registers only
when the device is named, expects the application and the authenticated Venus
image to be on the device already, copies the campaign's payload beside them,
and reads the artifact frames from the launch console, because the
guest-session log is an XCTest path that a device launch does not take:

```sh
cmake --preset macos-integration-venus \
    -DUML_IOS_DEVICE_VK_CTS_DEVICE=<udid> \
    -DUML_IOS_DEVICE_VK_CTS_CAMPAIGN=vulkan-info-receipt-smoke-v1
ctest --test-dir .build/presets/macos-integration-venus \
    -R apple-ios-device-vk-cts
```

For a baked device mustpass fraction, the runner prints
`vulkan_campaign_complete=true` only after validating the exact post-exclusion
QPA case sequence, the exclusion report, the status oracle, and a lossless
artifact transfer. This completion marker is independent of
`vulkan_statuses_accepted`: a complete fraction may retain CTS failures for
diagnosis. Other device campaign modes do not print these validation markers.

The headless device lane requires WSI-dependent cases to return
`NotSupported`. The `surface.query_devgroup_present_capabilities` and
`surface.destroy_null_handle` cases do not need a native window or live
surface, so their results use the ordinary CTS status oracle; a `Fail` remains
a failure. Both cases remain in the required ordered coverage.

Run the runner directly to install a bundle, stage an image, or take one of the
fifteen `vulkan-mustpass-NNof16-v3` fractions of the released list or the
`vulkan-mustpass-03of16-v4` fraction. See the runner's `--help`. A campaign
is judged by its `summary.txt`, the guest's own verdict, never by a console
marker alone.

Host and renderer diagnostics share that console with the artifact frames, and
a line written into the middle of one is a malformed marker that costs the
attempt its results. A host or renderer condition on a per-frame or per-bind
path therefore reports once and not again, which is also the only way an
operator can raise the renderer's log level on a device run.

A baked device campaign is the one lane whose guest memory is fixed by the
platform, so it is the only lane that leaves cases out. Its profile fixes the
identifier, guest memory, and any planned process boundary; the device runner
refuses a different memory value before launch. `DEVICE_EXCLUSIONS` in
`run_vk_cts.py` names exclusions by cause, and the generation suffix is part of
the selection's identity. The guest drops them from the fraction it enumerated
and transports `skipped.txt` as `<reason>\t<case>` rows, with `device_excluded*`
counts in `summary.txt`, so `cut -f2 skipped.txt` is the selection to re-run on
hardware without that limit. An excluded case is neither a pass nor a failure.
Every macOS lane runs the released selection unchanged.

The optional `guest-v4l2-decode` test qualifies the
[Virtio video decoder](../darwin/device/video/README.md). Build
`tests/integration/video/v4l2_decode_probe.cpp` as a static C++20 executable for
the guest architecture and configure an integration preset with
`UML_V4L2_PROBE_BINARY` pointing to it. The rootfs needs a shell, `ip`,
`sha256sum` and `timeout`. The test boots a disposable overlay with the probe
and its fixtures installed as payload files, checks their SHA-256 list, checks
every advertised profile's pixels, raw layouts including packed BGRA capture,
imported dma-buf capture, timestamps, source-change and EOS events, and rejects
kernel faults or failed shutdown. Run it on both Linux
page profiles; Android uses its own 16 KiB guest and deployment workflow.
The fixed x86_64 userspace product registers no CTest suites, so the x86_64
slice exercises its own V4L2 path by running `run_v4l2_decode.py` directly with
an x86-64 probe and an x86-64 rootfs, the way its boot check below is run.

The optional `guest-futex-atomic` test checks the futex contract in
[`../kernel/common/README.md`](../kernel/common/README.md). Configure
`UML_FUTEX_PROBE_BINARY` with a static C++20 build of
`tests/integration/futex/futex_atomic_probe.cpp`; the rootfs needs the video
probe's shell tools. Run it on both Linux page profiles and the maintained
Android kernel.

`guest-aarch32-kernel` checks the kernel's AArch32 task support in
[`../kernel/common/README.md`](../kernel/common/README.md) on both Linux page
profiles, and needs `python3` in the rootfs. The build generates a freestanding
armhf program from `tests/integration/aarch32/aarch32_probe.S` with the
configured compiler's ARM backend, wraps it into an ELF32 executable with
`make_elf32.py` (the toolchain has no ARM linker) in a layout for any page size
and in one for 4 KiB pages only, and never commits either. While no engine
implements AArch32 (`UML_AARCH32_EXPECT=unsupported`, the default) the test
requires `ENOEXEC` for both programs, `EINVAL` for `personality(PER_LINUX32)`,
and unchanged 64-bit `uname`, `/proc/cpuinfo`, `vm.mmap_rnd_compat_bits` and
`abi.cp15_barrier`. With `UML_AARCH32_EXPECT=run` it runs the program (kuser
helpers, signals and VFP state, `cacheflush`, Thumb, `mmap2`, `PER_LINUX32`
views, restarts and exec between ABIs; threads through `clone` and `clone3`,
fault signals and stack growth, the time64 and socket calls with
`SCM_RIGHTS`, `vfork`, timers, `seccomp` and a 32-bit tracer), a 64-bit
tracer's session (registers, system call and single-step stops, signal-frame
VFP state), a core dump and many runs at once, and requires the 4 KiB-only
program to run on a 4 KiB guest and to be killed by `SIGSEGV` at exec on a
16 KiB one, leaving the guest working. The tooling test
`aarch32-elf32-wrapper` checks the two layouts the wrapper writes.

`UML_TEST_APPLE_AUDIO=ON` enables `apple-audio-playback`, which sends a brief,
low-volume stereo tone through the real macOS output and verifies completion
and restart. It requires access to macOS audio services.

The full sanitizer suites remain separate because of their runtime cost:

```sh
cmake --preset macos-unit-16k-asan-ubsan
cmake --build --preset asan-ubsan
ctest --preset unit-asan-ubsan

cmake --preset macos-unit-16k-tsan
cmake --build --preset tsan
ctest --preset unit-tsan
```

AddressSanitizer keeps its heap at the product's macOS native window and
ignores `mlock()`, so under it the cases that need that window or wired guest
RAM report themselves unsupported instead of running.

## Integration prerequisites and workflows

Integration needs four local inputs:

- the accepted kernel bundle for the selected page profile below
  `.build/kernel/artifacts`;
- the accepted boot base archive below `.build/rootfs`, which `uml-darwin`
  embeds ([boot ramdisk](../tools/rootfs/boot-ramdisk/README.md));
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

The shared serial harness boots each guest through the launcher's per-run
initramfs: its `/init` mounts the image beneath a tmpfs overlay, installs the
files a test needs as payload files, and starts the
[harness session](../tools/rootfs/boot-ramdisk/README.md#harness-session),
which prepares the requested runtime mounts, network, workspace, swap and
cgroups and gives the harness a root shell. The console carries commands and
their output, never files. The harness sends commands through a nonblocking
pipe and keeps PTY output. This keeps host input pollable when every guest CPU
is busy while preserving terminal output behavior and bounded writes under
backpressure.

`ctest -N -L integration` lists the registered guest tests. Each runner or
guest probe states in its header what it checks, and a contract's owner, such
as [`../kernel/common/README.md`](../kernel/common/README.md), names the tests
that check it.

The checked acceptance descriptors select complete kernel bundles. A local
override may select another whole bundle of the matching profile through
`UML_MAINLINE_618_KERNEL_ARTIFACT_DIR`; do not override individual files.

## Rootfs contract

`rootfs.erofs` is a unified immutable guest image. Ordinary runs use it
directly; authentication happens only when provisioning a replacement. Shared
runs map it with `ubd0c=`, and the per-run `/init` mounts `/dev/ubda` as EROFS
beneath the run's writable layer; no runner names the root device. Never use
the root device as writable scratch space.

Ordinary test guest state belongs in a disposable OverlayFS
layer; the Steam appliance instead owns a separate persistent GPT data disk. Rootfs updates replace the complete authenticated image through
`tools/rootfs/provision.py`; there is no in-place maintenance workflow.

The image owns accounts, credentials, login policy, gettys, desktop packages,
and supporting payloads under `/opt`. Launchers must not invent replacement
users or general login policy. Narrow compatibility overlays already owned by a
launcher must remain confined to that image/session combination.

The systemd smoke uses local source-built container content with no registry or
network dependency. Docker and containerd state are mounted separately from
the writable root overlay.

`tools/rootfs/starter` separately owns the writable raw-GPT starter disks used
with the macOS emulator and the boot base archives. A starter disk boots
through the generic per-run `/init` with the starter `boot.env`, which grows
ext4 only after a host tool has enlarged the disk and its sole partition.
Growth happens before the real root is mounted, is skipped for non-ext4 or
multi-partition disks, and is non-fatal. Each image owns its `hvc0` getty, DHCP/DNS service,
accounts, and optional Plasma session. See the
[producer README](../tools/rootfs/starter/README.md) for its single build and
grow entry points; these images do not replace the unified immutable product
rootfs.

The [x86-64 smoke image](../tools/rootfs/x86_64-smoke/README.md) qualifies the
dedicated 4 KiB x86_64-userspace kernel slice, whose kernel mode stays native
arm64 but which cannot load arm64 userspace; its README gives the build on an
x86-64 Linux host and the bounded
`tests/integration/x86_64/run_guest_x86_64_smoke.py` boot check. The slice
registers no CTest suites, so its other runners also run directly, each with
the `macos-x86_64` `uml-darwin` and an x86-64 EROFS image such as the
[x86-64 LTP rootfs](../tools/rootfs/ltp-x86_64/README.md). That `uml-darwin`
embeds the x86-64 boot base, so
`tests/integration/boot/run_guest_boot_initramfs.py --engine gadgets` checks it
with the x86-64 base and a `uml-initramfs`.
`tests/integration/x86_64/run_guest_x86_64_rflags.py` and
`run_guest_x86_64_exceptions.py` cross-build freestanding x86-64 and i386
probes with `clang++` and `ld.lld` and check the slice's user-RFLAGS and
exception contracts in the [mainline README](../kernel/mainline-6.18/README.md)
against native x86 Linux's values. The smoke, RFLAGS and exception runners
take `--engine jit` to run x86-64 and i386 code on the macOS-only x86-64 JIT
instead of the default gadget engine; run them on both engines when a change
reaches either. `run_guest_x86_64_mm_shared.py`, which takes the same options,
checks that the host sees an address space as shared exactly when the
kernel's contract in the [common kernel README](../kernel/common/README.md)
says so, including when the sharing ends, and on the JIT that exactly the
marked dispatches run ordered code.
`run_guest_x86_64_fork_cow.py` forks from a busy multi-threaded process on
four vCPUs, as glibc's fork() does, and checks that the workers keep their
counts and that the parent's releases of the locks it held across the fork
reach its copy. `run_guest_x86_64_tso_litmus.py` runs x86 memory-order
litmus tests on four vCPUs, between threads and between processes over
shared memory, in both modes, and in cycles that share, stop sharing and
share again, whose log
must show the space marked and ordered exactly while shared. Its negative
control is the unit test `x86-64-jit-store-order`, which
shows message passing catching a JIT space that keeps no order. Both engines
pass every exception case, unmasked
`#XM`, `#MF` and `#AC`, selector loads, the LDT, segment limits and far
transfers included, in both modes.
`run_guest_x86_64_fenv.py`, which takes the same options, builds
`fenv_probe.cpp` with the image's g++, so it needs an image with a toolchain
such as the x86-64 LTP rootfs, and checks what glibc's `fetestexcept`,
STMXCSR and `fadd` show of the sticky SIMD exception flags against an Intel
Xeon's, on either engine.

`tools/rootfs/steam-x86_64` owns the gaming-only Steam image for that slice.
Its tooling audit is the `rootfs-steam-x86-64-image-audit` test; boot, session
readiness, and persistence of its separate GPT/ext4 data disk are exercised by
`tests/perf/run_steam_boot_bench.py`. The
[Steam producer](../tools/rootfs/steam-x86_64/README.md) owns its inputs,
binary audit, startup contract, and continuing graphics limitation.

## Apple builds and Kernarium

Build the general Apple libraries with:

```sh
cmake --preset ios-device
cmake --build --preset ios-device
cmake --preset ios-simulator
cmake --build --preset ios-simulator
scripts/build-xcframework.sh
```

Kernarium is an iOS device product with runtime presets for 16 KiB and 4 KiB
Linux and 16 KiB Android and no Simulator variant; see
[`graphics.md`](graphics.md#current-path).
`Kernarium/scripts/package-ios-frameworks.sh` packages all selected runtime and
graphics slices plus the static GPT fdisk and decode-only zstd XCFrameworks for
Xcode. It also packages the Apple battery provider's archive, as the Android
runtime links it, as `HostBattery.xcframework`; only `KernariumTests` links
it, so the test process reaches the provider without loading a second runtime.
`dependencies/build-ios-storage.sh --verify-only` and
`dependencies/build-ios-zstd.sh --verify-only` check both arm64 Apple slices,
their provenance and deployment targets, required APIs, and the absence of
unused command-line providers. Apple builds
validate the kernel bridge ABI, page profile, product family, and `6.18` series
before embedding an artifact; they do not bind to its patch release or source
commit and never prepare sources or invoke a kernel/AOSP build. Rootfs bundles
are launch inputs, not emulator build inputs.

Kernarium's defaults, guest bundles, and scripts are described in
[`../Kernarium/README.md`](../Kernarium/README.md).

Launch and bundle assembly are owned by `Kernarium/scripts/run-ios-mac.sh` and
`Kernarium/scripts/make-guest-bundle.py`. Use each script's `--help` rather than
copying a long invocation into documentation. `open(1)` cannot start an iOS
application on macOS and `devicectl` does not enumerate the host Mac, so Xcode's
local install service, driven by `xcodebuild test`, is the only command-line
launcher for this mode. xcodebuild passes each `TEST_RUNNER_`-prefixed
variable in its environment to the app with the prefix removed; the launcher
sets `TEST_RUNNER_KERNARIUM_ROOTFS` itself (and, with `--aquarium`, the
probe's automation settings), and a caller exports any other
`TEST_RUNNER_KERNARIUM_*` setting. An iOS application reads nothing outside its
container, so the launcher copies the rootfs, by default the accepted KDE
desktop image Kernarium itself defaults to, into
`~/Library/Containers/<uuid>/Data/Documents` under its own name rather than
referencing it in place.

The shared `Kernarium` and `KernariumAutomation` schemes gather no code
coverage. xcodebuild applies a scheme's coverage choice to every action, `build`
included, and the test plan Xcode creates for a scheme by default gathers
coverage. Each scheme therefore keeps its own test settings, so neither the
Release app a device installs, the one the launcher runs through `xcodebuild
test`, nor the automation host carries coverage instrumentation. A coverage run
passes `-enableCodeCoverage YES` to `xcodebuild test`.

For an opt-in diagnostic, pass one SLiRP forwarding specification as
`TEST_RUNNER_KERNARIUM_HOST_FORWARD` when launching the app. For example,
`tcp:127.0.0.1:5567-5555` exposes guest ADB on host loopback port 5567. Normal
launches leave the variable unset and publish no host port.

Android recovery qualification uses a fresh disposable bundle and sets
`TEST_RUNNER_KERNARIUM_ANDROID_BOOT_MODE=recovery`. Verify the OrangeFox UI,
touch input, ADB, fixed-GPT partition discovery, read-only payload mounts,
userdata/metadata mount and format behavior, backup/restore, file manager,
terminal, battery display, and normal/recovery reboot paths. Never run wipe or
restore checks against an existing user bundle.

`KernariumTests` runs on an iPad and, as an iPad app, on an Apple silicon
Mac. The iPad is the reference, and the whole suite passes there. On the Mac
up to four tests fail because of how the Mac hosts an iPad app:

- `theKeyBarNeverCoversTheConsole` and `anUnfocusedConsoleKeepsEveryRow`
  always fail. They model the key bar docked at the bottom of the screen,
  which is where the console looks for it. On the Mac the window ends above
  the bottom of the screen and the bar sits at the bottom of the window, so
  the console does not make room for it.
- `theKeyboardButtonFocusesTheViewOnScreen` and
  `anEndedRunIsNeverStartedAgain` find SwiftUI controls by walking the
  window's accessibility elements. SwiftUI's hosting view exposes them only
  while UIKit application accessibility is on in the process. A test run on
  an iPad has it on. On the Mac it follows the host's
  `ApplicationAccessibilityEnabled` preference in `com.apple.Accessibility`,
  which an assistive client turns on; while it is off, both tests fail.
- `anEndedRunIsNeverStartedAgain` also stops the guest the application boots
  as it launches, so it fails when the launch has no image to boot, as a run
  whose `TEST_RUNNER_KERNARIUM_ROOTFS` names a missing file does.

### UI automation on a device

Unattended device automation goes through the long-lived
`KernariumAutomation` UI-test host, because each new XCTest session on a
device with a passcode needs the owner's approval at the device. Its commands,
approval rules, and failure modes are in
`Kernarium/scripts/ipad-automation.py --help`. Keep all automation code in the
test targets.

## LTP

The full LTP campaign is opt-in and runs only on gadget or JIT engines.
`tests/integration/ltp/verified-tests.tsv` and `disabled-tests.tsv` are the
current exact manifests. A disabled case represents an intentional product or
kernel-policy exclusion; do not enable a kernel feature solely to increase the
test count.

The runner uses `/opt/ltp` from `rootfs.erofs` and the selected kernel bundle's
`modules.builtin`. The shipped kernels have `CONFIG_MODULES=n`; disposable
guest helpers may recognize exact built-ins but must reject unknown module
operations.

`--guest-arch x86_64` runs the same manifests on the x86_64-userspace slice
with the [x86-64 LTP rootfs](../tools/rootfs/ltp-x86_64/README.md), applying
the expectation overrides and additional exclusions in
`tests/integration/ltp/x86_64/`. An override exists only where that image's
userspace or the x86 ABI changes a case's outcome; an x86 exclusion names
hardware or host-address-space behavior UML cannot provide. See the runner's current options with:

```sh
python3 tests/integration/ltp/run_guest_ltp.py --help
```

## A64 instruction corpus

`arm64-instruction-corpus` runs the build-generated corpus from
[`../tests/unit/arm64/corpus/`](../tests/unit/arm64/corpus/README.md) through
the gadget and JIT engines of each page profile in one process, its SVE cases
through the SVE semantic library as well, and the
`arm64-instruction-corpus-freshness` tooling test checks the corpus metadata
against its pack. The corpus README owns the format, sampling policy, runner
options, and promotion of new cases, which requires a producer commit. Cases
that need engine hooks or a deferred floating-point entry state stay in focused tests
that run the production gadget and JIT engines through
`tests/unit/arm64/arm64_engine_test_support.hpp`.
`arm64-emitted-registers` checks that no host code names x18, which Darwin
clears when it interrupts a thread: every word of the gadget region, and the
JIT's translations of the corpus for this host and for one with every feature
and SME, in each link shape and SVCR mode, including each corpus word with a
register field set to 18. A word that its register oracle,
`tests/unit/arm64/a64_register_operands.hpp`, cannot classify fails it too.

The corpus covers every A64 inventory entry with representative cases; it is
not exhaustive variant proof.
[`a64-instruction-coverage.md`](a64-instruction-coverage.md) owns the
inventory tables and that completeness boundary.

The `arm64-sme-native` records hold SME results observed on the M4 at its
streaming vector length, 64 bytes. They run for each engine whose
`sme_support` is at least `complete` where the virtual CPU implements SME
at that length, which needs an SME host and the guest-SME policy, on by
default, and a
case that needs SME2 runs only for an engine at `sme2`. Elsewhere they are
skipped, and the `arm64-absent-sme` record pins every SME profile encoding
UNDEFINED on every host. The
`arm64-instruction-corpus-sme-native` tooling test re-derives their cases from
their generator, and `arm64-sme-native-oracle` drives the native oracle with
base A64 cases on any arm64 Mac. Regenerating their outcomes needs an SME host;
the corpus README describes it. The M4 has no SVE, so the native SME cases
outside streaming mode whose words the SVE profile executes there run under
the `without_sve` option, which translates them for a virtual CPU without
SVE on every host.

A corpus case with non-streaming SVE state, or one that needs an SVE profile
feature natively, runs only for an engine whose `sve_support` is `complete`
where the virtual CPU implements the SVE profile; the summary counts every
other one as skipped for `sve`. The `arm64-absent-sve` records pin every
encoding the SVE profile executes outside streaming mode UNDEFINED under
`without_sve`, and every other SVE encoding UNDEFINED there on every virtual
CPU, so both engines run them today with the guest-SVE policy off or on.

A virtual CPU with SME and without SVE executes CNT, INC, DEC, RDVL, ADDVL and
ADDPL at SM = 0 for a vector length of 16 bytes, a deliberate departure from
the architecture (see [CPU execution and
scheduling](architecture.md#cpu-execution-and-scheduling)). No host observes
the results, because an M4 traps those words, so the `arm64-sm0-vl` record
holds the project's own, written from the pseudocode, and
`arm64-instruction-corpus-sm0-vl` re-derives it. The `arm64-sme-native` records
hold no family word at SM = 0, and `arm64-instruction-corpus-sm0-vl-profile`
runs the `arm64-sm0-vl`, `arm64-absent-sve` and `arm64-absent-sme` records for
a virtual CPU with the SME profile assumed on any host, which also pins every
other SVE word UNDEFINED at SM = 0 in both engines. The unit tests
`arm64-sm0-vector-length`, `arm64-gadget-sme`, `arm64-jit-sme-control` and
`arm64-jit-sme-codegen` check the family against a model of the pseudocode, run
it in each engine, and pin the same for the rest of the encoding table.

The `arm64-sve-native` records hold non-streaming SVE results observed at a
16-byte vector length on an arm64 Linux SVE host (a Neoverse V2 core),
in both page profiles, with a 4 KiB-only record for crossings of 4 KiB
pages. Their floating-point forms are sampled across FPCR's rounding modes
and its FZ, DN, and FZ16 controls. Both engines are at `sve_support`
`complete`, so their lanes run them with the guest-SVE policy on. The
corpus's library lane runs them through the gadget engine with the SVE
semantic library behind its illegal-instruction exit, and the library covers
every form the encoding table routes; the JIT lane is checked against the
records directly. The `arm64-instruction-corpus-sve-native` tooling
test re-derives their cases from their generator, and
`arm64-sve-native-oracle` drives the native oracle with base A64 cases on any
arm64 Mac, and with SVE cases where the host has SVE at that length.
Regenerating their outcomes needs such a host and runs the oracle twice from
clean directories; the corpus README describes it.

The `arm64-sve-sme` records hold, at the same vector length, the 29 SME
forms that SVE executes outside streaming mode. No host observes them
there. Their integer and predicate families come from models of the
pinned pseudocode, each of which must first reproduce every M4 streaming
case of its encoding. Their floating-point families come from the M4 in
streaming mode with every 128-bit segment holding the case. The runner
runs them only for an engine at `sve_support` `complete` on a virtual CPU
with SME, and SME2 where named. The `arm64-instruction-corpus-sve-sme`
tooling test re-derives them. The `arm64-sve-afp` records come from the
M4 the same way. They hold those forms under FPCR.AH, FIZ, and NEP, which
the Neoverse V2 host lacks. The `arm64-instruction-corpus-sve-afp` test
checks them against their request.

The `arm64-floating-trap-native` records, and the outcomes of the
register-only `arm64-floating-trap-scalar`, `-vector` and `-control` cases,
were observed on the M1, which traps floating-point exceptions natively; any
arm64 Mac regenerates them with the same oracle. The `afp` and `bf16` records
need FEAT_AFP and FEAT_BF16 and were observed on the M4. The
`arm64-instruction-corpus-floating-trap-native` tooling test re-derives their
cases and holds every trap to one cause, and one at a program's entry to no
state change.

The `arm64-sve-trap` records hold the SVE floating-point forms with FPCR
trap enables set. No hardware observes them, since the M1 has no SVE: each
is derived from a native case's observed exceptions, or authored with
operands whose exceptions IEEE 754 arithmetic and FPRoundBase() decide,
under the trap contract the M1's records fix. The
`arm64-instruction-corpus-sve-trap` tooling test re-derives them from the
native records.

The `arm64-instruction-corpus-invalid-encoding` tooling test requires every
illegal-instruction case that the decode partitions, the allocation table, and
the SVE/SME encoding table settle to carry the invalid-encoding bit that
`tools/arm64-tests/invalid_encoding.py` decides (see the corpus README's
[invalid-encoding bits](../tests/unit/arm64/corpus/README.md#invalid-encoding-bits)).
Its `-llvm` sibling is opt-in with the pinned LLVM MC and requires LLVM to
decode the word of every case whose bit is clear.

When changing the A64 architecture oracle, its generators, the evidence
bindings, or the pinned Arm release, configure with
`-DUML_A64_ARCHITECTURE_ARCHIVE=<path>/ISA_A64_xml_A_profile-2026-06.tar.gz`
and run the opt-in `arm64-instruction-oracle-freshness` and
`arm64-sme-encoding-table-freshness` tooling tests. They regenerate the
allocation and decode-partition tables and the SVE/SME encoding table with the
pinned LLVM MC and require the checked-in bytes; without the archive or that
LLVM MC they skip.

A gadget that runs an instruction as the host's own, such as FEAT_ECV,
FEAT_I8MM, FEAT_BF16, or FEAT_FHM on an A15 iPad, runs that path only on a
host with the feature, and the M1 has none of the first three. FEAT_AFP and
FEAT_RPRES need an A17 Pro or newer, such as the iPhone 15 Pro Max or an M4
Mac. An M4 Mac has all of these features and a 1 GHz system counter, and the
unit suite, the corpus included, holds there in both engines. Elsewhere
`arm64-instruction-corpus-afp-twins` runs the FEAT_AFP twins only on the cases
whose FPCR leaves the controls clear. Check a change
to such a path by hand on an iOS device that has the feature: launch
Kernarium on the device with the `guest-hwcap-sanity` payload, which executes
every family the guest advertises, and with the corpus cases of the touched
families built into one static guest program, and require the same results as
a gadget-engine run of both programs on a Mac. The device runs only the
gadget engine, so a JIT path that needs one of these features runs only on an
M4 or newer Mac.

## AArch32 decoder

`arm64-aarch32-decoder` (unit, both page profiles) decodes the stored sample
words of every row of the encoding table and the whole 16-bit Thumb space. It
compares each answer with the one `tests/tooling/arm64/a32_expected.py`
computes from the same XML pseudocode independently of the C++ translation,
and requires LLVM MC to have decoded whatever the decoder accepts. The
`arm64-a32-encoding-table` tooling test checks the generated tables and the
test data against each other offline.

To change the table, the profile policy (`a32_profile_policy.py`) or the pinned
release, run `tests/tooling/arm64/generate_a32_encoding_table.py
--architecture-archive <ISA_AArch32_xml_A_profile-2026-06.tar.gz>`; it needs
LLVM MC 22.1.8 and takes a few minutes. Configure with
`-DUML_A32_ARCHITECTURE_ARCHIVE=<archive>` to run the opt-in
`arm64-a32-encoding-table-freshness` tooling test, which regenerates the
outputs (the tables, the test data and the instruction inventory) and
requires the checked-in bytes; without the archive or that LLVM MC it skips.
The `arm64-a32-instruction-inventory` tooling test checks the inventory against
the decoder's table and the counts in
[`a32-instruction-coverage.md`](a32-instruction-coverage.md).

## AArch32 corpus

`arm64-aarch32-corpus` (unit, both page profiles) checks every case of the
generated AArch32 corpus against the decoder: the program decodes in order,
in the IT state its instructions leave, to legal forms except at the
instruction that raised an event; the instruction under test is the form the
case names; the event is one its kind can raise; and the retired count, fault
position, state changes and memory runs agree. A case whose forms need a level
above `--level` is skipped. The `arm64-a32-corpus` tooling test regenerates
the pack from `tests/unit/arm64/aarch32_corpus/source` and compares it,
`provenance.json`, `manifest.tsv`, `coverage.tsv` and `excluded.tsv` with the
checked-in bytes, and requires cases for every executed base inventory entry.
Neither needs an oracle. The corpus README owns the format, the oracles and how
to record a suite with `tools/arm64-tests/a32-corpus.py`; recording runs
`qemu-arm` on a build host over ssh and is opt-in.

## AArch32 semantic library

`arm64-aarch32-semantics` (unit, both page profiles) checks the library's
contract that the corpus cannot record: fault ordering and the unchanged
state at a fault, the exclusive monitor, the single-instruction entry's IT
handling, the events, the library-exit counters and the coverage predicate.
`arm64-aarch32-semantics-corpus` is the corpus's model lane: it runs every
case of the pack through `step_a32()` on a model of the oracle's address space
(page by page, so a crossing access splits across separate host pages) and
requires the recorded event, retired count, fault address, registers, flags,
FPSCR, D registers, thread registers and data bytes, with 0 failures. The
library covers the whole profile, so a case with a form it does not cover is a
failure of the lane too; `--skipped` lists such forms. It runs the BFloat16,
8-bit matrix multiply and FHM forms on any host, because the library does not
need those features. Run one record with
`uml_arm64_aarch32_semantics_corpus_tests <pack> --record <name>`, one case
with `--case`, and `--trace` for its steps and initial state.

## Guest SME policy

Unit binaries take the guest-SME policy from their environment, like any
run: on by default, off with `DARWIN_UML_GUEST_SME=0`. The suites hold
either way on every host, and a host without SME sees no difference.
`arm64-sme-foundation` checks the prober against the host's own sysctls under
either policy, and the identification registers and HWCAP2 against arm64
Linux's table with injected SME hosts at every SVL from 16 to 256 bytes and
every engine support level. On
an SME host, an M4 Mac, also run it, `arm64-advertised-features`,
`arm64-jit-sme-control`, `arm64-jit-sme-register`,
`arm64-jit-sme-memory` and `arm64-instruction-corpus`, which then runs the
native SME records on both engines, with the policy at its default and with
`DARWIN_UML_GUEST_SME=0`.
Elsewhere `arm64-jit-sme-control` runs only its SM=0 cases, on an injected
profile, `arm64-jit-sme-register` and `arm64-jit-sme-memory` skip, and the
corpus skips its native SME records. Integration runs read the policy the same
way: on an M4, `guest-gadgets-smoke` and `guest-jit-smoke` expect the SME
HWCAP2 bits and `guest-hwcap-sanity` executes the SME families.

`guest-sme-gadgets` and `guest-sme-jit` check the guest kernel's SME contract
with the policy at its default. Each skips, naming its reason, unless the host has SME
and its engine's SME support is `control` or better. The freestanding
`sme_probe.cpp` boots as `/init` and checks signal frames, invalid
sigreturns, the system-call exit from streaming mode, fork, vfork, clone,
exec, regsets, `AT_MINSIGSTKSZ`, prctl and the sysctl, first through
sigreturn and ptrace alone, then with SME executed by the engine; further
boots send host signals and each terminating signal while a vCPU streams,
and `glibc_za_probe.py` checks the image's glibc and libgcc SME ABI: the ZA
lazy-save commits at fork, spawn, thread creation, longjmp and siglongjmp,
a glibc-installed handler for a signal that interrupts a streaming region,
returning or leaving by siglongjmp, and libgcc's SME support routines. The
`kernel`, `spin` and `terminate` modes need no engine support; on an SME host
they run by hand with
`run_guest_sme.py --minimum-support none --modes kernel,spin,terminate`.
`guest-sme-off-gadgets` and `guest-sme-off-jit` run on every host and check
that with the policy forced off the probe sees no SME.

Two opt-in tools list what SME code executes outside streaming mode, where
an SVE word is UNDEFINED on an SME CPU without SVE unless it belongs to the
SM=0 family (CNTB/H/W/D, RDVL, ADDVL, ADDPL and the scalar INC/DEC
B/H/W/D). `tools/arm64-tests/sme-sm0-scan.py` takes the shipped images (the
desktop EROFS images and the Android release directories), extracts them
with `fsck.erofs`, `debugfs` and `zstd`, and walks every SME function of
every AArch64 ELF file, APK and APEX payloads included, from its inferred
entry mode; any other SVE word reachable at SM = 0 is a decision to extend
the family or withhold SME from that workload, and `--expect` pins a
binary's words (Chromium's libyuv executes only CNTD there).
`tools/arm64-tests/sme-idiom-corpus.py` builds `sme_idioms.cpp` with the
given clang and GCC compilers for SVE and SME-only targets, every
optimization level and callee-saved register pressure, and reports the
same classification for each compiler idiom, callee-saved Z and P spills
included. The `arm64-sme-sm0-scan` tooling test checks the classifier and
both tools on synthetic ELF fixtures.

## Guest SVE policy

`arm64-sve-foundation` checks the guest-SVE policy, the host features each
part of the SVE profile needs, the engines' support levels, the predicate
mask form and the SM-change reset, with injected hosts. Unit binaries take
that policy from their environment as well; the suites must hold unchanged
with `DARWIN_UML_GUEST_SVE=1`.
`arm64-jit-sve-control` checks the JIT's control core against the
pseudocode on injected SVE hosts under either policy, and
`arm64-jit-sve-forms` checks every row of the profile through the JIT
against the semantic library on random states, requires a lowered word to stay
out of the library from a clean state, runs FSCALE with each kind of element
(zeros, infinities, NaNs, denormals, scales that overflow, underflow or wrap)
and requires exactly those to take the library, runs the conversions between
precisions and UQSHL at the values they turn on, and runs gathers and scatters with a
lane out of reach, scatters while another vCPU's reservation tracks stores,
the saturating counts at their limits, table lookups with indices that land
in the tables, and the governing-predicate shapes (none, one, all but one, runs,
every subset of the narrow sizes) of the breaks, COMPACT and SPLICE. It
requires a block of SVE memory words to check the top-byte-ignore state once,
runs the words with tagged and untagged bases, requires the loads of one base
to share a translation exactly where a window allows (words between them, base
changes, stores, other bases, the 256-byte span and the 17-word limit) and runs
them against the library inside a page, across its end and across the mapped
pages, and also runs the 29 forms
that FEAT_SME and FEAT_SME2 share with FEAT_SVE2p1 (the SCLAMP class), which
execute at SM = 0 only with SME beside SVE, on an injected profile with SME and
again with SME2.
`arm64-jit-sve-codegen` compiles every row of the profile (with SME and SME2
as well) and every evidence witness at SM = 0 in both ZA states and every
link shape, and requires that no translation holds an SVE or SME word.

`DARWIN_UML_SVE_LIBRARY_EXITS=<file>` counts, for the whole run, the SVE
words that leave translated code for the semantic library, per table row, and
writes the report to the file as each executor retires and as the process ends
(the emulator leaves through `_Exit`; the last write is the complete one). The
JIT and the gadget engine write the same report: a header and one
tab-separated line per row, most exits first, with the form, the row, its
encoding, the translations, those that ended the block in the library and the
exits. `tools/arm64-tests/sve-library-report.py` names and ranks the forms
of either engine's report and sums several. The words a workload sends there
are the ones worth lowering. `arm64-jit-sve-forms` runs a child process
through `_Exit` and reads the report it leaves, and one that exits having run
no JIT executor and requires it to leave a report already in the file alone,
since either engine's process may write it.

`guest-sve-gadgets` and `guest-sve-jit` check the guest kernel's SVE contract
under the policy on. Each skips, naming its reason, unless its engine's SVE
support is `complete`. The freestanding `sve_probe.cpp` boots as `/init` and
checks signal frames, sigreturn and its invalid frames, the live state across
system calls, preemption, fork, vfork, clone and exec, `NT_ARM_SVE` at
signal and syscall-entry stops and across a traced fork, `AT_MINSIGSTKSZ`,
prctl and the sysctl; on an SME host a second boot adds the SME policy and
the streaming-mode checks. The same binary runs as an ordinary program on
arm64 Linux with SVE at a 16-byte vector length, `sve-probe kernel`, and
every check must pass there too; its expected values came from such a run on
Linux 6.12, and checks where native Linux may keep a discarded state live
with zero registers accept either outcome. Where 6.12 differs from the
guest's Linux 6.18, the guest is held to 6.18 and only a native run accepts
6.12. `guest-sve-off-gadgets` and `guest-sve-off-jit` run on every host and
check that with the policy off the probe sees no SVE and PTRUE is undefined.

## Guest AArch32 foundation

`arm64-a32-foundation` checks the state `CpuState` appends and its assembly
offsets, the guest-AArch32 policy, the engines' support levels and what a
policy presents of them, the AArch32 identification registers and compat
HWCAP words at each level over every subset of the host features they follow,
and that both engines take the AArch32 entry as the illegal-instruction exit
without changing the state. `arm64-a32-guest-key` checks the tagged
translation keys (round trips, interworking, that no key collides with an A64
key, the empty cell or a fault position, and the hash-visible bit) and the
key-aware validation of saved guest units across page boundaries of both
guest page sizes. `arm64-jit-shared-translation-cache` also checks that the
shared tier keys and sizes AArch32 translations by ISA. Unit binaries take the
policy from their environment as well; the suites must hold unchanged with
`DARWIN_UML_GUEST_AARCH32=1`.

## x86 instruction conformance

The 4 KiB x86 gadget backend uses both handwritten x86-64 and i386 ASM suites
from the pinned FEX `x86-conformance` dependency as its external instruction
campaign. For the gadget engine FEX is a test input only; FEXCore is linked
into nothing but the macOS-only x86-64 JIT.
Materialize each mode with the pinned NASM producer and
`tools/fex-tests/build-asm-corpus.py`; generated corpus data stays outside the
repository. Run every binary through the test-only gadgets adapter with
`tools/fex-tests/run-asm-corpus.py`. Generic upstream-disabled cases remain
explicit exclusions, while FEX backend- and host-specific exclusions do not
apply to gadgets. Programs that request FEX's reduced-precision x87 mode run
at full precision against their natively produced expectations. Acceptance
requires every applicable materialized case in both modes to pass under the
policy, which sets aside the expectations x86 hardware contradicts (upstream's
`Known_Failures_host` and the project's record, as the
[campaign README](../tools/fex-tests/README.md) describes), with no decoder or
semantic failures; each JSON report records the pinned source identity, mode,
applicable count, upstream-disabled and set-aside cases, both pass rates, and
per-test result.

The x86-64 JIT takes the same campaign: give `run-asm-corpus.py`
`uml_x86_64_fex_asm_jit_runner`, which runs i386 binaries in a compat32
context. The macOS 4 KiB unit tree also runs the build-generated x86-64 corpus
through the x86-64 JIT. `x86-64-jit-instruction-corpus` holds it to
`tests/unit/x86_64/jit_corpus_exclusions.tsv`, whose rows name each record the
engine is known to fail with its class and fail the test when the record passes
or names nothing. `x86-64-jit-emitted-registers` repeats the run and walks
every block FEXCore translates, failing on a reachable instruction that names
x18 or that the same oracle cannot classify, or on a block that does not parse.
`x86-64-jit-code-buffer-growth` repeats the run from one byte
of host code per IR node, requires the code buffer to grow, and requires the
same results. `x86-64-retirement` and `x86-64-jit-retirement` hold the gadget
engine and the JIT to the same count of retired instructions before each kind
of stop, in long64 and compat32, including a store into the code of the block
running it. `x86-64-masked-flags` and `x86-64-jit-masked-flags` run a subset
of XE2's request sets through each engine's ordinary path with every
exception masked and hold the MXCSR flags each instruction leaves to an Intel
Xeon's, except the documented inexact cases the test names. The
`ios-products-carry-no-fexcore` tooling test checks the script the iOS
packaging runs to keep FEXCore out of every iOS product.

## Performance and external campaigns

Benchmarks, full LTP, external networking, KDE/VNC/WebGL desktop campaigns,
GPU conformance, SMP stress, official A64 semantics, and connected-device runs
are opt-in. Run them only when the change or question crosses that boundary.

Geekbench runs the image-local payload, or an attached payload image checked
by its SHA-256 for an image without one, such as every x86-64 and armhf image,
through the checked-in workload script with `--no-upload` and requires no
Internet preflight. The workload runs Geekbench 6, or Geekbench 5's armv7
build on an armhf image. Manual performance tools and the probe inventory are
under [`../tests/perf/README.md`](../tests/perf/README.md). Record conclusions in
[`optimization-history.md`](optimization-history.md), not raw logs or a new
investigation document.

GPU capabilities, qualification boundaries, and unsupported Vulkan work are
summarized in [`graphics.md`](graphics.md).

## Graphics conformance campaigns

Guest CTS qualification is opt-in, receipt-bound, and separate from the
`*-venus*` workflows above, which never launch a guest or make a Vulkan support
claim. Every campaign guest is JIT with exactly four vCPUs and 4 GiB.

Two rules bound what any graphics run may be used to claim.

A version or capability rises only when natural capset discovery and a
functional test agree. `MESA_GL_VERSION_OVERRIDE`, `MESA_GLES_VERSION_OVERRIDE`,
forced Virgl capability bits, and extension-string injection are never
evidence, in a run or in a report.

A run must exercise the real Virtio-GPU/Virgl path. An app-owned framebuffer, a
Chromium-specific library preload, or any other substitution for the guest's own
driver stack invalidates the result for capability purposes.

`tests/integration/gpu/run_vk_cts.py` runs the Vulkan lane,
`tests/integration/gpu/run_vk_gl_cts.py` the OpenGL and OpenGL ES lanes, and
`tests/integration/gpu/run_graphics_cts_campaign.py` coordinates both into one
campaign. Each runner's `--help` describes its selection modes, inventories,
display setup, guest sessions, shard pool, and restart options.

A qualification run is bound to its inputs. A Vulkan run is bound to
`tests/integration/gpu/vulkan_cts_receipt.json`: the sealed image's receipt,
the CTS and Mesa source locks, and the staged KosmicKrisp provider digest,
which must match the configured verified prefix so a run cannot silently use
an older provider. An OpenGL matrix run is bound to the combined image's
schema-4 receipt and to an ordered EGL configuration inventory captured from
the same emulator, image, and provider. Window-system cases render on real surfaces:
native Xorg with Virgl-backed glamor and, for Vulkan selections that include
Wayland, the image's own KWin nested on Xorg. Xwayland and Xvfb are rejected.

A shard attempt passes only when its guest ran exactly the enumerated
selection, in released traversal order and each case once, delivered its
whole QPA, stopped with a clean Virtio-GPU takedown, and reported only
statuses the receipt's runner oracle accepts. Accepted `NotSupported`,
warning, and `Waiver` results stay distinct from `Pass` and prove nothing
about an optional feature. A campaign is complete when every shard has one
attempt that passes, whichever generation of inputs ran it. A complete
campaign is not an all-`Pass`, all-features-supported, or Khronos conformance
claim. The campaign runner's `--help` gives the fixed plan, the input
generations, and the evidence layout that `--verify-only` reparses without
launching a guest.

### OpenGL ES3 FBO state-query waiver

`tests/integration/gpu/vk_gl_cts_waivers.json` holds the granted waivers. Its
one entry covers the fifteen `dEQP-GL45-ES3.functional.state_query` cases that
assert default-framebuffer state in an `es3-main` run that draws into a
framework-owned framebuffer object; a conforming driver reports the bound
object there, so those failures belong to the configuration rather than to the
driver. The project owner granted it on 2026-09-08 for Zink over Venus and
KosmicKrisp.

The matrix produces such a run only where a must-pass record asks for that
surface itself. A record naming a GL config selects the framework framebuffer
object only when its own command line also names that surface; the ES-for-GL
records name a config without it, so they keep the surface their EGL config
provides and do observe default-framebuffer state.

A waiver applies only to a matrix run whose API, configuration name, and draw
surface match its scope. The runner installs the official waiver file in the
guest, the launcher verifies its digest, `glcts` receives `--deqp-waiver-file`,
and the guest records the applied waiver URL in the QPA session header.
Verification accepts `Waiver` only for the listed cases of such a run and only
when that header names the granted list; a listed case that still fails, a
`Waiver` anywhere else, or a missing header is rejected. `run.json` records
every waived result, the campaign aggregate counts `waived_cases` per run, and
`unwaived_runner_success` is false whenever a waiver was used. A waived result
is not a pass and makes no conformance claim.

### Framebuffer completeness compatibility warning

`KHR-GL46.direct_state_access.framebuffers_check_status` grades three of its
observations optional, because the specification does not make the statuses
they expect mandatory, and reports `CompatibilityWarning` when one of them sees
`GL_FRAMEBUFFER_COMPLETE` instead. The runner policy above counts that status as
a success, and this case is expected to report two of those observations;
[`graphics.md`](graphics.md#guest-capability-boundary) explains why both
framebuffers are complete.

## Claiming completion

A passing structural audit does not prove generated-code shape, performance,
kernel boot, or guest behavior. A unit test does not prove an external
campaign. Report only checks actually run and clearly identify any external
input or campaign that was unavailable.

Do not normalize compiler metadata or require byte-identical independent
builds. For a measured A/B, record the revision and dirty state, relevant
configuration, command, result, and exit status; Git and the owning manifests
provide the remaining provenance.
