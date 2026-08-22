# Testing and preparation

## Current accepted state (2026-08-10)

The complete refactor acceptance remains anchored at implementation baseline
`783bfcf7e2447f8f981830a158bec3f402b08d97`. That baseline passed the ordinary
16 KiB and 4 KiB unit suites at 572/572 and 573/573, respectively, with
tooling at 51/51 in each profile. Full ASan+UBSan passed 572/572, TSan passed
8/8, the malformed loader ASan gate passed 1/1, and both normal and sanitized
GPU configurations passed 5/5 units plus the 1/1 VNC tooling test.
`integration-fast` passed 5/5 in each page profile, and the standalone SMP
workload reached 8.19 and 8.03 effective host cores at 16 KiB and 4 KiB. The
three-slice Apple package and its raw, installed-tree, and real Xcode consumers
passed. Those results are not silently reattributed to the later rootfs
migration.

The subsequent unified-rootfs migration supersedes the old partitioned-image
policy. Its final host workflows passed 572/572 units plus 49/49 tooling tests
at 16 KiB and 573/573 plus 49/49 at 4 KiB. Both production page profiles were
rebuilt; the latest regenerated `integration-fast` suites passed 5/5 in 65.42
seconds at 16 KiB and 5/5 in 69.33 seconds at 4 KiB with raw EROFS at
`/dev/ubda`: overlay/systemd, interpreter, gadgets, JIT, and deterministic
SLiRP. The complete integration workflows also passed 52/52 tooling checks in
each profile. Their overlay/systemd tests passed in 34.06 and 38.19 seconds,
respectively, with separate Docker and containerd tmpfs mounts and an offline
source-built hello-world container. Focused rootfs, provisioning, launcher,
GPU-runner, LTP-runner,
network-runner, performance-runner, emulator-option, and API contracts also
pass. The 16 KiB guest reports
the upstream `EXPERIMENTAL EROFS subpage compressed block support in use`
warning because the image has 4 KiB filesystem blocks; the 4 KiB guest does
not. This is a recorded upstream filesystem warning, not a failed mount or an
emulator diagnostic.

`rootfs.erofs` now supplies LTP at `/opt/ltp`, Node at `/opt/node/bin/node`,
`e2fsprogs` formatters (`mkfs.ext2`, `mkfs.ext3`, and `mkfs.ext4`),
`/usr/sbin/debugfs`, `/usr/sbin/e4crypt`, the full iproute2 `ip`, `bc` 1.07.1,
the 4.09 `quota`/`quotacheck` suite (`quota` is `/usr/bin/quota`), and
benchmark payloads under `/opt`. The manifests contain
2,428 verified LTP cases and 135 disabled cases, with no overlap; 88 cases
moved to disabled
because they require deliberately absent `CONFIG_HUGETLBFS` (79),
`CONFIG_TRANSPARENT_HUGEPAGE` (5), or `CONFIG_CAN` (4). This migration did not
run or claim the full LTP campaign. The credited final-image boundary is a
44-case focused gadgets-engine qualification: every live expectation was
accepted, `statx05` passed four assertions in both page profiles, and the
single-vCPU serialized `zram01` run returned zero with 12 TPASS and one
packaged-test TCONF in 167.6 seconds. The user-directed final Geekbench scoring
run remains skipped; the local Geekbench 6 command uses `--no-upload`, and no
new score is claimed.

All results were produced locally; no hosted CI workflow exists. Instruction
input materialization was off for the complete refactor acceptance and remains
an explicit, storage-intensive opt-in. The rootfs provisioning identity and
EROFS superblock contract live in `tools/rootfs/manifest.json`; ordinary guest
runs do not read or hash that manifest.

Use CMake/Ninja and CTest as the first-party source of truth. The `unit` suite
is host-only, contains no Python, and neither configures a kernel nor reads a
rootfs. Native emulator behavior tests are C++/assembly. The separately owned
`tooling` checks may use shell, CMake, or Python for build/source contracts;
they remain host-only and are selected alongside `unit` by the standard unit
workflow rather than becoming guest integration tests.

Every first-party registration has exactly one enforced primary suite label:
`unit`, `tooling`, or `integration`. The test manifest, registration helpers,
and owner paths enforce that suite-level taxonomy. Domain words in names and
source ownership remain useful, but secondary per-domain labels and a specific
per-domain subdirectory layout are optional organization, not acceptance gates.

Reproducibility here means pinned upstream Git repositories and full commits,
Git-generated patch bytes/IDs/subjects/order, verified prepared repositories,
configuration inputs, ABI identity, and a recorded build procedure. Git,
Kbuild, compiler, linker, and Xcode-generated timestamps, commit IDs, build
IDs, user/host fields, package layouts, and output hashes stay authentic and
may differ between runs; record them as observations rather than rewriting
metadata or requiring byte equality. Rootfs artifact authentication is limited
to explicit provisioning and is not repeated around guest execution.

Kernel paths are family/version scoped. Mainline commands in this document
select `kernel/mainline-6.18` explicitly; `kernel/common` supplies only the
shared ABI, 16 KiB geometry, and artifact contract. The separate Android 17
producer and accepted-release workflow are documented in
`android/aosp-17/README.md`; ACK inputs remain under `kernel/android-6.18`.
The current ACK source candidate includes a cooperative timer checkpoint for
IRQ-enabled Darwin spin waits. Its focused multi-vCPU gate is a four-vCPU
gadgets boot: require continuing guest timestamps beyond second 180 and no
`rcu: INFO` or `detected stalls` record. The 2026-08-21 validation reached
second 198; the preceding kernel reproducibly stalled at second 56–58.

## Green checkpoints and bisectable commits

Every completed feature or bug fix ends in a local commit only after
proportionate validation shows that the affected emulator state works. Start
with the narrow target or exact test, then run the single canonical workflow
that owns the changed subsystem. Do not repeat equivalent sanitizer, package,
or guest matrices merely to create more evidence. Page-size-sensitive CPU,
memory, ABI, kernel, and guest changes still require both 16 KiB and 4 KiB
coverage; GPU, loader, package, and guest workflows are required only when
those paths change.

A green checkpoint has no known failure in the affected behavior and includes
the tests, documentation, manifests, ABI identities, and generated copies that
the implementation requires. Each commit must contain one coherent change
and pass its applicable checks independently so `git bisect` can identify the
first regression. Do not commit an incomplete implementation, a temporary
diagnostic, or a knowingly broken emulator state. If an external input,
credential, host permission, or unavailable campaign prevents required
validation, leave the change uncommitted and report the missing prerequisite.

This policy requires verified local commits; it does not authorize pushing,
rebasing, amending, force-pushing, or otherwise rewriting shared history.

```sh
cmake --preset macos-unit-16k
cmake --build --preset unit-test-binaries
ctest --preset unit

cmake --preset macos-unit-4k
cmake --build --preset unit-test-binaries-4k
ctest --preset unit-4k

# Equivalent authoritative configure/build/unit/tooling workflows.
cmake --workflow --preset check-unit
cmake --workflow --preset check-unit-4k

# Standalone exact instruction-evidence pipelines. Each selection orders the
# CMake reset, all host producers, and the Python validator.
ctest --preset instruction-evidence
ctest --preset instruction-evidence-4k
```

## Kernarium iOS application prototype

`Kernarium/Kernarium.xcodeproj` is an arm64 iOS device and Simulator
application that embeds separate versioned runtime frameworks for the
maintained 4 KiB and 16 KiB UML kernels. It defaults to 16 KiB, loads exactly
one saved page profile for the process lifetime, and boots the selected
`.erofs` image from Documents with 4 GiB on Simulator and 2 GiB on physical
devices by default. Both default to all processors reported by
`NSProcessInfo`. The settings sheet persists 1 through that processor count,
512 MiB through 4 GiB of RAM in 64 MiB steps, a 1--32 GiB overlay size, a Documents rootfs filename,
the 4 KiB or 16 KiB kernel page profile, and Retina mode; changes are
snapshotted on the next app launch. Retina mode is off by default.
SwiftUI lays the `ContentView` Metal view out inside the safe area. Point mode
maps one host point to one Metal/guest pixel and advertises 160 DPI through
Virtio-GPU EDID. Retina mode multiplies the same dimensions by
`UIScreen.nativeScale` and advertises `160 * nativeScale`. UIKit scales touch
points into the selected pixel range. Point mode requests VGA8x16 fbcon text;
Retina densities request TER16x32. Trailing-center 32-by-32-point settings,
display-mode, and keyboard buttons remain directly available. The display
button sits between settings and keyboard and switches from the default
framebuffer surface to a SwiftTerm serial terminal and back. Both surfaces stay
mounted, so the terminal accumulates boot output while hidden and switching
does not reset Virtio-GPU. A fourth button below them expands
a row of Power, Volume Up, Volume Down, Escape, Tab, and Home actions to the
left; each action sends a press/release pair through the existing Virtio
keyboard device.
Touch and text events enter the guest through separate real Virtio-input
devices, and the app disables the idle timer only while the guest is running.
One real Virtio Sound card is also attached on both Device and Simulator. It
offers one 48 kHz, stereo, signed-16-bit playback stream through the upstream
`snd_virtio` driver and sends consumed guest periods to `AVAudioEngine`.
Kernarium configures the iOS playback audio session and recovers the engine
after route changes, configuration changes, and interruptions. Capture, mixer
controls, and synthesized legacy UML sound remain intentionally absent.
The Darwin kernel scheduler uses one persistent host carrier per vCPU on
macOS and both iOS targets, names them `uml-vcpu-N-carrier`, and moves Linux
task contexts between those carriers without creating a host thread per guest
task. The shipped 100 Hz plus `NO_HZ_IDLE` policy suppresses ticks on idle
carriers.
The early Darwin calibration timer uses process-directed `ITIMER_REAL`.
Kernarium blocks `SIGALRM` before application main and again around the
managed run, leaving the launcher on its existing Swift worker. Only the vCPU0
carrier explicitly unmasks the signal before cooperative deadlines take over.
IRQ-enabled arm64 spin checkpoints service pending Darwin IPIs, so synchronous
cross-CPU calls progress even when two fixed carriers are waiting on each
other; IRQ-disabled regions continue to defer the work.
Both page profiles provide a 128 KiB Linux task stack (order 3 at 16 KiB and
order 5 at 4 KiB), so high-task-count guests retain sleeping contexts
without consuming a contiguous 512 KiB allocation apiece.

The standalone macOS app has a separate larger default: when the command line
omits either setting, it supplies `mem=8G` and `ncpus=10` independently. Its
shared scalable layout places an 8 GiB direct map plus 1 GiB auxiliary window
at 96 TiB. Explicit kernel command-line values continue to override these
defaults.

The common Apple-GPU and iOS network prefixes are separate from the macOS
non-graphics dependency prefix. Their repositories, full commits, ordered Git
mail patches, platform contracts, preparation, builders, and verifiers are all
owned by [`../dependencies/`](../dependencies/README.md). Prepare the sources
once, build the macOS graphics slice first, then both iOS slices before
packaging the Xcode app:

```sh
dependencies/prepare.sh --product apple-gpu
dependencies/build-apple-gpu.sh --platform macos
dependencies/build-apple-gpu.sh --platform ios-simulator
dependencies/build-apple-gpu.sh --platform ios

dependencies/prepare.sh --product ios-network
dependencies/build-ios-network.sh
dependencies/build-ios-network.sh --platform ios

# Read-only checks once the prefix exists.
dependencies/build-apple-gpu.sh --platform macos --verify-only
dependencies/build-apple-gpu.sh --platform ios-simulator --verify-only
dependencies/build-apple-gpu.sh --platform ios --verify-only
dependencies/build-ios-network.sh --verify-only
dependencies/build-ios-network.sh --platform ios --verify-only

# Build the two canonical page-profile inputs shared by macOS and iOS.
kernel/mainline-6.18/build/build-kernel.sh \
  --build /Volumes/<case-sensitive-volume>/build-16k \
  --page-size 16k
kernel/mainline-6.18/build/build-kernel.sh \
  --build /Volumes/<case-sensitive-volume>/build-4k \
  --page-size 4k
python3 -B kernel/mainline-6.18/build/publish-artifact.py \
  --source /Volumes/<case-sensitive-volume>/linux \
  --build /Volumes/<case-sensitive-volume>/build-16k \
  --page-size 16k \
  --output-root "$PWD/.build/kernel/artifacts" \
  --release-id reviewed-release-id
python3 -B kernel/mainline-6.18/build/publish-artifact.py \
  --source /Volumes/<case-sensitive-volume>/linux \
  --build /Volumes/<case-sensitive-volume>/build-4k \
  --page-size 4k \
  --output-root "$PWD/.build/kernel/artifacts" \
  --release-id reviewed-release-id

cmake --preset kernarium-ios-simulator
cmake --build --preset kernarium-ios-simulator
cmake --preset kernarium-ios-simulator-4k
cmake --build --preset kernarium-ios-simulator-4k

cmake --preset kernarium-ios-device
cmake --build --preset kernarium-ios-device
cmake --preset kernarium-ios-device-4k
cmake --build --preset kernarium-ios-device-4k
Kernarium/scripts/package-ios-frameworks.sh
```

