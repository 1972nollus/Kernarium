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

The helper that registers a test sets its suite label; `ctest -N -L <suite>`
lists a configured tree's registrations. A unit test without its own `TIMEOUT`
gets a 600 s bound, so a wedged binary fails the suite instead of stalling it.
Performance and long external campaigns are not CTest correctness suites.

Tooling tests run tools against fixtures; they do not grep tracked source for
expected tokens or restate values that another file owns.

Unit tests create scratch files through `tests/unit/scratch/`, never at a
host-global path unless that path is the production contract under test, as
for the mconsole socket. Each registration owns `test-scratch/<test>` in the
build tree; a binary run outside CTest uses `$TMPDIR`. Unix-domain socket paths
use `$TMPDIR` because a build-tree path may not fit `sun_path`. Tests remove
what they create, so a leftover file names the test that leaked it.

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
its owning workflow; for tooling tests alone that is:

```sh
cmake --workflow --preset check-tooling
```

Specialized workflows are:

```sh
cmake --workflow --preset check-gpu
cmake --workflow --preset check-gpu-asan-ubsan
cmake --workflow --preset check-loader-asan-ubsan
cmake --workflow --preset check-jitprof
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
broken iOS graphics closure fails in seconds rather than inside a guest. The retired Simulator and device GLES
lanes verified the removed ANGLE provider and have no replacement.

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
`guest-console-input-burst` writes an 11 KiB burst to `ttyS0` in one host write,
enough to throttle the tty, and then requires later typed input to reach the
shell. `guest-console-concurrent-input` streams 1 MiB of numbered lines to a
reader on one of four vCPUs. The reader is slower than the stream, so the line
throttles repeatedly, and each unthrottle drains the host channel while new
input raises the line's interrupt on another CPU. Every line must arrive whole
and in order.

`guest-balloon-config-churn` gives a one-vCPU guest an external balloon backend
that raises a config change every 2 ms, so config-change requests cross the
driver's config reads constantly; the guest must keep running commands and read
the config thousands of times. The backend runs in its own process and so cannot
serve virtqueues: guest RAM is resolvable only inside the emulator.

`guest-bpf-callbacks` runs hand-assembled BPF programs through `bpf()` from the
rootfs Python: `bpf_loop`, inlined and through the helper,
`bpf_for_each_map_elem`, a self-rearming BPF timer, nested callbacks, and tail
calls into callback programs. It checks that the verifier's frame, stack, and
tail-call limits still reject, and exhausts and refills the interpreter's
callback entry-point pool.

`guest-boot-initramfs` boots the launcher's base and per-run initramfs. The
generic `/init` must mount the image beneath a tmpfs overlay, install the
payload and switch root into the payload's entry as PID 1; the same payload
must boot from an initrd written by `uml-initramfs`. The base alone, a segment
appended without padding, an initrd the kernel never loads and an unknown
`/boot.env` key must each stop the guest with their named reason and a nonzero
exit, and the launcher must remove its scratch initrd, including one an
aborted run left behind.

`guest-kernel-nofault` boots the rootfs and checks, through a sysrq t dump,
that a faulting nofault read recovers silently and that every trace entry is
reliable, as [`../kernel/common/README.md`](../kernel/common/README.md) specifies.

`guest-user-strings` boots the rootfs and checks the strings system calls copy
from user memory against the copy contract in
[`../kernel/common/README.md`](../kernel/common/README.md).

`guest-native-window-unmap` maps a page just below and just above the Darwin
kernel's native window, which it finds from the addresses `mmap()` refuses,
and unmaps each alone and both with one `munmap()` across the window. The
page tables freed with them widen the kernel's TLB flush over the window, and
the guest must survive each unmap and run another command.

`guest-ptrace-pstate` writes a stopped child's PSTATE through
`PTRACE_SETREGSET` and `PTRACE_POKEUSER`. SS and the RES0 bits must read back
cleared and TCO, DIT and SSBS kept, a mode or DAIF bit must fail with
`EINVAL`, and the child continued after an SS write must exit rather than
stop on a single-step trap.

`guest-run-end-stop` needs only the kernel image: it stops embedded guests
throughout their boot and checks the run-end and SIGALRM contracts in
[`../kernel/common/README.md`](../kernel/common/README.md).

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

`tools/rootfs/x86_64-smoke` owns the separate, pure x86-64 EROFS image used to
qualify the dedicated 4 KiB x86_64-userspace kernel slice. Kernel-mode
execution remains native arm64, but that slice cannot load arm64 userspace.
Build it on an x86-64 Linux host and run the bounded
`tests/integration/x86_64/run_guest_x86_64_smoke.py` boot check; it is not a
replacement for the accepted general-purpose rootfs. The slice's own
`uml-darwin` embeds the x86-64 boot base, so check it with
`tests/integration/boot/run_guest_boot_initramfs.py --engine gadgets`, the
`macos-x86_64` `uml-darwin`, any x86-64 EROFS image such as the
[x86-64 LTP rootfs](../tools/rootfs/ltp-x86_64/README.md), the x86-64 base
and a `uml-initramfs`.

`tools/rootfs/steam-x86_64` owns the gaming-only Steam image for that slice.
Its tooling audit is the `rootfs-steam-x86-64-image-audit` test; boot, session
readiness, and persistence of its separate GPT/ext4 data disk are exercised by
`tests/perf/run_steam_boot_bench.py`. Interactive Steam launches retain that
disk across boots and rootfs replacements. The
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
Xcode.
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

`KernariumTests` runs on the iPad and, as an iPad app, on an Apple silicon
Mac. The iPad is the reference, and the whole suite passes there. On the Mac
four tests fail because of how the Mac hosts an iPad app:

- `theKeyboardButtonFocusesTheViewOnScreen` and
  `anEndedRunIsNeverStartedAgain` find SwiftUI controls by walking the
  window's accessibility elements. On the iPad a test run has UIKit
  application accessibility on, and the walk finds every control. On the Mac
  it is off, and SwiftUI's hosting view then exposes no elements. Turning it
  on in the process makes the same walk find them.
- `theKeyBarNeverCoversTheConsole` and `anUnfocusedConsoleKeepsEveryRow`
  model the key bar docked at the bottom of the screen, which is where the
  console looks for it. On the Mac the window ends above the bottom of the
  screen and the bar sits at the bottom of the window, so the console does
  not make room for it.

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

`arm64-instruction-corpus` runs the checked-in corpus in
[`../tests/unit/arm64/corpus/`](../tests/unit/arm64/corpus/README.md) through
the gadget and JIT engines of each page profile in one process, and the
`arm64-instruction-corpus-freshness` tooling test checks the corpus metadata
against its pack. The corpus README owns the format, sampling policy, runner
options, and regeneration, which requires a producer commit. Cases that need
engine hooks or a deferred floating-point entry state stay in focused tests
that run the production gadget and JIT engines through
`tests/unit/arm64/arm64_engine_test_support.hpp`.

The corpus covers every A64 inventory entry with representative cases; it is
not exhaustive variant proof.
[`a64-instruction-coverage.md`](a64-instruction-coverage.md) owns the
inventory tables and that completeness boundary.

## x86 instruction conformance

The 4 KiB x86 gadget backend uses both handwritten x86-64 and i386 ASM suites
from the pinned FEX `x86-conformance` dependency as its external instruction
campaign. FEX remains a test input and is not linked into the runtime.
Materialize each mode with the pinned NASM producer and
`tools/fex-tests/build-asm-corpus.py`; generated corpus data stays outside the
repository. Run every binary through the test-only gadgets adapter with
`tools/fex-tests/run-asm-corpus.py`. Generic upstream-disabled cases remain
explicit exclusions, while FEX backend- and host-specific exclusions do not
apply to gadgets. Programs that request FEX's reduced-precision x87 mode run
at full precision against their natively produced expectations. Acceptance
requires every applicable materialized case in both modes to pass with no
decoder or semantic failures; each JSON report records the pinned source
identity, mode, applicable count, upstream-disabled cases, and per-test
result.

## Performance and external campaigns

Benchmarks, full LTP, external networking, KDE/VNC/WebGL desktop campaigns,
GPU conformance, SMP stress, official A64 semantics, and connected-device runs
are opt-in. Run them only when the change or question crosses that boundary.

Geekbench runs the image-local payload through the checked-in workload script
with `--no-upload` and requires no Internet preflight. Manual performance tools and the probe inventory are under
[`../tests/perf/README.md`](../tests/perf/README.md). Record conclusions in
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
attempt that passes, whichever generation of inputs ran it. Its plan -- the
suites, page profile, images, receipts, and EGL configuration set -- never
changes, except that a relaunch may add a suite, and a catalogued attempt
whose evidence was removed fails the campaign closed. A complete campaign is
not an all-`Pass`, all-features-supported, or Khronos conformance claim.

A campaign directory holds `campaign.json`, which records the plan, every
generation of inputs, the attempt catalogue, the state and, once the campaign
is complete, its aggregate status counts. A shard's first attempt lives in
`shards/<shard>/` and later ones in `retries/<shard>/<NNNN>/`. Each attempt
keeps its `attempt.json`, `runner.log`, and guest ownership journal, and under
`driver-output/` the leaf run's `run.json`, console log, and transported
`artifacts/`, including `results.qpa` and `summary.txt`. `--verify-only`
reparses that evidence without launching a guest.

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
