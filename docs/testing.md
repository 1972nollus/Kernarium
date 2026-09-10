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

## Graphics conformance campaigns

Guest CTS qualification is opt-in, receipt-bound, and separate from the
`*-venus*` workflows above, which never launch a guest or make a Vulkan support
claim. Every campaign guest is JIT with exactly four vCPUs and 4 GiB.

X11 CTS surfaces use native Xorg with virtio DRM modesetting and Virgl-backed
glamor. The launchers verify the server executable, DRM file descriptor,
DRI3 and Present extensions, connected output, and accelerated rendering;
Xwayland and Xvfb are rejected. OpenGL explicitly selects X11 for EGL.
Vulkan selections that include Wayland also start the image's unmodified
KWin Wayland compositor nested on Xorg, without Xwayland, so mixed WSI cases
retain their released traversal order and working Wayland surface support.
The launchers stop their display processes before requesting guest shutdown.

`tests/integration/gpu/run_vk_cts.py` owns the Vulkan lane. `--qualification`
binds the run to `vulkan_cts_receipt.json`: the sealed image's receipt, the CTS
and Mesa source locks, and the staged KosmicKrisp provider digest, which must
match the configured verified prefix so a run cannot silently use an older
provider. `--mode exact-family` selects cases in released traversal order; a
reordered list is rejected. Accepted `NotSupported` and warning results stay
distinct from `Pass` and prove nothing about an optional feature.

`tests/integration/gpu/run_vk_gl_cts.py` owns the OpenGL lane. `--api gl46
--leaf gl46 --case` runs one core `KHR-GL46.*` case; `--matrix gl46` runs the
official XML-derived must-pass configurations by
`--mustpass-config-index`/`--fraction` and rejects `--case`. The ES-for-GL
leaves (`dEQP-GL45-ES3.*`, `dEQP-GL45-ES31.*`, `KHR-Single-GL46.*`) exist only
as matrix shards. Seeds, dimensions, and FBO settings come from the XML
records, and an ordered `CTS-Configs.gl46` inventory captured from the exact
emulator, image, and provider (`--config-inventory gl46`, replayed with
`--config-inventory-run`) assigns the first/other EGL configuration roles.

`tests/integration/gpu/run_graphics_cts_campaign.py` coordinates both suites:
eight Vulkan fractions and eight deterministic fractions per OpenGL run, at
most six owned guests at a time (`--jobs` may only lower that), immediately
refilled. It freezes source, runner, emulator, framework, rootfs, receipt,
selection, and timeout identities per attempt, and reverifies completed ones.

A campaign is complete when every shard has one attempt that validates, not
when every shard passed on its first try. Restarting reruns only the shards
that have not passed; a shard that failed is retried in a new directory beside
the evidence its earlier attempt left, and `--max-attempts` bounds how often.
The inputs that may change between attempts are the emulator and the source
tree: each distinct set is recorded as a generation, and every attempt is
judged only against the generation that ran it, so a campaign may span several
emulators without any single result meaning less. What may never change is the
plan -- the suites, page profile, images, receipts, and EGL configuration set
that decide which shards exist -- because that is what "every shard passed"
refers to. A catalogued attempt whose directory has been removed still fails
closed: retrying a failure is ordinary, but no later run can restore what
deleted evidence proved.

With `--detach-on-signal`, an interrupted pool leaves its running leaves,
guests, and emulators alive instead of stopping them, and the next pool over
that campaign adopts them: they occupy their slots exactly as if it had started
them, so restarting to pick up a new emulator costs no in-flight shard its
work. An adopted leaf is not the adopting pool's child, so its exit status
comes from the file the leaf writes for itself; an older leaf that predates
that file is held to its ownership journal and artifact evidence alone. Even with `--keep-going`, a host-observed emulator
stall or dirty DRM shutdown cancels the remaining pool through its owned-process
cleanup and preserves the attempted shards' evidence. Ordinary CTS result
failures retain `--keep-going` behavior. One stall is exempt: a guest that
transported and had acknowledged its final artifact frame and then missed its
shutdown grace cost the shard no coverage, so the host reaps that guest, records
a `shutdown_stall` beside the session and in `run.json`, keeps the complete
evidence, and lets the pool continue. That transcript still has to prove a clean
Virtio-GPU takedown; only the clean guest stop marker is waived for it. `--verify-only` reparses all evidence without launching
a guest and `--dry-run` prints the derived shard commands. A leaf arms its
ownership journal before creating a child, so a repeated interrupt cannot skip
the wait/kill fallback, and the pool releases a slot only after a run-token
journal acknowledges that exact guest's reap and console/socket cleanup;
`gpu-guest-process-ownership-safety` exercises that boundary host-only. A
complete accepted-status campaign is not an all-`Pass`, all-features-supported,
or Khronos conformance claim.

Every CTS guest masks the image's desktop stack and its container runtime
(`docker.service`, `docker.socket`, `containerd.service`). No suite uses either,
and a short shard powers off while the runtime is still starting: tearing its
freshly loaded BPF programs down from that state leaves kworkers blocked in
`bpf_prog_free_deferred`, with `systemd` blocked behind them and the guest never
reaching poweroff.

A shard is one or more guest sessions. The guest transports its growing QPA as
ordered checkpoint segments, so a killed guest still leaves the prefix it
produced, and the host reassembles the single `results.qpa` every consumer
reads. The progress watchdog is CPU-aware: a case that produces no QPA output
for `--console-stall-timeout` seconds while its `glcts` processes also burn no
CPU is abandoned, while a case that is silent but computing survives until
`--console-stall-busy-timeout` (campaign default 3600 s), because guest-side
software fp64 cases run for 700-2000 s without logging whereas a wedged Venus
wait consumes nothing. The host then records the case in flight as `Timeout` or
`Crash`, never retries it, and continues in a fresh guest bounded by
`--max-resumptions`. Both statuses are always failures.

### OpenGL ES3 FBO state-query waiver

`tests/integration/gpu/vk_gl_cts_waivers.json` holds the granted waivers. Its
one entry covers the fifteen `dEQP-GL45-ES3.functional.state_query` cases that
assert default-framebuffer state in the `es3-main` configuration, whose
must-pass record renders into a framework-owned framebuffer object; a
conforming driver reports the bound object there, so those failures belong to
the configuration rather than to the driver. The project owner granted it on
2026-09-08 for Zink over Venus and KosmicKrisp.

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

### Native whole-submit timing

The KosmicKrisp line and geometry unit executables accept the opt-in arguments
`performance` (render-only Vulkan queries) and `performance-whole-submit`
(native Metal commit feedback through `MESA_KK_DEBUG=submit_timing`);
`tests/perf/uml-kosmickrisp-rgb32-texel-bench` uses the same collector through
`--timing metal-whole-submit`. Registered CTests never run these modes. The
collector permits one outstanding submission and requires exactly one finite,
ordered, wall-bounded callback per draw; missing, malformed, duplicate, late,
or unmatched records fail closed rather than being filtered. Whole-submit
intervals include every clear, barrier, and copy in the Metal commit and are
not comparable with render-only query intervals. No speed ratio is a
correctness threshold.

## Claiming completion

A passing structural audit does not prove generated-code shape, performance,
kernel boot, or guest behavior. A unit test does not prove an external
campaign. Report only checks actually run and clearly identify any external
input or campaign that was unavailable.

Do not normalize compiler metadata or require byte-identical independent
builds. For a measured A/B, record the revision and dirty state, relevant
configuration, command, result, and exit status; Git and the owning manifests
provide the remaining provenance.