The Apple-GPU contracts authenticate one source and topic matrix across all
three platforms. Its UTM WebKit topics retain the Simulator ES 3 minimum
varying correction, disable CGL/EAGL/native-GL backends, load only the flat
authenticated EGL/GLES frameworks, export the platform-display bootstrap, and
enable the arm64 macOS Metal baseline. They also implement GLSL boolean `mix`
with Metal component selection so selected subnormal float bit-carriers are
not changed by arithmetic interpolation. The UTM libepoxy topic resolves
GL/EGL only through those framework install names; UTM virglrenderer retains
its ES 3 multisampled-renderbuffer and native Metal resource path. Wholesale
replacement with ambient or latest desktop branches is not equivalent: every
consumer uses the central full commits, ordered generated patches, common
builder, and verified format-3 contract. Chromium is launched through its
ordinary ANGLE/GLES selection; the app does not preload, replace, or otherwise
interfere with Chromium libraries.

The network prefixes reuse the authenticated PCRE2, libffi, proxy-libintl,
GLib, GVDB, and libslirp sources prepared by `dependencies/prepare.sh`. The
iOS-specific GLib patch removes unavailable process-management probes; the
guest still receives the normal SLiRP callback provider used by the emulator.
`package-ios-frameworks.sh` combines both versioned runtimes plus EGL and
GLESv2 device and Simulator slices below `.build/kernarium-xcframeworks`, which
is the only framework location referenced by the Xcode project.

The default macOS integration target and iOS runtime consume the accepted
16 KiB mainline bundle below `.build/kernel/artifacts`; the corresponding
4 KiB compatibility products consume the accepted 4 KiB bundle. macOS embeds
the manifest-derived ELF directly. Kernarium packages the same bytes into
`KernariumRuntime_<release>_16K` and `KernariumRuntime_<release>_4K`, embeds
both without a launch-time dependency, and loads only the saved profile.
Override a whole artifact directory only when deliberately testing another
maintained publication of the matching page profile. With one Simulator
already booted, the following command builds both runtimes and the Xcode app,
installs it, copies the EROFS image into the app's Documents directory, and only
then launches the app with the guest console on the invoking terminal's stdout:

```sh
Kernarium/scripts/run-ios-simulator.sh \
  --rootfs "$PWD/rootfs.erofs" \
  --artifact-16k "$PWD/.build/kernel/artifacts/accepted-16k-directory" \
  --artifact-4k "$PWD/.build/kernel/artifacts/accepted-4k-directory"
```

The serial console remains on that stdout relay and is also copied, in order,
to the app's SwiftTerm surface. Typing in SwiftTerm writes to the same `ttyS0`
socket; this does not add a Virtio-console backend or replace `tty0` fbcon.

The app exposes Documents through both `UIFileSharingEnabled` and
`LSSupportsOpeningDocumentsInPlace`. Its target records the Extended Virtual
Addressing capability in the Xcode project and the
`com.apple.developer.kernel.extended-virtual-addressing` entitlement in
`Kernarium.entitlements`. A checked-in shared scheme keeps Xcode's iOS and
Simulator destinations attached to the application target. The entitlement is
a device signing contract; Xcode carries it in the Simulator's simulated
entitlement input even though the local Simulator code-signature entitlement
dictionary is empty.

iOS cannot execute an anonymously copied ELF text segment under an ordinary
non-debugger signature. For both device and Simulator, each versioned runtime
embeds its complete page-profile kernel ELF in a 16 KiB-aligned Mach-O
`__TEXT,__uml_elf` section. Xcode's normal framework code signature covers
those bytes. The app's small C loader validates the framework name, Linux
release, API version, and guest page size before publishing its immutable
function table; a second profile cannot be loaded into the same process. The
loader gives the software-MMU guest a 128 TiB sparse task
range, derives a native hole ending at 62 GiB from guest RAM plus a page-rounded
one-eighth auxiliary allowance with a 32 MiB floor, biases the ET_DYN image
into that window, aliases each
immutable executable ELF page from the signed framework mapping with
`vm_remap`, and copies only non-executable data/BSS into anonymous pages. It
rejects writable executable segments, carrier mismatches, and relocations that
target signed text. The Darwin linker aligns executable boundaries to the
16 KiB host-page granularity even for the 4 KiB guest profile, and packaging
rejects an image whose signed executable host pages overlap later loader data.
This path performs no RW-to-RX transition, uses no JIT entitlement, and does
not depend on LLDB. macOS retains its existing unsigned loader and uses the
same 128 TiB guest range, RAM-scaled window derivation, relocation, and
exclusion-hole handling, while locating its active native window at 96 TiB to
avoid low-host-VA collisions.

The guest follows the same mount-and-desktop-init flow as
`scripts/boot_kde_vnc.py`, but the iOS writable state is one disposable disk.
The app deletes any previous temporary image, creates a sparse image at the
configured size (8 GiB by default), and passes it as `/dev/ubdb`. The init
shell formats it as ext4, binds the EROFS root as the OverlayFS lower layer,
uses directories on ext4 for upper/work, mounts devtmpfs in the new root, and
pivots. Separate Docker and containerd directories are then bind-mounted from
the same ext4 filesystem, keeping their contents separate while sharing its
free space. The shell applies the image-appropriate disposable desktop setup
and execs the rootfs systemd or OpenRC init as PID 1. KDE uses the image's
display manager and `uml` account. Phosh keeps the image's stock Phrog/greetd
graph, normal VT
session, and image-owned getty and login policy. The current test image exposes
the `debian` account with password `1234` through `serial-getty@ttyS0`; the
launcher neither rewrites those credentials nor changes any getty. It
whiteouts only the image's mutually exclusive development
`graphical.target.wants/phosh.service` enablement so graphical target uses the
stock `display-manager.service` alias, greetd, and Phrog. Empty disposable
copies of `/etc/phrog/phoc.ini` and `/etc/phosh/phoc.ini` make both the greeter
and logged-in Phosh session select the Virtio-GPU EDID mode instead of the
image's fixed 720x1440 mode. The underlying units and immutable EROFS image are
unchanged, and no wait helper or greetd ordering drop-in is present. Kernarium
adds `plymouth.enable=0` to its systemd kernel arguments: a same-build iOS
Simulator A/B showed the image otherwise blocking in
`plymouth-start.service` before greetd, while the disabled arm reached stock
greetd/Phrog. In that disabled, checked-allocation run, the app's Virtio
keyboard delivered the image-owned password and the logged-in Phosh app screen
filled the 1320x2580 guest surface about 45 seconds after authentication. The
direct Geekbench shell and macOS launchers are unchanged.

Selecting `pmos-sxmo.erofs` from Documents instead preserves postmarketOS's
UID-10000 account, tinydm default session, and `sxmo_swayinit.sh` entry point.
OpenRC classifies UML as a container and normally suppresses its udev coldplug
services, so Kernarium removes that keyword only from disposable overlay
copies, supplies minimal no-swap deviceinfo, adds Virtio-input classification,
and grants the image's `video` and `input` groups device-node access after udev
settles. It then starts the image's `/sbin/init`; it does not synthesize a user,
session, or runlevel. The common static kernel includes the mixed-family
nftables table, conntrack, and reject expressions used by postmarketOS's
default firewall.

The kernel builds zswap in, enables it by default, and selects zstd. Because
zswap is only a compressed cache in front of a backing swap device or file,
Kernarium's no-swap launch allocates no zswap pool. The prototype does not
synthesize a desktop user, modify the EROFS image, or start VNC. Standard
VT/fbcon renders early boot through Virtio-GPU fbdev emulation; the DRM log
client and conflicting UML stdio console are disabled. The guest exposes the
normal 64 VT nodes, four UML serial lines, dynamic Unix98 PTYs, and no legacy
BSD PTY bank. Kernarium attaches no Virtio-console device, so `ttyS0` is the
serial fallback connected to both the app's stdout relay and SwiftTerm terminal
while `tty0` remains on fbcon. SwiftTerm delegate input is written back through
the same serial socket.
`CONFIG_PRINTK_TIME` gives the kernel records on both consoles guest-monotonic
prefixes. In ordinary UML execution, `sched_clock()` derives them from a
boot-relative host `CLOCK_MONOTONIC` nanosecond counter rather than jiffies, so
records such as `[    0.004448]` retain sub-10-ms precision even though the
kernel tick remains 100 Hz. Time-travel modes retain their deterministic
simulated-jiffies clock.

Aquarium is an opt-in macOS/Simulator-only performance canary. The common
probe requires WebGL 2 through guest Virgl, fixes 500 fish, waits 15 seconds,
then records thirty one-second FPS samples. A fixed native macOS run comes
first:

```sh
python3 -B tests/integration/gpu/run_chromium_webgl.py \
  --display native --page-profile 16k \
  --emulator .build/presets/macos-integration/bin/uml-darwin \
  --engine gadgets --memory 1536M --cpus 4 \
  --width 440 --height 860 --dpi 160
```

Pass that run's `run.json` to the Simulator adapter so it reparses rather than
trusts the predecessor evidence:

```sh
python3 -B tests/integration/gpu/run_ios_simulator_chromium_webgl.py \
  --page-profile 16k --device booted \
  --macos-run .build/chromium-webgl/<run>/run.json
```

Use the corresponding `macos-integration-4k` emulator and `--page-profile 4k`
for the compatibility profile. Each adapter preserves raw FPS samples,
derived frame-time statistics, shader compile/link calls and synchronous CPU
time, raw host-process and synchronized guest CPU/memory snapshots, final
native-display counters, exact configuration identity, and clean shutdown.
Any CPU-fallback frame, missing sample/counter, dirty DRM teardown, nonzero
runtime result, software renderer, or mismatched macOS predecessor fails the
run. `Kernarium/scripts/run-ios-simulator.sh --aquarium` uses the same generated
probe and fixed Simulator configuration as a manual bring-up convenience; the
evidence adapter above owns recorded runs. The dated 2 GiB Simulator evidence
reports `WebGL 2.0 (OpenGL ES 3.0 Chromium)` and
`ANGLE (Mesa, virgl, OpenGL ES 3.0)`. Aquarium is forbidden on the physical
iPad acceptance guest; do not add swap or another memory workaround solely
for that campaign.

The repeated physical checkpoint used a standard-signed Release build on an
iPad mini 6 with `phosh.erofs`, `mem=1536M`, and all six reported CPUs under
the former `96 * nativeScale` density rule. Five consecutive cold launches
reached DHCP, mounted the read-only EROFS root and fresh ext4 overlay, started
stock greetd, reached `graphical.target`, opened the `_greetd` session, and
reported direct virgl/ANGLE/Metal presentation at 2266x1390 and 192 DPI. Every
boot brought all CPUs online and reported
1,520,944 KiB available from 1,572,864 KiB during kernel initialization. The
captured runs contain timestamped printk records and no Darwin panic, POSIX
signal failure, allocator-corruption, or guest OOM marker; the device produced
no new Kernarium crash report. Runs were stopped by the host after stable
presentation, so orderly guest shutdown is not claimed. RAM was not reduced
below 1.5 GiB because no OOM occurred. The older 1 GiB KDE device and Simulator
boots remain dated predecessor evidence. A focused standard-signed Debug check
of the now-optional `160 * nativeScale` Retina rule booted at 2266x1390/320 DPI
on that iPad and at 1320x2580/480 DPI on the iPhone 17 Pro Max Simulator. Both runs
initialized Virtio-GPU EDID/fbcon and reached direct Metal desktop
presentation. A later iPhone 17 Pro Max Simulator check booted the new default
point mode at 440x860/160 DPI with a 55x53 VGA8x16 console, then Retina mode at
1320x2580/480 DPI with an 82x80 TER16x32 console; both reached stock Phrog and
the host serial log contained non-tick-aligned timestamps. These focused
checks do not replace the five-run physical
reliability evidence. Archived pre-migration macOS evidence boots the
unmodified Phosh service set, completes its
GStreamer plugin scan without poisoning the virgl context, captures the sparse
Phosh lock screen through VNC, and enters the direct
CGL/IOSurface/Metal presentation path in native mode. A separate stock-Phrog
1290x2796/288-DPI validation completed two cold boots with no greetd restart,
accepted EDID density, switched fbcon to Virtio-GPU, and observed exactly 64
VTs, four serial lines, and zero legacy PTYs. A matched 1320x2580/288-DPI
native-Metal control then isolated the authenticated-session sizing bug. With
the image's `/etc/phosh/phoc.ini`, DRM reported a Phoc-owned 720x1440
framebuffer, plane, and CRTC at origin. Replacing both the greeter and session
configs with empty overlay files produced a Phoc-owned 1320x2580 framebuffer,
plane, and CRTC at origin under otherwise identical conditions. The validation
started the image's development Phosh unit directly to isolate the post-login
path; it did not replace the production greetd login flow.

Prepare full integration explicitly. Linux replay and Kbuild remain standalone
producer work on a case-sensitive filesystem. After both builds pass, publish
their compact artifacts below `.build/kernel/artifacts`; Apple configure and
build use only those local bundles. Do not create or attach a repository-local
sparsebundle for the kernel source workflow, and do not substitute a transient
case-sensitive mount under `/tmp`.

Source preparation, dependency-prefix construction, and Linux replay are
explicit mutating setup steps. Kernel preparation fetches the exact commit
pinned on the upstream `linux-6.18.y` stable branch, verifies its tree, then
replays the Git-generated project topics. Verification is read-only and is what
ordinary builds should use once those inputs exist:

```sh
dependencies/prepare.sh
dependencies/build.sh
cp kernel/mainline-6.18/source-location.conf.example \
  kernel/mainline-6.18/source-location.conf
# Set the absolute case-sensitive UML_KERNEL_SOURCE_DIR in that ignored file.
kernel/mainline-6.18/scripts/prepare-source.sh
# Recreating that configured checkout can use the pinned offline Git cache.
kernel/mainline-6.18/scripts/prepare-source.sh --no-fetch

dependencies/prepare.sh --verify
dependencies/verify-prefix.sh \
  --prefix .build/dependencies/macos-arm64 \
  --manifest dependencies/manifest.tsv \
  --deployment-target 14.0
kernel/mainline-6.18/scripts/prepare-source.sh --verify
kernel/mainline-6.18/scripts/verify-replay.sh \
  --work-root /Volumes/<case-sensitive-volume> --skip-build
python3 -B kernel/mainline-6.18/build/publish-artifact.py \
  --source /Volumes/<case-sensitive-volume>/linux \
  --build /Volumes/<case-sensitive-volume>/build-16k \
  --page-size 16k \
  --output-root "$PWD/.build/kernel/artifacts" \
  --release-id reviewed-release-id
cmake --preset macos-integration
cmake --build --preset macos-integration
ctest --preset integration-fast
```

Kernel compilation uses every online host processor by default. Set
`UML_KERNEL_JOBS` or pass `--jobs N` only to impose an explicit lower limit.

The ignored `kernel/mainline-6.18/source-location.conf` is the sole authority
for standalone source preparation and Kbuild. Apple CMake instead derives its
default bundle from the checked page-specific acceptance descriptor. To test a
different reviewed local publication, override the complete artifact directory:

```sh
cmake --preset macos-integration \
  -DUML_MAINLINE_618_KERNEL_ARTIFACT_DIR=/local/artifacts/mainline-6.18-16k-release
cmake --preset macos-integration-4k \
  -DUML_MAINLINE_618_KERNEL_ARTIFACT_DIR=/local/artifacts/mainline-6.18-4k-release
```

`kernel/mainline-6.18/build/build-kernel.sh` fails before toolchain selection
when its source configuration is absent or invalid. Apple configuration does
not read that configuration; it fails when the selected accepted artifact is
missing, stale, wrong-family, wrong-page, or ABI-incompatible.

The dedicated Android macOS product consumes only an already-installed accepted
release and has no 4 KiB variant:

```sh
cmake --preset macos-android17
cmake --build --preset macos-android17
ctest --test-dir .build/presets/macos-android17 \
  -R '^apple-shared-kernel-image-policy$' --output-on-failure
```

Configuration repeats the installed-release verification and imports its ACK
ELF, initramfs, command line, and expanded static GPT through one read-only
target. The executable's `--help`, `--version`, missing-path rejection, and
Mach-O carrier inspection are host-side gates. An Android boot is a separate
guest acceptance gate and is not implied by a successful product build.

`dependencies/prepare.sh --verify` authenticates the already prepared source
trees; it does not construct or verify the compiled dependency prefix.
`dependencies/build.sh` builds that private arm64 static prefix without network
access and publishes it transactionally. CMake configuration independently
checks current manifest/patch validation, required payload presence, real
archive format, deployment
target, arm64 architecture, and static linkage before integration targets can
consume it. The verifier checks compatibility, not product identity or byte
equality.

Host build tools and the Apple SDK may come from the configured toolchain or
PATH. Production third-party library headers and binaries may come only from
the verified private prefix, never from system or package-manager locations.

`kernel/mainline-6.18/scripts/verify-replay.sh` creates and removes a fresh
checkout, so its work root must have several GiB free. Omit `--skip-build` to
rebuild both page-size kernels inside that replay. The line-scoped Git cache is
`.build/kernel-cache/mainline-6.18` unless explicitly overridden.

`integration-fast` runs host fixtures plus short real guest boots: the
production overlay/systemd path, interpreter, gadgets, JIT, and deterministic
SLiRP. The interpreter smoke intentionally stops after shell arithmetic and
CPU/HWCAP checks. All three engine smokes compare `AT_HWCAP` and `AT_HWCAP2`
with the complete host-gated virtual-CPU contract; JSCVT remains optional after
its additional runtime flag-behavior probe. `/proc/cpuinfo` is checked
separately against the feature subset currently published by the Darwin kernel
port rather than being treated as the auxiliary-vector authority.

Keep the interpreter out of every heavy guest campaign, whether registered or
run by hand. It is far slower than the translating engines — enough that a
campaign finishing in minutes under gadgets or JIT may not finish in a
practical time under it — and it buys no semantic coverage, because the unit
suites already execute interpreter, gadgets, and JIT in order against one
oracle for each case. Full LTP, Docker, KDE native/VNC, WebGL, SMP and
shared-MM pressure, and every Geekbench measurement are therefore gadgets and
JIT only, and `uml_add_ltp_engine_campaign` is registered for those two
engines alone. Node/V8/Wasm coverage is likewise restricted to gadget and JIT
runs; do not add heavyweight Node.js, Go, or similar workloads to interpreter
acceptance.

When an interpreter-specific question does arise, answer it with a focused
probe instead of a campaign. A freestanding payload under
`tools/guest-shell.py --engine interpreter` boots in seconds and isolates the
instruction under test; that is how the missing `MRS <Xt>, DIT` encoding was
confirmed on all three engines without booting Docker under the interpreter.

`integration-full` (or `integration-full-4k` for the matching
page profile) additionally selects the 10-vCPU SMP and shared-MM pressure
tests. Shell-oriented workloads boot an explicit writable tmpfs
OverlayFS shell; that is intentional and avoids relying on a systemd getty for
PTY interaction, while `guest-overlay-systemd-smoke` validates the production
systemd path separately.  The shared-MM workload is a CMake-built freestanding
arm64/Linux executable, so the EROFS image does not need a guest compiler.

The bare CLI defaults to `init=/bin/sh`. Systemd-oriented launchers host-feed
their OverlayFS bootstrap to that temporary PID 1 shell, mount the EROFS lower
and tmpfs upper/work directories, then enter the overlay and start systemd.
They do not depend on a custom bootstrap executable inside the image. The
image owns its enabled VT and serial getties, account credentials, and login
policy; launchers must preserve them instead of creating a replacement symlink,
drop-in, autologin override, or credential rewrite in the overlay.
Before starting systemd, the bootstrap mounts tmpfs at
`/var/lib/docker` and `/var/lib/containerd` so Docker storage and containerd
state are not nested on the writable root OverlayFS. The
`guest-overlay-systemd-smoke` target builds a freestanding arm64/Linux
hello-world executable from repository source, packages it as a local rootfs
archive, imports that archive into the disposable Docker state, and runs
the resulting local image with `--pull=never --network none`. The smoke has no
registry or external-network dependency. KDE/VNC and WebGL launchers reuse the
image's non-root `uml` account and must not create `plasma` or another desktop
account in the overlay.

Guest tests map the shared image directly with `ubd0c=` and boot
`root=/dev/ubda rootfstype=erofs`. EROFS supplies the read-only lower-filesystem
contract; do not add per-run hashing, sidecar locking, host-mode, `ubd0r`, or
`ro` checks. The `c` mapping disables UML's host-file lock but does not make raw
`/dev/ubda` safe scratch space, so no test may target the root block device for
destructive block-I/O probes. Guest tests obtain dynamic VNC ports through
LibVNCServer's 5900-and-up auto-port search.

Guest runners start the emulator **attached**, in the runner's own process
group, and signal that process directly. Whatever kills a runner's group -- a
shell job, a CTest timeout, Ctrl-C -- therefore takes its guest with it. Do not
reintroduce `start_new_session` or `killpg` here: a runner that is killed
outright runs no cleanup code, and a guest that survives one boots to idle and
still burns one to two host cores indefinitely, so a later benchmark silently
measures an oversubscribed host. `guest-harness-attached-session` pins both
halves of that property.
The VNC smoke is mandatory when registered: it does not convert a headless
graphics failure into a skip.

The KDE launcher opens a native Metal window by default. Select VNC explicitly
when a remote client or an automated capture runner is required:

```sh
python3 scripts/boot_kde_vnc.py
python3 scripts/boot_kde_vnc.py --display vnc --port 5900
python3 scripts/boot_kde_vnc.py --iso ~/Downloads/installer.iso
python3 scripts/boot_kde_vnc.py --display vnc \
  --serial-socket .build/uml-desktop-serial.sock
```

`--iso PATH` attaches the image as the read-only secondary UML block device
`/dev/ubdb` and mounts its ISO9660 filesystem read-only at `/mnt/cdrom` before
the display manager starts. The boot log records the resulting source,
filesystem type, and mount options. The path is expanded and resolved on the
host; the base EROFS image and disposable root overlay are unchanged.

`--serial-socket PATH` keeps the normal console log/stdout copy and also
publishes the bidirectional `ttyS0` stream through one reconnectable local
Unix-domain client. The launcher refuses to replace an existing path and
removes only the socket inode it created. Attach from another terminal with
`nc -U .build/uml-desktop-serial.sock`; a new client receives the recent
console tail, so a prompt printed before attachment remains visible. The same
option is available directly on `run_gpu_guest.py` for focused diagnostics.

The launcher always passes the host-only `--audio` and
`--gpu-display=native|vnc` options, so the standard KDE desktop has the Apple
Virtio Sound playback device as well as the selected display backend.
The current image ships `pipewire` and `pipewire-pulse` without WirePlumber or
`pipewire-media-session`; without a session manager, Plasma exposes only its
fallback dummy output even though ALSA has registered the card. Resolve that
guest-userspace deficiency with a whole-image update that installs a supported
session-manager package such as WirePlumber. The KDE and Kernarium overlays do
not synthesize a PipeWire sink or routing policy.

`--bind`, `--port`, and `--password` configure only VNC and therefore require
`--display vnc`. The bare `uml-darwin --gpu` host option retains VNC
as its compatibility default, while the KDE launcher deliberately selects
native mode. Native keyboard, pointer, and wheel events use the same inherited
Virtio-input datagram boundary as VNC. The native Virgl path receives the
`MTLTexture` exported by the common ANGLE/Metal renderer and samples it through
the shared Metal presentation owner; there is no CGL/IOSurface bridge,
`glReadPixels`, CPU framebuffer copy, or extra FIFO frame queue on that path.
CPU readback remains a counted diagnostic/VNC fallback only for unsupported
scanout resources. A scanout the display accepts but cannot present yet is
dropped, because answering host backpressure with readback on the guest flush
path would couple the two timelines exactly where they must stay independent.
Native qualification requires a zero fallback rate.

The standalone macOS executable attaches the same Virtio Sound device only
when the host-only `--audio` option is present; the standard KDE launcher
supplies it automatically. `tools/guest-shell.py --audio` provides that
attachment for focused guest commands; inspect `/proc/asound/cards` or
`/dev/snd/pcmC0D0p` after boot. The current image does not contain the `aplay`
utility. The
`virtio-sound-protocol` unit test covers discovery, format negotiation,
repeatable SET_PARAMS/PREPARE and RELEASE-to-PREPARE transitions, malformed
requests, and deferred TX completion. The shared vhost-user
transport test also exercises a four-queue device whose descriptor is not
published to the used ring until an asynchronous completion arrives. A guest
desktop still needs its normal PipeWire or ALSA session policy to route
application audio; the host device and launchers do not replace guest
userspace.

The macOS executable also accepts
`--virtio-console-socket=/absolute/path`. This is a host-only attachment for a
standard single-port Virtio console (device ID 3), not a kernel command-line
argument. The service refuses to replace an existing path, publishes its socket
with mode 0600, removes only the inode it created, retains a bounded recent
output tail for the next attachment, and supports reconnecting after a client
disconnects. Its output path is deliberately nonblocking; a client that stops
reading loses console bytes rather than stopping a guest vCPU. The focused
`virtio-console-protocol` unit test covers both queue directions, socket path
ownership, tail delivery, reconnect, and in-process service teardown.

`--virtio-serial-socket=/absolute/path` selects the separate multiport mode of
the same standard device and is mutually exclusive with the single-port
option. It exposes the non-console port `org.uml.host-serial.0` as
`/dev/virtio-ports/org.uml.host-serial.0`, implements Linux's control-queue
handshake, and uses the same reconnectable mode-0600 Unix stream backend. The
port is raw rather than a tty: clients must not run `stty` on it. Because it is
not `/dev/hvc0`, attaching it neither consumes nor disables the image-owned
serial getty. The focused protocol test covers the device-ready, add, ready,
name, open, data, reset, validation, and asynchronous control-wakeup paths.

`UML_GPU_FRAME_STATS=<seconds>` turns on native-display frame accounting on
macOS and in Kernarium; unset, zero, or unparseable keeps it off. Each interval
writes one bounded line to stderr, and macOS also mirrors it into the window
title:

```text
Native display frames: guest 6.0/s presented 6.0/s dropped 0.0/s (0.0%)
cpu-fallback 0.0/s | totals guest 1782 presented 1875 dropped 1 cpu-fallback 0
```

`guest` counts scanout flushes offered to the display and `presented` counts
frames the host finished drawing, so the two are separate timelines rather than
one rate; `presented` also includes CPU-framebuffer frames such as the boot
console, which is why it can exceed `guest`. `dropped` counts guest frames
retired without ever being presented, and `cpu-fallback` counts flushes that
took the readback path. Both were measured at zero on macOS and effectively
zero on the Simulator, so a nonzero `dropped` or `cpu-fallback` rate is a
finding, not routine.

External network, KDE native/VNC, and rendered WebGL workflows are opt-in
because they need network/GUI permission:

```sh
cmake --preset macos-integration \
  -DUML_ENABLE_EXTERNAL_NETWORK_TESTS=ON
ctest --preset integration-full
```

The repository-owned VK-GL-CTS runner uses the same KDE graphical session,
the image's `uml` account, real Xwayland authorization, and the immutable CTS
and resource tree. It accepts only typed leaf, exact-case/fraction, API,
context, page-profile, engine, seed, and timeout inputs. The runner verifies
both DRM nodes and Virgl before execution, rejects llvmpipe, softpipe, swrast,
Zink, Xvfb, the retired CGL host marker, incomplete QPA unions, and corrupt or
unsafe framed artifacts, and always requests guest poweroff. Long shards use
JIT or gadgets, never the interpreter:

```sh
python3 tests/integration/gpu/run_vk_gl_cts.py \
  --api gles3 --leaf gles3 \
  --case KHR-GLES3.shaders.declarations.declarations.missing_variable_name_vertex

python3 tests/integration/gpu/run_vk_gl_cts.py \
  --api gles3 --leaf gles3 --fraction 0,16

python3 -B tests/integration/gpu/run_vk_gl_cts.py --inventory
```

The default remains the authenticated EROFS image. The one user-authorized
GL/GLES derivative is an ignored local 8 GiB ext4 artifact whose complete
identity is checked in at
`tests/integration/gpu/vk_gl_cts_derivative_rootfs.json`. Select it explicitly:

```sh
python3 -B tests/integration/gpu/run_vk_gl_cts.py \
  --rootfs .build/rootfs/rootfs-gl-gles.ext4 \
  --rootfs-type ext4 \
  --rootfs-receipt tests/integration/gpu/vk_gl_cts_derivative_rootfs.json \
  --campaign gles3-identity-sentinel-v1 \
  --page-profile 16k --engine gadgets --display native
```

The driver requires the ext4 host file to be a regular mode-0444 file with the
exact checked size and SHA-256, verifies its filesystem UUID and in-guest
receipt, attaches it as `ubd0cr`, mounts it read-only, and builds disposable
OverlayFS state. It rejects a writable file, an unmatched receipt, any changed
exclusion list, or an ext4 image without a receipt. The maintenance epoch for
this artifact is closed: do not boot it writable or update it in place. The
original `rootfs.erofs` and its provisioning manifest remain unchanged.

`--inventory` is a fail-closed display-backed GLES baseline capture. It invokes
the exact installed multi-package `glcts` binary twice, filtering separately
to `KHR-GLES2.*` and `KHR-GLES3.*`. It requires exactly those two nonempty
package exports, rejects any row from another module, concatenates them in
fixed GLES 2 then GLES 3 order inside the disposable guest, and egresses a
small strict report containing scope, completion state, raw caselist SHA-256,
executable-case count, and modules. Both CTS commands must exit zero before the
host accepts the frozen 6,972-case SHA-256
`3ec390ed2d36f0fd8e32a02718e19c5750cf9381eab002aac9b71f848ebc6162`.
The installed official must-pass leaves remain the execution authority and
contain 473 GLES 2 plus 6,498 GLES 3 cases.

Enumerate the exact conformant EGL configurations in VNC before an official
matrix. Then run the resumable matrix orchestrator against the matching
inventory evidence. The orchestrator has no switches for the acceptance
execution shape: every child is VNC, JIT, ten vCPUs, 8 GiB, and the sealed
ext4 lower with disposable OverlayFS state. `--jobs` is bounded to two such
ten-vCPU guests; `--fraction-count` changes only deterministic shard size, not
the required config-by-case union.

```sh
python3 -B tests/integration/gpu/run_vk_gl_cts.py \
  --config-inventory gles2 --page-profile 16k --display vnc \
  --rootfs .build/rootfs/rootfs-gl-gles.ext4 \
  --rootfs-type ext4 \
  --rootfs-receipt tests/integration/gpu/vk_gl_cts_derivative_rootfs.json

python3 -B tests/integration/gpu/run_vk_gl_cts_matrix.py \
  --api gles2 \
  --config-inventory-run .build/vk-gl-cts/CONFIG_RUN/run.json \
  --campaign-dir .build/vk-gl-cts/matrix/gles2-16k \
  --fraction-count 1 --jobs 2 --page-profile 16k \
  --rootfs .build/rootfs/rootfs-gl-gles.ext4 \
  --rootfs-receipt tests/integration/gpu/vk_gl_cts_derivative_rootfs.json

python3 -B tests/integration/gpu/run_vk_gl_cts_matrix.py \
  --api gles2 \
  --config-inventory-run .build/vk-gl-cts/CONFIG_RUN/run.json \
  --campaign-dir .build/vk-gl-cts/matrix/gles2-16k \
  --fraction-count 1 --jobs 2 --page-profile 16k \
  --rootfs .build/rootfs/rootfs-gl-gles.ext4 \
  --rootfs-receipt tests/integration/gpu/vk_gl_cts_derivative_rootfs.json \
  --verify-only
```

Use the GLES 3 config-inventory evidence and `--api gles3` for that matrix.
The campaign specification freezes the Git and emulator identities, CTS XML,
runner oracle, config inventory, rootfs receipt, timeouts, and shard geometry.
Resuming skips an already verified shard but never retries or overwrites an
attempt. A failed, truncated, interrupted, duplicated, reordered, or
nonconformant attempt permanently fails that campaign directory; use a new
directory after an implementation change. Final aggregation reparses every
QPA and console, verifies all hashes and statuses, interleaves every fraction
back into the exact ordered leaf, checks the frozen leaf SHA-256 for every EGL
config, and requires the complete cartesian product before writing `passed`.
Every macOS child automatically attaches the named Virtio serial port above.
The guest sends bounded gzip/base64 frames and waits for an exact acknowledgement
after the begin record, every chunk, and the end record; the host retains the
canonical raw transcript as `artifact-serial.log` and rechecks its hash during
matrix aggregation. Do not redirect this protocol through `/dev/hvc0`, mask its
getty, or fall back to the unacknowledged journal/console stream.

Current 16 KiB macOS evidence is the clean detached pair
`.build/vk-gl-cts/matrix/gles2-16k-e741cbad-z32-truthful` and
`.build/vk-gl-cts/matrix/gles3-16k-e741cbad-z32-truthful`. Their matching
Release inventories contain config IDs 1-4, 13-16, 25-28, 37-40, and 49-52
with canonical SHA-256
`843544a7aef987426f74851a8802447a2428bde9ff9a5d893e1c2073ffbb2607`.
The GLES 2 campaign and separate `--verify-only` run accepted all 9,460
executions: 8,760 `Pass`, 700 released-policy `NotSupported`, and no disallowed
status across 20 immutable shards. The GLES 3 pair likewise accepted all
129,960 executions: 125,270 `Pass`, 4,690 `NotSupported`, and no disallowed
status. The earlier 25-config inventory falsely included normalized 32-bit
depth configurations backed by 24-bit depth; retain its matrix only as
diagnostic history and never resume that directory.

A native arm64 Debian 13 direct-virgl diagnostic using the exact installed CTS
payload reproduces status 137, the same eight nonempty modules and 148,476
tests, and partial SHA-256
`dbad92115f7c3a80891f6842faa191e6cb02c577955ae31fc38ba5e2e1bea5dc`.
The standalone `deqp-gles2` and `deqp-gles3` text exporters succeed with
19,825 and 48,300 tests, and filtered `glcts` exports succeed with 473
`KHR-GLES2` and 6,499 `KHR-GLES3` hierarchy cases. In the released source, the
global all-package hierarchy writer later initializes `dEQP-GL45-ES3`, whose
GLES 3 context path requests a real core GL 4.5 context even outside execute
mode. That global diagnostic still fails identically in the VM and emulator,
but the ES-on-desktop-GL package is outside the GLES qualification scope. Do
not rebuild the rootfs or patch CTS for that diagnostic. The VM is not a
ground truth or a conformance environment: only complete official execution
inside this emulator can establish the implementation result.

Kernarium accepts no arbitrary CTS script or selector. The schema-2 Apple
campaign document names the original exact GLES 3 declaration sentinel plus
fixed ordered five-case GLES 2 and twelve-case GLES 3 identity sentinels; CMake
generates their bounded launchers from the same driver into each 4 KiB and 16
KiB runtime. The generator also emits exactly two fixed discovery launchers,
`gles2-config-inventory-v1` and `gles3-config-inventory-v1`; they run only the
corresponding `CTS-Configs` case and cannot accept a config ID, case selector,
script, or engine from the app environment.
Run the native macOS gadgets lane first so it records the common host context,
full decoded Virgl capsets, direct-presentation counters, and passing QPA that
the Simulator must consume. After packaging and building a clean app, boot the
selected Simulator when needed, wait for it to become ready, and run the
ordered adapters:

```sh
python3 -B tests/integration/gpu/run_vk_gl_cts.py \
  --campaign gles3-identity-sentinel-v1 \
  --page-profile 16k --engine gadgets --display native

xcrun simctl boot SIMULATOR_UDID
xcrun simctl bootstatus SIMULATOR_UDID -b

python3 -B tests/integration/gpu/run_ios_simulator_vk_gl_cts.py \
  --campaign gles3-identity-sentinel-v1 \
  --page-profile 16k \
  --macos-run .build/vk-gl-cts/MACOS_RUN/run.json \
  --device "$(xcrun simctl getenv booted SIMULATOR_UDID)" \
  --app .build/kernarium-cts-derived/Build/Products/Debug-iphonesimulator/Kernarium.app
```

To use the checked derivative in the same ordered chain, add these arguments
to the native macOS command and both signed-app adapters, and pass the
matching derivative predecessor `run.json` at each step:

```sh
  --rootfs .build/rootfs/rootfs-gl-gles.ext4 \
  --rootfs-type ext4 \
  --rootfs-receipt tests/integration/gpu/vk_gl_cts_derivative_rootfs.json
```

The mobile adapters accept only semantic equality with the repository's
checked derivative receipt. The Simulator copies the sealed image into app
Documents with an APFS clone when available, falls back to an ordinary copy,
and enforces mode 0444. Kernarium selects `rootfstype=ext4`, passes `ro`,
attaches the lower block device read-only, and keeps all runtime writes in its
ordinary overlay disk. An arbitrary ext4 image is not accepted as CTS input.

Before constructing a signed-app full matrix, run the matching macOS config
inventory through VNC with JIT and ten vCPUs, then pass its authenticated
`run.json` to the fixed Simulator inventory:

```sh
python3 -B tests/integration/gpu/run_vk_gl_cts.py \
  --config-inventory gles3 --page-profile 16k --display vnc \
  --engine jit --cpus 10 \
  --rootfs .build/rootfs/rootfs-gl-gles.ext4 \
  --rootfs-type ext4 \
  --rootfs-receipt tests/integration/gpu/vk_gl_cts_derivative_rootfs.json

python3 -B tests/integration/gpu/run_ios_simulator_vk_gl_cts.py \
  --config-inventory gles3 --page-profile 16k \
  --device SIMULATOR_UDID \
  --macos-run .build/vk-gl-cts/MACOS_CONFIG_RUN/run.json \
  --app .build/kernarium-xcode-derived/Build/Products/Debug-iphonesimulator/Kernarium.app \
  --rootfs .build/rootfs/rootfs-gl-gles.ext4 \
  --rootfs-type ext4 \
  --rootfs-receipt tests/integration/gpu/vk_gl_cts_derivative_rootfs.json
```

Use `gles2` for the matching GLES 2 pair. Config discovery is an input to
qualification and rejects `--qualification`. The Simulator adapter reparses
the macOS QPA and requires exact ordered equality of config ID, supported
surface types, and selected surface type. The device adapter applies the same
fail-closed rule to its matching Simulator inventory before it may launch.

The complete signed matrix uses the checked schema-2 manifest; it exposes no
runtime config/fraction/case selector. GLES 2 fixes one full 473-case process
for each of the 20 configs. GLES 3 fixes four interleaved process slices for
each config, with 1,625, 1,625, 1,624, and 1,624 cases and a signed selected-
list hash for every slice. The slicing was introduced after a complete-leaf
process reproducibly OOM-killed the former 1.5 GiB guest and remains useful for
bounded process lifetime. Current qualification explicitly uses 4 GiB on
Simulator and starts at 2 GiB on the physical device, without adding swap or
omitting a case. Run the
Simulator campaign sequentially after its inventory and the matching complete
macOS campaign:

```sh
python3 -B tests/integration/gpu/run_ios_vk_gl_cts_matrix.py \
  --platform simulator --api gles3 --page-profile 16k \
  --device SIMULATOR_UDID \
  --config-inventory-run .build/vk-gl-cts-ios-simulator/CONFIG_RUN/run.json \
  --predecessor-campaign .build/vk-gl-cts/matrix/MACOS_CAMPAIGN \
  --campaign-dir .build/vk-gl-cts-ios-simulator-matrix/CAMPAIGN \
  --app .build/kernarium-xcode-derived/Build/Products/Debug-iphonesimulator/Kernarium.app \
  --rootfs .build/rootfs/rootfs-gl-gles.ext4 \
  --rootfs-receipt tests/integration/gpu/vk_gl_cts_derivative_rootfs.json
```

The campaign validates every complete macOS config predecessor before creating
its first attempt. Each compiled ID boots one fresh guest, runs its immutable
full leaf or signed slice, exports artifacts, and shuts down. Final aggregation
interleaves every GLES 3 slice back into the exact frozen 6,498-case order for
each config, rejects a duplicate or missing case, and verifies the reconstructed
leaf hash. An attempt directory is never removed or retried; interruption,
incomplete output, a disallowed runner status, or a dirty shutdown permanently
fails that campaign directory. Use the identical command plus `--verify-only`
for independent reconstruction after completion.
For the connected iPad, first run its fixed config inventory from the matching
Simulator inventory, then use `--platform device`, the normally signed device
app, and the passed Simulator matrix directory as `--predecessor-campaign`.
The device orchestrator applies the same signed 20-shard GLES 2 or 80-shard
GLES 3 single-attempt contract. Its default is `--guest-memory-mib 2048`. If
and only if the emulator is OOM-terminated on the physical device, begin a new
immutable campaign and reduce that value by exactly 64 MiB; repeat one 64 MiB
step per confirmed OOM until the emulator remains alive. The Simulator adapter
always fixes and records 4,096 MiB, and device evidence cannot use a
Simulator predecessor captured with another memory value.

The adapter verifies the frozen rootfs before copying it to app Documents,
forces the selected runtime and gadgets engine, captures the app/guest console,
decodes the same bounded artifact frames, requires exact page/DRM/Xwayland/
Virgl evidence and a `Pass` for every exact ordered case, then stops the app
after guest poweroff. An
accepted transcript must contain the successful Darwin stop, guest-exit, and
runtime-return markers and must not contain the dirty DRM memory-manager
teardown signature. Echoed launcher source is not an artifact frame; raw
frames and service-prefixed frames remain fail-closed. The opt-in host report
requests ES 3.0, 3.1, and 3.2 separately; captures sorted extensions; preserves
the full raw capset 1/2 payloads; decodes every known v1/v2 field; and records
exact macOS-to-Simulator and Simulator-to-device semantic differences. Native
campaigns also require a final frame report with nonzero guest/presented totals
and zero CPU fallback. iOS capability records use the ordered stdout/guest
console stream because `devicectl` can merge separate stdout/stderr transports
between bytes.
The strict console frames are sufficient for these bounded artifacts, so this
workflow does not require VirtioFS or a new Virtio-serial device.

The adapter does not boot a Simulator, so use the explicit `simctl boot` and
`simctl bootstatus` commands above when the selected device is shut down. The
connected-iPad wrapper uses the same campaign and validation but additionally
requires the matching Simulator `run.json` and QPA, a normally signed device
app, and a booted paired developer-enabled physical iPad:

```sh
python3 -B tests/integration/gpu/run_ios_device_vk_gl_cts.py \
  --campaign gles3-identity-sentinel-v1 \
  --page-profile 16k --device DEVICE_IDENTIFIER \
  --simulator-run .build/vk-gl-cts-ios-simulator/RUN/run.json \
  --app .build/kernarium-device-derived/Build/Products/Release-iphoneos/Kernarium.app
```

Do not run the device wrapper before its matching Simulator result, and do not
run the Simulator wrapper before its matching native macOS result. Do not
substitute Aquarium on the physical iPad. Both wrappers retain the same
`--qualification` receipt/caselist refusal as the macOS driver.

The 2026-08-21 16 KiB diagnostic passed the allow-listed case 1/1 first on the
iOS 26.5 iPhone 17 Pro Max Simulator and then on the wired iPad mini 6 running
iOS 18.5. Both produced identical renderer and selected-caselist artifacts,
reported desktop GL 2.1/GLES 3.0, and returned guest/runtime status zero. Its
first iPad poweroff exposed an intermittent dirty `drm_managed_release`.
Maintained kernel topic 0020 shuts atomic scanout down before device unplug;
after both page-profile kernels and all runtime slices were rebuilt, three
consecutive Simulator runs and three gated iPad runs passed the exact case and
the clean-shutdown validator. That focused repair does not satisfy the broader
performance, thermal, sustained-lifecycle, or qualification gates.

The final broader identity runs passed the exact five-case GLES 2 and
twelve-case GLES 3 sets under both 16 KiB and 4 KiB on macOS first, the booted
Simulator second, and the connected iPad last. Predecessor validation reparses
the selected list and QPA and rechecks campaign, leaf, seed, renderer,
capability, and frame evidence.
The accepted runs all shut down cleanly and reported zero CPU fallback. None
of the three diagnostic non-passes discovered while selecting these bounded
sentinels was waived. The complete macOS matrix classifies the depth case as
15 `Pass` plus five released-policy `NotSupported` results on no-depth
configurations, and the repaired transform-feedback query case passes all 20
configurations. The former Simulator GLES 2 texture findings are repaired by
the swizzle and float-filter topics. Managed topic `fff809d63b` separately
enables native Metal sampler comparison on modern arm64 Simulator and repairs
the eight GLES 3 depth/stencil shadow texture-repeat failures from the first
bounded slice. Both current 16 KiB macOS matrices pass as recorded above.
Fresh fixed signed Simulator inventories for both APIs match the same 20
tuples and canonical digest under the explicit 4,096 MiB policy at
`.build/vk-gl-cts-ios-simulator/20260822-131649-63237-16k-gles3-config-inventory-v1`
and
`.build/vk-gl-cts-ios-simulator/20260822-131735-63365-16k-gles2-config-inventory-v1`.
The repaired GLES 3 `cfg00` fraction `0/4` then completed 1,625/1,625 with
1,563 `Pass`, 62 permitted `NotSupported`, and no failure or CPU fallback at
`.build/vk-gl-cts-ios-simulator/focused/gles3-shadow-sampler-fff809d/20260822-131903-63508-16k-gles3-matrix-cfg00-000of004-v1`.
The first connected-iPad attempt at 2,048 MiB completed normally but found one
real failure in the `cfg00` fraction `2/4` RGB32F linear-filter case. Because
the emulator was not OOM-terminated, do not reduce RAM for that result.
Managed UTM WebKit topic `5328f5eb95` enables the common Metal R/RG/RGBA32F
filter path after the A15/Apple8 capability query withheld it despite the
passing focused hardware operation.
With the rebuilt normally signed app, the exact same device slice passed all
1,624 executions with 1,563 `Pass`, 61 permitted `NotSupported`, clean
shutdown, and zero CPU fallback at
`.build/vk-gl-cts-ios-device-probe/float32-force-56b25e98/20260822-154345-73520-16k-gles3-matrix-cfg00-002of004-v1`.
Complete fresh-policy case matrices remain outstanding on Simulator/iPad.

The common macOS ANGLE/Metal provider is now the only built path, so diagnostic
sentinels may run when the guest prerequisites are available. The host
transcript must contain its exact backend marker and any CGL marker is fatal.
`--qualification` still refuses to use the original image while its frozen
identity lacks an authoritative CTS build receipt. The checked ext4 derivative
has its own authenticated build receipt and is the required input for future
qualification runs. Its 16 KiB macOS GLES 2/GLES 3 cross-products pass;
complete Simulator/iPad matrices remain. Its scoped display-backed GLES
2/GLES 3 hierarchy inventory is complete and frozen; the separate global
all-package diagnostic is not a qualification input because it includes an
ES-on-desktop-GL 4.5 package outside the supported profile. See
[`GL_GLES_BASELINE.md`](GL_GLES_BASELINE.md). Do not mutate either immutable
lower image or work around the block by overlaying or preloading replacement
guest graphics userspace.

`guest-docker-hello-world` joins that opt-in set. `guest-overlay-systemd-smoke`
already proves Docker runs a locally imported image with no network; this one
covers the path a real user takes instead — registry DNS and TLS over SLiRP, an
image pull, and container networking over the `docker0` bridge. It is also the
broadest Go exercise in the suite, because `dockerd`, `containerd` and the
client all read HWCAP-gated system registers during startup. Run it directly
against either engine with:

```sh
python3 tests/integration/gpu/run_gpu_guest.py \
  --emulator .build/presets/macos-integration/bin/uml-darwin \
  --rootfs rootfs.erofs \
  --command "docker run --rm hello-world" \
  -- --execution-engine=gadgets
```

### JIT translation cache budget

The JIT sizes all retained translation storage from a whole-process budget. A
complete private working set is capped at 64 MiB per executor; once those fixed
working sets are funded, the process-global shared tier receives the entire
remainder and is the only tier that grows with guest memory. By default the
whole budget is half of guest memory, read from the `mem=` argument the host
passes the guest, so an eight-gigabyte, ten-vCPU guest has a 4 GiB accounted
ceiling: at most ten private working sets plus roughly 3.4 GiB for shared
translations. Shared shard/map backing is allocated on first insertion rather
than at budget publication. This is an accounting bound, not a promise that
the process immediately commits that much RSS.
The host derives the divisor with the same policy as the maintained kernel:
an explicit `ncpus=` value is clamped to 1 through `CONFIG_NR_CPUS` (64), while
an omitted value selects the online host-processor count capped at 64. This
keeps the whole-process bound intact for the kernel's automatic SMP default.
Repeated `mem=` arguments also retain their UML meaning: the last occurrence
wins, numeric prefixes select decimal, octal or hexadecimal as in base-zero
parsing, and the case-insensitive suffix grammar is `K`, `M`, `G`, `T`, `P`,
or `E`. UML ignores text after the parsed suffix, so the budget calculation does
too. This kernel compatibility does not loosen the host-only option below.

`--jit-cache-budget=` overrides the total. The separate host-only
`--jit-executor-cache-budget=` option may lower, but never raise, the 64 MiB
private ceiling for diagnosis. Both are consumed before the kernel command line
is built and accept a byte count with an optional `K`, `M` or `G` suffix:

```sh
.build/presets/macos-integration/bin/uml-darwin \
  --jit-cache-budget=1G --jit-executor-cache-budget=32M \
  ubd0c=rootfs.erofs root=/dev/ubda \
  rootfstype=erofs mem=8G ncpus=10
```

Lowering it trades throughput for memory: the campaign in G135 of
`docs/HOT_PATH_EXPERIMENTS.md` measured about 8% on multi-core Clang and Object
Detection between the former fixed 131072-set/512-MiB-arena geometry and the
4 GiB default budget of an 8 GiB guest. Raising the budget past half of guest
memory is allowed but the host must have the memory to back it.
Geometry scales down to one cache set and 16 KiB code and tracked-store arenas,
and launch fails with `UML_EMULATOR_RUNTIME_ERROR` if even that accounted
per-executor minimum does not fit. The configuration is process-wide and
first-valid-configuration-wins: another run may repeat the exact total, CPU
count, and private ceiling, but an incompatible later or concurrent run fails
instead of silently using geometry derived from the winning run. An executor
constructed before explicit publication claims the documented bounded
one-executor fallback configuration.
Within an admitted emulator run, the configured CPU count is also the exact
ceiling on retained JIT executors. Task-carrier host threads keep lightweight
engine wrappers and borrow those executors exclusively for each guest entry;
an N+1 simultaneous borrower waits for a slot instead of constructing an
unbudgeted executor or reporting lease exhaustion, and an already queued
borrower takes priority over a tight preferred-slot reborrow. A managed session
belongs to one active runtime memory domain. Executors may switch CPU states,
guest generations, and backings within that domain; session teardown drains
the pool before the domain retires, so they never cross runs or memory domains.
A wrapper or borrower that survives an explicitly misordered teardown fails
closed and cannot attach to a later session or a different code-generation
capability profile. Idle ownership is published through one 128-byte-aligned
atomic session-token cell per executor, so independent slots do not update one
shared availability mask and an old session cannot claim a replacement
executor even if its allocation reuses the same address. From registry
reservation through final managed teardown, standalone executor construction
is rejected; only the pool's private tagged lease path is admitted. The
four-slot `arm64-jit-executor-pool-multislot` fixture pins lazy lowest-slot
construction and fallback, concurrent exclusive handoff, FIFO priority, mixed
idle/active drain, stale pre-claim restart, activation-window lease exclusion,
and fresh-session recovery; the existing one-slot fixture retains the
memory-preparation and stale-wrapper contracts.

Every checked configure preset inherits an explicit production-selector
contract: `UML_JIT_SHARE_TRANSLATIONS_BETWEEN_EXECUTORS=ON`,
`UML_JIT_CARRY_X8_X9_ACROSS_LINKS=OFF`, and
`UML_GADGET_ADD_SUB_IMMEDIATE_SUCCESSOR_HOIST=OFF`. This includes the ordinary
page profiles, integration, profiling, sanitizer, loader, GPU, and Apple
library presets, so reconfiguring a canonical build tree cannot retain a stale
experimental cache value. Use a separately named build tree and an explicit
`-D` override for an A/B arm; do not mutate a canonical preset tree in place.
The manually configured GPR-link profiling build is a source-level exception:
it keeps executor-owned counter pointers private by disabling translation
sharing, and it rejects the x8/x9 carry option as incompatible.

The completed fixed-x8/x9 carry microprobe is recorded at
`.build/results/jit-cache-handoff/carry-x8-x9-matched-perf-final-20260812/README.md`.
It used separate Release build directories and a symmetric x0/x1 control,
JIT/1 GiB/one worker, P10 and E4, repetition-1 warmup,
and fixed `ABCD DCBA BADC CDAB` order. The strict result is P PASS/E
FAIL/overall FAIL; a separately labeled user post-hoc cross-core tolerance
accepts the prototype, but does not change the production selector. Keep
`UML_JIT_CARRY_X8_X9_ACROSS_LINKS=OFF` in checked presets until a
representative workload or Geekbench disposition supports enabling it. E-core
results use `taskpolicy -c background`, which is a process-wide QoS clamp and
must not be described as hardware affinity.

### Opt-in JIT profiling emulator

`macos-jitprof` inherits the release `macos-integration` configuration and
changes only `UML_ENABLE_RUNTIME_PROFILING=ON`. It therefore uses the same
verified dependency prefix, case-sensitive Linux source, 16 KiB kernel build,
CLI, and device providers as the production comparison while keeping all
profiling sources in a separate build tree. Build it and run its focused preset
contract plus the retired-and-live-tail final-report fixture (including a
deliberately held stderr `FILE` lock) with:

```sh
cmake --workflow --preset check-jitprof

# Equivalent configure and build steps when the focused audits are not needed.
cmake --preset macos-jitprof
cmake --build --preset macos-jitprof
```

The profiling executable is
`.build/presets/macos-jitprof/bin/uml-darwin`. The workflow does not boot a guest
or establish benchmark evidence; measured campaigns use `tools/guest-shell.py`
without `--quiet`, because profiling output is written to emulator stderr.
`guest-shell.py`, the Geekbench runner, and the standalone timing runners
report success only when their requested commands complete and the emulator
then halts within the orderly-shutdown deadline with status zero. A shutdown
timeout or nonzero emulator status fails the run without replacing an earlier
command failure status; forced close remains cleanup rather than evidence.

The principal runtime controls are `UML_JIT_PROFILE_COMPILE=1`,
`UML_JIT_PROFILE_OVERLAP=1`,
`UML_JIT_PROFILE_SHARED_TRANSLATIONS=1`, and the unscored mechanism trace
`UML_JIT_PROFILE_EXECUTOR_POOL=1`. The first three emit periodic cumulative
snapshots and a process-wide final summary after a clean, joined shutdown; the
pool trace emits only its change-detected final.
After the embedded ELF entry returns with its task carriers retired, the
emulator invokes an explicit, idempotent reporting boundary before releasing
the session-owned executor pool. The profiling CLI repeats that boundary after
`uml_emulator_run()` returns and before its deliberate `std::_Exit()`. It merges
both already-retired recorders and the sub-interval tail of every still-live
thread-local compile/overlap recorder, then emits the executor-pool snapshot
before invoking the global shared-cache reporter. When both snapshots changed,
the pool record therefore precedes the shared record; change suppression may
silence either record independently. Runtime records are formatted in bounded
private memory and written
directly to stderr's file descriptor with partial-write and interruption retry;
a nonblocking descriptor waits for writable backpressure before resuming the
exact unwritten suffix. Each bounded buffer carries a leading LF in that same
descriptor write, terminating any unrelated partial PTY line before its
parseable record body. They neither acquire nor flush the shared stderr `FILE`
object. Compile/overlap and shared reporters retain normal-exit callbacks for
non-CLI processes. The executor-pool reporter is intentionally explicit-only
because it requires a quiescent pre-drain boundary. Unchanged explicit calls or
subsequent compile/overlap/shared normal-exit callbacks emit no duplicate.
If a process starts another embedded run or records more profiling activity,
the next reporting boundary emits a new cumulative final, followed again by
silence until the state changes. Expansion dumps use one bounded header and
typed, numbered continuation records; concurrent long dumps remain separately
reconstructible without a process-wide output lock.

The executor-pool trace keeps ordinary counters and a fixed association set in
each profiling-build JIT wrapper. Every successful borrow associates the
wrapper's captured session and current nonnull sequence address with the
claimed slot. The borrow path does not increment a process-global profiling
atomic. Wrapper construction, retirement, cold
standalone-lease lifetime events, and the quiescent final use one private
registry mutex; retired and still-live wrapper records are aggregated at that
boundary. The final reports borrow preference and fallback behavior, cell CAS
attempts, waiter enqueue/wake/FIFO handoffs, managed construction, created and
observed peak-live/borrowed populations, wrapper slot changes, null and changed
interrupt-sequence observations, same-sequence/different-slot movement,
bounded sequence wrapper fanout and extra-wrapper associations, distinct
session/sequence identities, sequence multi-slot counts, extra slot
associations, maximum slot fanout, overflow, and standalone construction/live
peak. The merge keys by session and sequence, ORs wrapper-local slot masks, and
never emits either key; the consecutive same-sequence/different-slot count is
only supplemental transition evidence.
`peak-borrowed-observed=` is a post-claim diagnostic scan, not a linearizable
global maximum. Profiling code is excluded from ordinary builds. When the environment control is
unset, the adapter passes a null profile and the pool skips the diagnostic
post-claim scan. The profiling product still contains conditional branches,
and trace-enabled runs add post-claim scans, so its timings are mechanism
diagnostics and are unusable as benchmark or score evidence. The bounded pool
final can exceed `PIPE_BUF`; it is emitted only at the quiescent reporting
boundary, where no carrier profile producer remains, and the direct descriptor
writer already resumes exact suffixes after partial writes, so it is not split
into continuation records.
Compile reporting defaults to every 4,096 blocks per translating host task
thread, overlap reporting to every 2^20 translations after its first batched
flush, and shared-translation reporting to every 65,536 publications. The
shared report's process-wide fields include `entries=`, `published=`,
`matched=`, `reused=`, `missed=`, `rejected=`, `degraded-rejected=`,
`malformed-rejected=`, `insertion-failures=`,
`failed-insertion-retained-accounted-bytes=`, `materialization-failures=`,
`invalid-metadata=`, `direct-link-capacity=`, `indirect-link-capacity=`,
`arena-exhaustion=`, `payload-bytes=`, `accounted-bytes=`, and
`maximum-accounted-bytes=`. Retained failure bytes are conservative accounted
reservation bytes after successful map insertion followed by entry-vector
failure, not measured live heap bytes. An
abnormal exit
before the joined reporting boundary can still omit thread-local compile or
overlap batches, so retain the guest completion marker, final lines, and normal
emulator exit with every final-profile claim. The ordinary
`macos-integration` product keeps this profiling code compiled out.

The compile profile distinguishes branch-operand `direct-link-refusals=` and
`indirect-link-refusals=` from `target-link-refusals=`, where the latter means a
compiled block's arena-adjacent target record could not enter the fixed reverse
index during installation. `direct-link-table-high-water=` is the maximum
registered arena-footer population and `indirect-link-table-high-water=` is the
maximum occupied PIC-record population;
their matching `*-capacity=` fields are the configured per-executor bounds.
Periodic lines are cumulative for their translating host task thread, while
the final line sums refusal/reset counts and takes the process-wide maximum of
each table occupancy and capacity. `arena-resets=` counts failed primary
code-arena stores that immediately trigger a whole block/link-cache clear and
primary arena reset; it does not count the separate tracked-store or
floating-transaction arena resets. `local-compile-calls/ns/avg-ns` measures the
complete successful local compile, private publication, optional canonical
publication, and install path. `shared-materialization-calls/ns/avg-ns`
measures the complete successful shared lookup, metadata/relocation handling,
private code/footer regeneration, and install path. These fields are compiled
out of ordinary products and are intended for relative mechanism diagnosis,
not benchmark scoring.

Full LTP is independently selected, never a unit test. The unified image
contains it at `/opt/ltp`; run it separately per engine. Cases whose required
kernel options are deliberately disabled belong
in `disabled-tests.tsv` rather than prompting config re-enablement:

```sh
cmake --build .build/presets/macos-integration --target guest-ltp-gadgets
cmake --build .build/presets/macos-integration --target guest-ltp-jit
# Convenience aggregate after both independent campaigns are desired:
cmake --build .build/presets/macos-integration --target guest-ltp-verified
python3 tests/integration/ltp/run_guest_ltp.py --help
```

A direct focused run must name the matching Kbuild metadata explicitly; the
runner validates its complete canonical path inventory and sibling kernel
release before booting cases:

```sh
python3 tests/integration/ltp/run_guest_ltp.py \
  --emulator .build/presets/macos-integration/bin/uml-darwin \
  --rootfs rootfs.erofs \
  --manifest tests/integration/ltp/verified-tests.tsv \
  --modules-builtin .build/kernel/artifacts/mainline-6.18-16k-release/kernel/modules.builtin \
  --case containers/netns_sysfs
```

The CMake engine targets supply the selected artifact's `kernel/modules.builtin`
automatically. The harness copies
that complete file byte-exact into disposable OverlayFS state and replaces
`modules.dep` with an empty file only when the running config proves
`CONFIG_MODULES=n`. The generated setup is gzip-compressed on the host and
transported through independently acknowledged base64 append commands below
2 KiB; a final bounded command decodes, decompresses, executes, and removes the
staging files under `/run`, and the harness restores console echo on every
path. It also places fail-closed overlay-only `kmod`, `modprobe`, and `rmmod`
names ahead of the LTP binary directory in `PATH`. Fake loads are no-ops
limited to exact built-in names from the validated metadata; removal,
parameters, unknown names, and unsupported operations fail. The immutable
rootfs does not need kmod for this static-kernel accommodation.

The 10-core topology, utilization, and hotplug workload is also deliberately
outside CTest. Run its standalone target explicitly:

```sh
cmake --build .build/presets/macos-integration --target guest-smp-parallel
```

Provisioning an image is explicit and authenticates the candidate against the
pinned manifest before atomic installation. It refuses to replace an existing
image without `--replace`. Network acquisition additionally verifies a
detached signature over the release statement; pass the URL, statement,
signature, trusted public key, and `--allow-network` explicitly or through the
corresponding `UML_ROOTFS_*` environment variables. This work is performed once
when installing an artifact, not before or after guest runs. EROFS updates are
complete image replacements; in-place writable maintenance is unsupported.

Guest benchmarks use payloads already installed under `/opt`; they do not
download inputs at run time. Geekbench 6 runs locally without an Internet
preflight or upload permission gate:

```sh
python3 tests/perf/run_guest_geekbench.py \
  --emulator .build/presets/macos-integration/bin/uml-darwin \
  --rootfs rootfs.erofs \
  --console-log .build/results/geekbench-gadgets-16k.log \
  --engine gadgets --cpus 10 --memory 8G
```

The runner's default guest command is
`/opt/geekbench/release/geekbench6 --no-upload`.
It runs from a direct tmpfs-backed OverlayFS shell and does not start systemd
or its background services. Production systemd boot remains covered by the
separate integration workflows and is not part of performance measurement.
See [`../tests/perf/README.md`](../tests/perf/README.md) for profiling and probe
commands. Full Geekbench scoring remains a separate manual campaign.

The opt-in Kernarium physical-device equivalent is selected only by launching
the installed app with `KERNARIUM_GEEKBENCH=1`. It runs the same image-local
payload without systemd, using the app's recreated 8 GiB ext4 disk as the
OverlayFS upper storage. Its console completion contract is
`KERNARIUM_GEEKBENCH_READY`, `KERNARIUM_GEEKBENCH_END:0`, clean guest poweroff,
and status-zero managed-emulator completion. The exact physical `devicectl`
and Time Profiler commands are documented in
[`../tests/perf/README.md`](../tests/perf/README.md).
An optional validated `KERNARIUM_GEEKBENCH_WORKLOADS` comma-separated numeric
list selects focused single-core workloads without changing the normal full
campaign.

Kernarium passes `--execution-engine=auto`, which both Apple products resolve
to the gadget executor. `KERNARIUM_EXECUTION_ENGINE` overrides that with
`auto`, `gadgets`, or `interpreter`; an unrecognised value is reported and
ignored. The override exists so an engine A/B needs no rebuild. `jit` is not
selectable on either iOS product: a device cannot publish into a `MAP_JIT`
mapping, and the Simulator is deliberately held to the same answer so it keeps
representing the device. Requesting it fails rather than silently falling back.

G107's bounded successor fetch is selected by the default-on
`UML_GADGET_ADD_SUB_IMMEDIATE_SUCCESSOR_HOIST` CMake option. When changing the
mechanism, use separate explicit OFF and ON build directories. Keep translation sharing ON, x8/x9 carry
OFF, and every profiling selector OFF. The exact focused host gates are
`arm64-gadget-successor-hoist-structure` and `arm64-gadget-terminal-count`;
run them with `architecture-audit` in 16 KiB and 4 KiB Release OFF/ON trees and
repeat the focused tests in a 16 KiB ON
ASan/UBSan tree. The timing payload is
`guest_gadget_polymorphic_successor_loop`; its paired low-polymorphism
same-family throughput control is `guest_add_immediate_loop`. Run both through
`run_guest_link_bench.py` on performance and efficiency classes. The control is
not literally monomorphic: its selected ADD
branch sees itself 15 times and SUB once per loop.

The completed 2026-08-12 comparison is recorded at
`.build/results/jit-cache-handoff/g107-successor-hoist-matched-perf-setup-20260812/`.
It used eight one-repetition preflights followed by 16 P then 16 E scored
invocations, three samples each and no retry/deletion. P polymorphic/control
wall improved 13.723512%/0.516300% with 13.207211 points
difference-in-differences; E improved 7.258795%/5.481481% with 1.777314 points.
Polymorphic host CPU improved 13.888889%/9.683605% on P/E; all wall ranges were
disjoint and all counterbalanced halves were beneficial. The prospective
dynamic policy (`P > 2%`, E regression strictly below twice measured P) and the
original stricter screen both pass; the stronger 2-point
difference-in-differences preference fails on E overall and by half. The host
load gate was user-waived and E selection is QoS rather than hardware affinity,
so retain every sample and the recorded load/spread limitations. The focused
result does not authorize a bank-wide successor-register rewrite. A subsequent
current-tree representative check improved full Single-Core 94 to 97 and
Multi-Core 564 to 567; a compact rerun of the full run's File Compression and
HTML5 outliers was within 1.3% of OFF. That closes the representative boundary
and enables only the bounded 32-entry family by default. Raw logs are under
`.build/results/gadgets-optimization-20260813/`.

The default-on `UML_GADGET_SIMD_DESTINATION_RESIDENCY` option is intentionally
bounded to runs of at least four nonfaulting integer MLA instructions with one
v0-v7 destination and nonaliasing sources. Its exact focused test is
`arm64-gadget-simd-destination-residency`; pair it with
`arm64-simd-three-same-arithmetic` in both page profiles when changing the
mechanism. The sensitive/control probes are
`guest_simd_destination_residency_loop` and
`guest_simd_destination_residency_control_loop`. Current evidence records
69.6%/43.5% sensitive gains on P/E, targeted SIMD Geekbench 91→92, and a clean
full-gadget Geekbench result of 96/575 versus the G107 predecessor's 97/567.
One earlier full attempt was stopped after an RCU stall; do not treat that
partial run as a score.

The default-on `UML_GADGET_EXACT_CACHE_VALIDATION` option preserves decoded
programs across same-generation code-change notifications only after comparing
all consumed guest words. Its exact host gate is
`arm64-gadget-cache-validation`; run it in both page profiles and also run an
explicit OFF tree when changing the compile-time boundary. The sensitive and
control probes are `guest_gadget_code_invalidation_loop` and
`guest_gadget_code_invalidation_control_loop`. Current P/E sensitive gains are
62.4%/50.6%; targeted multi-core Clang/Object Detection changed 394/461 to
383/464 (combined 426 to 422). The final full-gadget result is 96/567 versus
96/575 for the preceding G140 build and 94/564 for the original baseline, so
there is no obvious aggregate regression. Raw logs are under
`.build/results/gadgets-optimization-20260813/`.

`UML_GADGET_CACHE_SLOTS` (default 65536) sizes the gadget cache's shared slot
pool and `UML_GADGET_RESIDENT_GENERATIONS` (default 8) bounds how many address
spaces may hold translations in it at once. Both are per-executor. Their exact
host gate is `arm64-gadget-cache-validation`; the design and the measured
alternatives are recorded in G160.

The default-on `UML_GADGET_INLINE_CONDITIONAL_FALLTHROUGH` option translates a
conditional branch's fall-through inline behind it instead of ending the block
there, which is worth 60.1% of frame rate on WebGL Aquarium and recorded in
G162. Its exact host gate is `arm64-gadget-inline-fallthrough`; run it in both
page profiles. It pins what no conformance test can see, because the guest
registers are identical either way and only the retirement count differs: a
taken branch now leaves from the middle of a block that was precharged its whole
length, so it must give the suffix back. `UML_GADGET_BLOCK_INSTRUCTIONS`
(default 64) is the fetch window rather than a limit on block length; only 0.07%
of blocks run far enough to be cut off by it.

`UML_GADGET_SHARED_BUDGET_MIB` (default 0, off) is an experimental arm: a
process-wide budget for translations one executor built and another may adopt.
It works -- it removes 90% of translation work -- and costs frame rate anyway,
because an adoption is two to three times the cost of the gadget build it
replaces. G161 records the measurements; do not enable it without re-measuring.
Enabling it registers `arm64-gadget-shared-translation`, which pins that an
adopted program outlives the executor that built it, that one address space may
adopt another's, that a differing code-generation profile does not share, and
that an adopted block still revalidates its guest words. Run that under the
sanitizer preset too when changing the relocation contract: a canonical entry
holds zero in every cell naming executor-owned link state, so a missed
relocation faults on a null stream rather than branching into a destroyed
executor's memory, and the test destroys the producer before the consumer
executes.

Sanitizer, iOS, XCFramework, benchmark, and guest-workload runs are separate
acceptance campaigns.  Typical host-product commands are:

```sh
cmake --preset macos-unit-16k-asan-ubsan
cmake --build --preset asan-ubsan
ctest --preset unit-asan-ubsan -j 10 \
  --resource-spec-file "$PWD/cmake/ctest-resources.json"

cmake --preset macos-unit-16k-tsan
cmake --build --preset tsan
ctest --preset unit-tsan

# Canonical rootfs-free GPU/display descriptor-ownership gates.
cmake --workflow --preset check-gpu
cmake --workflow --preset check-gpu-asan-ubsan

# Canonical malformed embedded-loader sanitizer gate.
cmake --preset macos-loader-asan-ubsan
cmake --build --preset loader-asan-ubsan
ctest --preset loader-asan-ubsan

cmake --preset ios-device
cmake --build --preset ios-device
cmake --preset ios-simulator
cmake --build --preset ios-simulator
scripts/build-xcframework.sh
```

`unit-tsan` intentionally runs twelve tests labelled `tsan`:
`emulator-native-16k-ownership`, `static-mmu-batch`,
`arm64-pc-alignment-contract`, `arm64-static-runtime`,
`arm64-jit-publication_gate`, `arm64-jit-cache-geometry`,
`arm64-jit-shared-translation-cache`, `arm64-jit-executor-pool`,
`arm64-jit-executor-pool-multislot`, `memory-domain-lifetime`,
`runtime-route-transaction`, and `runtime-context-lifecycle`. It does not
select the instruction-evidence reset. The full normal and ASan/UBSan suites
retain the complete registered ordinary instruction-test selection.

The two GPU workflows are host-only and rootfs-free. They configure the shared
GPU, input, and audio-device graph; build the GPU and Virtio Sound unit
fixtures and all registered tooling fixtures plus the architecture audit; run
the focused GPU and audio unit selection; and then run the independently
labelled `virtio-gpu-vnc-smoke` tooling check. This covers service ownership,
dynamic queue counts, deferred used-ring publication, sound protocol
negotiation, direct-presentation protocol bypass, VNC lifetime/port reuse, and
the real LibVNCServer wiring normally or under ASan+UBSan. The interactive
AppKit window and audible Core Audio output remain external GUI checks; they
are not opened by ordinary host acceptance. Loopback socket policy must permit the VNC
port-reuse cases.

The strict `unit` selection is Python-free. In configurations that enable both
unit and tooling tests, it includes the CMake evidence reset so stale per-test
TSVs are removed before producers run. In a unit-only, tooling-disabled graph,
neither the reset nor the tooling validators are registered. The
`instruction-evidence` presets add the validator and are standalone after the
matching build preset. A raw `tooling` selection is non-destructive: in a
unit-enabled tree its evidence validator requires a preceding unit producer
run, as provided by `check-unit`; in the tooling-only tree that conditional
validator is not registered.

With `UML_INSTRUCTION_INPUT_MATERIALIZATION=OFF`, the ordinary combined
unit/tooling graph writes no input sidecars and does not register
`arm64-input-materialization-join`. Setting the option to `ON` makes each unit
producer record one compressed sidecar and registers exactly that join. The
join deliberately requires the complete producer set, so selecting it without
first completing the matching unit run fails instead of yielding a partial
index. Raw sidecars stay under the build tree. The resumable checkpoints and
contract index can be placed on a larger volume by setting the CMake cache path
`UML_INSTRUCTION_MATERIALIZATION_WORK_ROOT`; page-size-specific subdirectories
are added automatically. The registered join preserves at least 5 GiB on the
checkpoint volume and rejects a producer before its bounded working estimate
would consume that reserve. This opt-in exhaustive tooling gate covers roughly
30 million contributions and 449 producer shards per page profile, uses
resumable checkpoints, and has a bounded four-hour CTest timeout.

## Exact official A64 campaign

The `arm64-aslref-campaign-*` CTests are host-only fixtures for publication,
authority reconstruction, sharding, cancellation/resume, and final joining.
They do **not** execute the complete official campaign. The complete campaign
starts with fresh 4 KiB and 16 KiB unit evidence, reconstructs all 449 producer
checkpoints for each profile, and then executes the commands below. It can run
for days and must remain outside ordinary CTest.

Use a new absolute work root on a local filesystem. Output, scratch, and run
roots are siblings. Authenticate the official Arm archive and source PDF before
extracting the architecture input; that external acquisition check remains the
release/version boundary. The campaign itself records selected paths and
semantic input bindings but does not snapshot or repeatedly hash the archive,
PDF, repository sources, Python runtime, or tools. The example official-tool
paths are the paths found by the 2026-08-05 audit; replace any that no longer
exist. Every `test` below must succeed before proceeding.

```sh
REPO=/Users/dev/Downloads/linux-um-darwin
WORK=/Volumes/<case-sensitive-volume>/a64-official-2026-06-run-001
ARCH_ROOT=/private/tmp/a64-asl-2026-06.3oq9iS/ISA_A64_xml_A_profile-2026-06
FEATURES=/private/tmp/a64-features-2026-06.S4QKh4/Features.json
ASLREF=/private/tmp/aslref-inspect.k1syjc/herdtools7-aslref-v1.0_draft.2/_build/default/asllib/aslref.exe
BUNDLER=/private/tmp/aslref-inspect.k1syjc/herdtools7-aslref-v1.0_draft.2/herd/libdir/asl-pseudocode/bundler.py

test ! -e "$WORK"
test -d "$ARCH_ROOT"
test -f "$FEATURES"
test -x "$ASLREF"
test -f "$BUNDLER"

mkdir -p "$WORK"/join-4k "$WORK"/join-16k \
  "$WORK"/shards-4k "$WORK"/shards-16k \
  "$WORK"/local-shard-4k "$WORK"/local-shard-16k \
  "$WORK"/local-plan-4k "$WORK"/local-plan-16k \
  "$WORK"/plans "$WORK"/run-4k "$WORK"/run-16k \
  "$WORK"/local-run-4k "$WORK"/local-run-16k \
  "$WORK"/local-verify-4k "$WORK"/local-verify-16k \
  "$WORK"/local-promotion "$WORK"/promotion

cd "$REPO"
cmake --preset macos-unit-4k \
  -DUML_INSTRUCTION_INPUT_MATERIALIZATION=ON
cmake --build --preset unit-test-binaries-4k
ctest --preset unit-4k
cmake --preset macos-unit-16k \
  -DUML_INSTRUCTION_INPUT_MATERIALIZATION=ON
cmake --build --preset unit-test-binaries
ctest --preset unit

EVIDENCE_4K="$REPO/.build/presets/macos-unit-4k/instruction-evidence"
EVIDENCE_16K="$REPO/.build/presets/macos-unit-16k/instruction-evidence"
test "$(find "$EVIDENCE_4K" -name '*.tsv.inputs.jsonl.gz' -type f | wc -l | tr -d ' ')" -eq 449
test "$(find "$EVIDENCE_16K" -name '*.tsv.inputs.jsonl.gz' -type f | wc -l | tr -d ' ')" -eq 449

python3 -B tests/tooling/arm64/join_a64_input_materializations.py \
  --evidence-directory "$EVIDENCE_4K" \
  --guest-page-size 4096 --require-execution-ready \
  --output "$WORK/join-4k/index.tsv" \
  --contract-index-output "$WORK/join-4k/contracts.tsv.gz" \
  --checkpoint-directory "$WORK/join-4k/checkpoints" \
  --free-space-reserve-gib 20
python3 -B tests/tooling/arm64/join_a64_input_materializations.py \
  --evidence-directory "$EVIDENCE_16K" \
  --guest-page-size 16384 --require-execution-ready \
  --output "$WORK/join-16k/index.tsv" \
  --contract-index-output "$WORK/join-16k/contracts.tsv.gz" \
  --checkpoint-directory "$WORK/join-16k/checkpoints" \
  --free-space-reserve-gib 20

python3 -B tests/tooling/arm64/run_a64_aslref_campaign.py shard \
  --evidence-directory "$EVIDENCE_4K" --guest-page-size 4096 \
  --materialization-index "$WORK/join-4k/index.tsv" \
  --contract-manifest "$WORK/join-4k/contracts.tsv.gz" \
  --output-directory "$WORK/shards-4k" \
  --local-temp-root "$WORK/local-shard-4k" \
  --maximum-shard-bytes 536870912 --maximum-metadata-bytes 134217728 \
  --maximum-database-bytes 137438953472 \
  --maximum-merge-scratch-bytes 17179869184 \
  --minimum-output-free-gib 20 --minimum-local-free-gib 20
python3 -B tests/tooling/arm64/run_a64_aslref_campaign.py shard \
  --evidence-directory "$EVIDENCE_16K" --guest-page-size 16384 \
  --materialization-index "$WORK/join-16k/index.tsv" \
  --contract-manifest "$WORK/join-16k/contracts.tsv.gz" \
  --output-directory "$WORK/shards-16k" \
  --local-temp-root "$WORK/local-shard-16k" \
  --maximum-shard-bytes 536870912 --maximum-metadata-bytes 134217728 \
  --maximum-database-bytes 137438953472 \
  --maximum-merge-scratch-bytes 17179869184 \
  --minimum-output-free-gib 20 --minimum-local-free-gib 20

TOTAL_4K=$(python3 -B -c 'import json,sys; print(json.load(open(sys.argv[1], encoding="utf-8"))["total_contracts"])' "$WORK/shards-4k/shard-set.json")
TOTAL_16K=$(python3 -B -c 'import json,sys; print(json.load(open(sys.argv[1], encoding="utf-8"))["total_contracts"])' "$WORK/shards-16k/shard-set.json")
test "$TOTAL_4K" -gt 4096
test "$TOTAL_16K" -gt 4096

# Re-run in exact-resume mode so the derived totals become explicit gates.
python3 -B tests/tooling/arm64/run_a64_aslref_campaign.py shard \
  --evidence-directory "$EVIDENCE_4K" --guest-page-size 4096 \
  --materialization-index "$WORK/join-4k/index.tsv" \
  --contract-manifest "$WORK/join-4k/contracts.tsv.gz" \
  --output-directory "$WORK/shards-4k" \
  --local-temp-root "$WORK/local-shard-4k" \
  --expected-total-contracts "$TOTAL_4K" \
  --maximum-shard-bytes 536870912 --maximum-metadata-bytes 134217728 \
  --maximum-database-bytes 137438953472 \
  --maximum-merge-scratch-bytes 17179869184 \
  --minimum-output-free-gib 20 --minimum-local-free-gib 20
python3 -B tests/tooling/arm64/run_a64_aslref_campaign.py shard \
  --evidence-directory "$EVIDENCE_16K" --guest-page-size 16384 \
  --materialization-index "$WORK/join-16k/index.tsv" \
  --contract-manifest "$WORK/join-16k/contracts.tsv.gz" \
  --output-directory "$WORK/shards-16k" \
  --local-temp-root "$WORK/local-shard-16k" \
  --expected-total-contracts "$TOTAL_16K" \
  --maximum-shard-bytes 536870912 --maximum-metadata-bytes 134217728 \
  --maximum-database-bytes 137438953472 \
  --maximum-merge-scratch-bytes 17179869184 \
  --minimum-output-free-gib 20 --minimum-local-free-gib 20

python3 -B tests/tooling/arm64/run_a64_aslref_campaign.py plan \
  --campaign-id a64-2026-06-4k \
  --input-list "$WORK/shards-4k/shard-input-list.txt" \
  --output "$WORK/plans/campaign-4k.json" \
  --local-temp-root "$WORK/local-plan-4k" \
  --minimum-local-free-gib 20 --guest-page-size 4096 \
  --expected-total-contracts "$TOTAL_4K" \
  --architecture-root "$ARCH_ROOT" --aslref "$ASLREF" \
  --bundler "$BUNDLER" --features-json "$FEATURES" \
  --closure-capture-directory "$EVIDENCE_4K" \
  --materialization-index "$WORK/join-4k/index.tsv" \
  --contract-manifest "$WORK/join-4k/contracts.tsv.gz" \
  --timeout-seconds 1800 --max-rss-kib 12582912 \
  --max-encoded-bytes 134217728 --max-artifact-bytes 536870912 \
  --max-artifact-files 4096 --max-log-bytes 8388608 \
  --maximum-authority-database-bytes 137438953472 \
  --maximum-authority-merge-scratch-bytes 17179869184
python3 -B tests/tooling/arm64/run_a64_aslref_campaign.py plan \
  --campaign-id a64-2026-06-16k \
  --input-list "$WORK/shards-16k/shard-input-list.txt" \
  --output "$WORK/plans/campaign-16k.json" \
  --local-temp-root "$WORK/local-plan-16k" \
  --minimum-local-free-gib 20 --guest-page-size 16384 \
  --expected-total-contracts "$TOTAL_16K" \
  --architecture-root "$ARCH_ROOT" --aslref "$ASLREF" \
  --bundler "$BUNDLER" --features-json "$FEATURES" \
  --closure-capture-directory "$EVIDENCE_16K" \
  --materialization-index "$WORK/join-16k/index.tsv" \
  --contract-manifest "$WORK/join-16k/contracts.tsv.gz" \
  --timeout-seconds 1800 --max-rss-kib 12582912 \
  --max-encoded-bytes 134217728 --max-artifact-bytes 536870912 \
  --max-artifact-files 4096 --max-log-bytes 8388608 \
  --maximum-authority-database-bytes 137438953472 \
  --maximum-authority-merge-scratch-bytes 17179869184

python3 -B tests/tooling/arm64/run_a64_aslref_campaign.py pilot \
  --plan "$WORK/plans/campaign-4k.json" --output-root "$WORK/run-4k" \
  --local-temp-root "$WORK/local-run-4k" --workers 4 \
  --maximum-authority-database-bytes 137438953472 \
  --maximum-authority-merge-scratch-bytes 17179869184 \
  --minimum-output-free-gib 20 --minimum-local-free-gib 20
python3 -B tests/tooling/arm64/run_a64_aslref_campaign.py pilot \
  --plan "$WORK/plans/campaign-16k.json" --output-root "$WORK/run-16k" \
  --local-temp-root "$WORK/local-run-16k" --workers 4 \
  --maximum-authority-database-bytes 137438953472 \
  --maximum-authority-merge-scratch-bytes 17179869184 \
  --minimum-output-free-gib 20 --minimum-local-free-gib 20

# These commands resume the four pilot shards and run every remaining shard.
python3 -B tests/tooling/arm64/run_a64_aslref_campaign.py run \
  --plan "$WORK/plans/campaign-4k.json" --output-root "$WORK/run-4k" \
  --local-temp-root "$WORK/local-run-4k" --workers 4 \
  --maximum-authority-database-bytes 137438953472 \
  --maximum-authority-merge-scratch-bytes 17179869184 \
  --minimum-output-free-gib 20 --minimum-local-free-gib 20
python3 -B tests/tooling/arm64/run_a64_aslref_campaign.py run \
  --plan "$WORK/plans/campaign-16k.json" --output-root "$WORK/run-16k" \
  --local-temp-root "$WORK/local-run-16k" --workers 4 \
  --maximum-authority-database-bytes 137438953472 \
  --maximum-authority-merge-scratch-bytes 17179869184 \
  --minimum-output-free-gib 20 --minimum-local-free-gib 20

python3 -B tests/tooling/arm64/run_a64_aslref_campaign.py verify \
  --plan "$WORK/plans/campaign-4k.json" --output-root "$WORK/run-4k" \
  --local-temp-root "$WORK/local-verify-4k" --minimum-local-free-gib 20 \
  --maximum-authority-database-bytes 137438953472 \
  --maximum-authority-merge-scratch-bytes 17179869184
python3 -B tests/tooling/arm64/run_a64_aslref_campaign.py verify \
  --plan "$WORK/plans/campaign-16k.json" --output-root "$WORK/run-16k" \
  --local-temp-root "$WORK/local-verify-16k" --minimum-local-free-gib 20 \
  --maximum-authority-database-bytes 137438953472 \
  --maximum-authority-merge-scratch-bytes 17179869184

python3 -B tests/tooling/arm64/join_a64_aslref_campaign_results.py \
  --campaign-4k-plan "$WORK/plans/campaign-4k.json" \
  --campaign-4k-root "$WORK/run-4k" \
  --campaign-16k-plan "$WORK/plans/campaign-16k.json" \
  --campaign-16k-root "$WORK/run-16k" \
  --local-temp-root "$WORK/local-promotion" --minimum-local-free-gib 20 \
  --maximum-database-bytes 137438953472 \
  --maximum-merge-scratch-bytes 17179869184 \
  --output "$WORK/promotion/a64_aslref_behavior_status.tsv"
python3 -B tests/tooling/arm64/join_a64_aslref_campaign_results.py \
  --campaign-4k-plan "$WORK/plans/campaign-4k.json" \
  --campaign-4k-root "$WORK/run-4k" \
  --campaign-16k-plan "$WORK/plans/campaign-16k.json" \
  --campaign-16k-root "$WORK/run-16k" \
  --local-temp-root "$WORK/local-promotion" --minimum-local-free-gib 20 \
  --maximum-database-bytes 137438953472 \
  --maximum-merge-scratch-bytes 17179869184 \
  --output "$WORK/promotion/a64_aslref_behavior_status.tsv" --verify-only
```

The first sharding pass derives the exact campaign counts from the freshly
joined authority; never substitute aggregate-row counts or an older run's raw
contract totals. The checked aggregate manifest currently has 32,169 rows
(16,084 at 4 KiB and 16,085 at 16 KiB); those row counts are not campaign
totals. The derived shard count
is `ceil(TOTAL/1024)`, and the pilot itself requires the first four shards to
contain exactly 1,024 contracts each.

Sharding and campaign publication use temporary files followed by rename.
Run/pilot recheck both reserves
before each submission and after every completion using the configured artifact,
file-count, and combined-log caps for every active worker. On interruption,
only exact owned `.a64-campaign-*` local workspaces and shard partials are
recovered; published `SUCCESS.json` trees are semantically verified and
retained during resume. Sharding reconstructs one producer at a time and
validates the completed semantic shard set. A probe using
`--generated-directory` rejects placement in any
source, input, or capture tree and checks its explicit
`--minimum-generated-free-gib` reserve plus the artifact hard cap before it
creates the directory. Promotion keeps per-input results in its size-capped,
locally rooted SQLite database,
streams indexed bindings one aggregate at a time, and verifies the complete
join before publishing or verifying its output TSV.
The 128 GiB database caps are deliberate: the bounded promotion scale fixture
measured 1,043 bytes per input at its peak after including the authority,
campaign-ID, result, and normalized per-shard tables. The capacity model rounds
to 1,536 bytes, allows 40 million inputs, and applies a 3/2 safety factor
(92,160,000,000 required bytes). The configured database and scratch caps are
enforced directly during the join.

`scripts/build-xcframework.sh` builds each supported slice, checks its platform,
architecture, public API and package shape, and links external C, C++, and
Objective-C consumers for macOS, iOS device, and iOS simulator. It does not
compare or attribute individual archive members.
Interpreter, gadgets, native-shadow mappings, and software-MMU storage remain
data-only: no W+X mapping and no RW/RX transition. Executable publication is
retained only in the explicitly selected JIT arena for entitled
debugger/jailbreak use.

The current refactor evidence index and the local raw-result locations are in
[`refactor-evidence.md`](refactor-evidence.md).

They require the permissions and host hardware described by their commands; do
not convert an unavailable GUI, network, physical device, or performance host
into a passing result.
