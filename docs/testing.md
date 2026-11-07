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

A tooling test's `TIMEOUT` is a hard bound sized from measurement, not from
the time the test takes alone: about four times its longest duration at gate
concurrency (`ctest -j10`) while the host load average was 40 to 80, taken up
to the next of 30, 60, 90, 120, 180, 240, 300, 450, 600, 900, 1200 or 1800 s.
A process-heavy test runs many times slower there than alone. A wait inside a
test for a process, file or thread the test started outlasts the same load,
stays below the test's bound, and is never retried or unbounded.

A test that runs a built tool, named by a `$<TARGET_FILE:...>` argument of its
command, requires a CTest fixture that builds the tool: a `<suite>-build-<tool>`
test of the suite of the first test that needs it. Running the test by name
after a configure is enough. The check workflows build the suite's binaries
first, and the fixture then finds nothing to build.

Tooling tests run tools against fixtures; they do not grep tracked source for
expected tokens or restate values that another file owns. A shell tooling
test's `$TMPDIR` is its `test-scratch/<test>` in the build tree, which
configure creates: a shell defers its traps while a child runs, so a shell
test that times out in one cannot clean up, and what it leaves names it.
Python tooling tests keep the host `$TMPDIR` for `sun_path`.

`repository-environment-portability` is the one tooling test that scans
tracked source, and it looks for forbidden values rather than expected ones:
host paths and account names, private addresses, ssh endpoints, a
`Claude-Session:` trailer line (patch export drops it, and no exception
allows it) and Apple
signing identifiers (a development team is never tracked; a bundle identifier
stays inside the project's own namespace), read from `project.pbxproj` and
JSON `project.xcproj` projects, xcconfig files, plists and CMake. It also
scans new unignored files, so a leak fails before it is committed. A site adds
its private tokens to a local denylist, found at
`${XDG_CONFIG_HOME:-$HOME/.config}/linux-um-darwin/portability-denylist` or
through `UML_PORTABILITY_DENYLIST`; the test runs with and without it. A
reviewed exception goes in
`tests/tooling/cmake/environment-portability-allow.tsv` under one of its fixed
reasons, and an entry that matches nothing fails.
`python3 tests/tooling/cmake/check_environment_portability.py --list-rules`
describes the rules and the denylist format.

`repository-license-headers` enforces the [licensing rule](../LICENSES/README.md)
by location. It reads the first lines of every tracked or new unignored source
file or script, skips patch files, and requires an SPDX header whose
identifier is allowed where the file lives, no deprecated identifier, and the
texts in `LICENSE` and `LICENSES/` for each identifier in use. A reviewed
exception goes in `tests/tooling/cmake/license-header-exceptions.tsv`, and an
entry that matches nothing fails. Its fixtures mutate a header, a text and an
exception; `python3 tests/tooling/cmake/check_license_headers.py --list-rules`
describes the locations.

Unit tests create scratch files through `tests/unit/scratch/`, never at a
host-global path unless that path is the production contract under test, as
for the management console's run directory. Each registration owns
`test-scratch/<test>` in the build tree; a binary run outside CTest uses
`$TMPDIR`. Unix-domain socket paths use `$TMPDIR` because a build-tree path
may not fit `sun_path`. Tests remove what they create, so a leftover file
names the test that leaked it. A test that needs a private directory holds a
`ScratchSubdirectory`, which removes it with its contents.

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

Unit tests assert with `require`, `requireSyscall`, `fail` and `requireThrows`
from `tests/unit/require/require.hpp`, which every unit binary links. A failure
prints the caller's `file:line` and its message, then aborts, so no `catch` in
the test or in the code under test can swallow it; `requireSyscall` also names
the errno of the failed call. A test that posts signals to one of its own
threads builds what it needs before the first signal, files and mappings
included, because a signal that reaches a thread inside a setup call can end
it with `EINTR`. The same directory holds the other shared helpers: `waitUntil`
for a bounded wait on a condition, `Watchdog` to end a hung binary with a
message, `runNamedTests` for a table of named cases, and `wire_bytes.hpp` to
build and read device records.

For one test, use an anchored expression:

```sh
ctest --test-dir .build/presets/macos-unit \
  -R '^exact-test-name$' --output-on-failure
```

## Canonical host workflows

Every workflow needs the tools in
[Host prerequisites](../dependencies/README.md#host-prerequisites), among them
Python 3.11 or newer, which configure requires, and a Git identity. A test
that boots ten vCPUs, such as `guest-mmu-stress`, needs ten host CPUs and
skips on a smaller host. A guest test states its vCPU count with `ncpus=`: the
launchers' default follows the
[applications' vCPU limit](architecture.md#memory-and-executable-code), which
depends on the host. The LTP campaigns default to half the host's physical
cores, at most 16.

The ordinary host check is:

```sh
cmake --workflow --preset check-unit
```

A change confined to a host tool usually needs only its owning workflow; for
tooling tests alone that is:

```sh
cmake --workflow --preset check-tooling
```

Specialized workflows are:

```sh
cmake --workflow --preset check-gpu
cmake --workflow --preset check-gpu-asan-ubsan
cmake --workflow --preset check-loader-asan-ubsan
```

The `unit-gpu` test preset's name filter is the one selection of GPU workflow
tests, and the other `unit-gpu*` test presets inherit it. It selects every
registered test named `gpu-*` (the two `gpu-guest-*` tooling tests among them),
and the Virtio battery, console, input, sound, video and video encoder
protocol tests, `video-encoder-capabilities`, `platform-video-decoder`, and the
hardware `platform-video-encoder`, `-recovery` and `-cache` tests that the GPU
configuration also builds. A
new GPU test joins the workflow by its name: `uml_add_gpu_unit_test` refuses a
name that does not start with `gpu-`. Configure derives the
`gpu-unit-test-binaries` target from that filter, so a selected test's binary
is built by every GPU build preset. Configure fails when a test preset that
inherits `unit-gpu` sets its own filter.

A GPU unit test process keeps Metal's shader cache in its scratch directory,
not in the `com.apple.metal` cache that every command-line process of the user
shares (`tests/unit/gpu/metal_shader_cache_isolation.cpp`). The first pipeline
a process compiles reads that cache's whole index, which a host that has run
guest GPU workloads leaves at tens of megabytes: a fraction of a second plain,
and minutes under AddressSanitizer, where every realloc copies. The private
cache starts empty and is removed at exit, together with the caches of
child processes that ended without running exit handlers, so a test costs the
same on any host and leaves the host's cache alone.

The GPU workflows need the `apple-gpu-vulkan-experiment` dependency prefix. A
change that crosses the Venus or KosmicKrisp boundary also runs
`check-integration`.

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
in Designed for iPad mode; the iOS Simulator renders on the CPU instead
([`graphics.md`](graphics.md#ios-simulator-software-renderer)), which suits UI
tests and no conformance or performance campaign. Kernarium's own suite carries
the narrowest check of the device provider: it creates a surfaceless OpenGL 4.6
core context in the application process and requires Zink on the packaged
KosmicKrisp driver, so a broken iOS graphics closure fails in seconds rather
than inside a guest.

A whole CTS campaign runs in that application through the opt-in
`apple-ios-mac-vk-gl-cts` lane. It needs Xcode's local install service and a
sealed rendered image, so it registers only when the image is named:

```sh
cmake --preset macos-integration \
    -DUML_IOS_MAC_CTS_ROOTFS=<image>.ext4 \
    -DUML_IOS_MAC_CTS_ROOTFS_RECEIPT=<image>.receipt.json \
    -DUML_IOS_MAC_CTS_CAMPAIGN=gl46-matrix-cfg00-xml01-001of032-zink-v1
ctest --test-dir .build/presets/macos-integration \
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
guest-session log is an XCTest path that a device launch does not take. The
launch sets `KERNARIUM_PAYLOAD_REMOVE=1`, so the runtime deletes that payload
once it has read it and the lane leaves nothing but the image behind:

```sh
cmake --preset macos-integration \
    -DUML_IOS_DEVICE_VK_CTS_DEVICE=<udid> \
    -DUML_IOS_DEVICE_VK_CTS_CAMPAIGN=vulkan-info-receipt-smoke-v1
ctest --test-dir .build/presets/macos-integration \
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
every advertised profile's pixels, raw layouts including packed BGRA capture
and 8-bit capture of a 10-bit stream, the rows of every raw format at the
padded pitch the device states (TRY_FMT and S_FMT against the rule at widths
that are and are not whole pitches, and the 120 x 90 window streams read row by
row), imported dma-buf capture, timestamps,
source-change and EOS events, the display order of H.264 and HEVC B-pictures
and a seek in both (over open-group streams for HEVC), reads the H.264 and HEVC
profile and level menus (Constrained High beside High, levels from 1.0 up to
5.2 and from 1 up to 6.2, the current level the highest), decodes flat portrait
and square H.264 and HEVC pictures (2304 x 4096, 4096 x 4096, 4320 x 8192) to
frames of their own size inside a frame range that is a rectangle, and rejects
kernel faults or failed shutdown. Imported capture covers the queue's own
buffers handed back and guest memory a client brings, with each buffer's planes
in separate dma-bufs or in one and their lengths stated as QUERYBUF gave them,
as the whole dma-buf, or not at all. `guest-v4l2-decode-gpu` runs the same probe
with the GPU, where capture buffers are host-visible blobs and an imported
dma-buf takes another path in the driver. A stream that changes its picture size
twice is decoded the way Android's V4L2 codec handles it, including a drain asked
for while the last change is still on its way: every picture of a size returns,
then the LAST buffer, before the source change is visible. A client that queues
the whole stream and drains straight away, with both changes inside its drain,
gets every picture of all three sizes before the one LAST buffer that ends the
drain. Every profile is also
started in v4l2_codec2's order, with a provisional 64 x 64 capture format (and
queued buffers for it) before the size is known, the parameter sets and the
picture as two coded buffers, and the capture format chosen only after the
source change: the buffer that declared the size is held until then, nothing
lands in the provisional buffers, and the picture arrives whole before the LAST
buffer. The coded queue's buffer size is read back for every coded format as
a client sees it (S_FMT and G_FMT, then the plane length REQBUFS allocates): the
tier of each picture size stated, and for a size beyond the format's range, as a
client that states none passes it, the tier of the largest picture it takes.
The large buffers are also created with the guest's free memory in
blocks of 32 KiB, none larger, the state a guest that has run for days is in:
a 124 MiB coded buffer, the most the device attaches, is created, exported and
imported again, and the 4096 x 4096 and 4320 x 8192 flat pictures are decoded
into mapped buffers. The scatter lists of these buffers are hundreds of
kilobytes, so a driver that asks the kernel for a list in one block fails them
with `page allocation failure: order:4`, as it did a 4K HEVC decode on a
1536 MiB Android guest. Android uses its own 4 KiB guest and deployment workflow.
The fixed x86_64 userspace product registers no CTest suites, so the x86_64
slice exercises its own V4L2 path by running `run_v4l2_decode.py`, with `--gpu`
for the GPU case, directly with an x86-64 probe and an x86-64 rootfs, the way
its boot check below is run.

`guest-video-error-answer` needs no probe or rootfs. Its arm64 guest has a
decoder whose device, the production one behind a test-only shim, answers the
capture buffers the guest queues as failed, and the test checks that the client
dequeues one flagged as an error, empty, and with the timestamp the device
named, a contract the encoder's driver shares.

The optional `guest-v4l2-encode` test qualifies the Virtio video encoder the
same way. Build `tests/integration/video/v4l2_encode_probe.cpp` as a static
C++20 executable for the guest architecture and configure
`UML_V4L2_ENCODE_PROBE_BINARY`. The guest boots with `--video` and
`--video-encoder`, and the probe finds the encoder and the decoder by their
card names. For every advertised coded format, profile and raw layout it
encodes generated pictures from mapped and imported dma-buf input, and checks
the key-frame flags, header buffers, timestamps, forced key frames, controls
refused while streaming, crop, drain with the last flag and end-of-stream
event, resumption, a restart after STREAMOFF without a drain, and a resolution
change. It also holds every control and raw format the guest registers
against the host table, so a control the driver dropped or a format listed
under another fourcc fails; encodes packed BGRA and RGBA pictures, whose red
and blue blocks show the channel order; encodes every raw layout at a width
that is not a whole pitch (342 x 120), where the rows lie at the padded pitch
that TRY_FMT and S_FMT both state; and checks the parameter sets that
`PREPEND_SPSPPS_TO_IDR` adds and the layers that hierarchical coding makes.
Colorimetry stated on the raw queue, including `V4L2_COLORSPACE_RAW` with a
transfer function and range, must read back as stated, survive a repeated
`S_FMT` and `S_PARM`, and appear in the H.264 stream's VUI; BT.2020 with PQ
and with HLG (`V4L2_XFER_FUNC_HLG`, the kernel line's local value 8), stated on
a P010 raw queue, must read back and show as transfer characteristics 16 and 18
in the HEVC Main 10 stream's VUI; the B-frame control
must run from zero in steps of one and the level menus default to their lowest
item, as the host describes them. Each stream is decoded back through the guest decoder and compared with the
pictures by luma PSNR. The x86_64 slice runs `run_v4l2_encode.py`
directly, like the decoder.

The optional `guest-v4l2-mmap-capture` test covers what that one cannot: with a
GPU the decoder's capture buffers are host-visible Virtio-GPU blobs, and an
allocation of them has to finish on an idle guest. Build
`tests/integration/video/v4l2_mmap_capture_probe.cpp` the same way and configure
`UML_V4L2_MMAP_CAPTURE_PROBE_BINARY`. The probe allocates and maps the buffers
and fails when that takes ten seconds or when the buffers are not blobs.

The optional `guest-futex-atomic` test checks the futex contract in
[`../kernel/common/README.md`](../kernel/common/README.md). Configure
`UML_FUTEX_PROBE_BINARY` with a static C++20 build of
`tests/integration/futex/futex_atomic_probe.cpp`; the rootfs needs the video
probe's shell tools. Run it on the Linux guest and the maintained Android
kernel.

The optional `guest-gbm-dmabuf` test checks the virtio-gpu dma-buf contract in
[`../kernel/common/README.md`](../kernel/common/README.md). Build
`tests/integration/gpu/gbm_dmabuf_probe.cpp` as a dynamically linked C++20
executable for the guest architecture (`c++ -std=c++20 -O2 -ldl`) and configure
`UML_GBM_DMABUF_PROBE_BINARY`. The guest boots with the GPU, and the probe opens
the render node through the image's own libgbm and libEGL. It requires an
export that asked for no write access to be read-write, a GPU write to be
visible through the dma-buf mapping after `DMA_BUF_IOCTL_SYNC` (twice), and a
CPU write through it, bracketed the same way, to be what the GPU reads (twice).
The x86_64 slice registers no CTest suites, so it runs `run_gpu_guest.py`
directly with an x86-64 probe and an x86-64 image that carries Mesa, naming the
test's `--command` lines and kernel arguments.

The optional `guest-drm-vblank` test checks that Virtio-GPU paces its display
as the vblank contract in
[`../kernel/common/README.md`](../kernel/common/README.md) describes. Build
`tests/integration/gpu/drm_flip_probe.cpp` as a static C++20 executable for the
guest architecture and configure `UML_DRM_FLIP_PROBE_BINARY`. The guest boots
with the GPU; the probe sets the first output's preferred mode, waits for 30
vblanks and flips two buffers for two seconds, and requires both rates to be
within 10% of the mode's refresh rate. The x86_64 slice runs
`run_gpu_guest.py` directly with an x86-64 probe, naming the test's `--command`
lines and kernel arguments.

`guest-aarch32-kernel` checks the kernel's AArch32 task support in
[`../kernel/common/README.md`](../kernel/common/README.md), and needs `python3`,
`as` and `ld` in the rootfs. The build generates a freestanding
armhf program from `tests/integration/aarch32/aarch32_probe.S` with the
configured compiler's ARM backend, wraps it into an ELF32 executable with
`make_elf32.py` (the toolchain has no ARM linker), aligned to 64 KiB and aligned
to 4 KiB only, and never commits either. The test requires unchanged 64-bit
`uname`, `/proc/cpuinfo`, `vm.mmap_rnd_compat_bits` and `abi.cp15_barrier`, and
that `abi.swp` is 0, makes SWP end the program in SIGILL, then sets it to 1 and
runs the program's SWP and SWPB checks, and runs the program (kuser helpers, signals and VFP state, `cacheflush`, Thumb,
`mmap2`, `PER_LINUX32`
views, restarts and exec between ABIs; threads through `clone` and `clone3`,
fault signals and stack growth, the time64 and socket calls with
`SCM_RIGHTS`, `vfork`, timers, `seccomp` and a 32-bit tracer), a 64-bit
tracer's session (registers, system call and single-step stops, signal-frame
VFP state), a core dump and many runs at once, and requires the 4 KiB-only
program to run on the guest. Last, the guest assembles `aarch64_dirty.s`, a
64-bit program that leaves x15-x30 set, and the program runs every exclusive
and ordered transfer on each CPU right after it, through its execve and
between its `sched_yield` calls; the same two programs trade tokens through a
shared page, the armhf one with SWP and SWPB and the 64-bit one with
load/store-exclusive pairs, and every token must remain. `guest-aarch32-exclusives-gadgets` and
`-jit` run that check alone on four vCPUs of each engine. The tooling test
`aarch32-elf32-wrapper` checks the two layouts the wrapper writes, and
`aarch32-elf32-linker` the multi-object linker of the engine probe below.

`UML_TEST_APPLE_AUDIO=ON` enables `apple-audio-playback`, which sends a brief,
low-volume stereo tone through the real macOS output and verifies completion
and restart. It requires access to macOS audio services. It also enables
`apple-audio-interruption`, which builds the playback sink for the iOS
Simulator, plays a stream there and posts the audio-session notifications iOS
posts when another app interrupts it (the Simulator never does), with hooks on
the session's activation and its hint about other apps' audio and on the
player's scheduling: playback must stop while interrupted without slowing the
guest's clock, resume when the system advises resuming or, with no advice or
notification, when no other app plays, never take the session from an app that
does, keep completing periods through a slow session activation, keep an
interruption across the guest closing, resetting and reopening the stream, and
never wedge. It also drives the Virtio Sound device the way the Linux driver
does and checks that the guest's periods complete at the rate of audio in every
state the sink can be in. It also starts the Apple microphone over hooked
AVFAudio, with no real input and no permission prompt, and checks that an
interruption stops it and releases the session, that the system's advice to
resume starts it again, and that a recommendation not to resume starts it only
when no other app plays. It skips when no iOS Simulator device at the
deployment target exists.
`virtio-sound-microphone` runs the rest of the microphone over fakes, and the
real input is checked by hand (see the Microphone section of the audio README)
because granting microphone access is the user's act.

The full sanitizer suites remain separate because of their runtime cost:

```sh
cmake --preset macos-unit-asan-ubsan
cmake --build --preset asan-ubsan
ctest --preset unit-asan-ubsan

cmake --preset macos-unit-tsan
cmake --build --preset tsan
ctest --preset unit-tsan
```

Every sanitizer preset, these two and the GPU and embedded-loader ones,
instruments Objective-C++ as well as C++.

Both sanitizers keep the product's macOS native window for themselves, and a
process reserves its window once, so a sanitized test, and every run it makes
through the emulator test variant, takes a native window that both leave free
(`tests/unit/memory/native_window_test_support.hpp`; the test variant places
its runs there by default). AddressSanitizer also ignores `mlock()`, so under
it the cases that need wired guest RAM report themselves unsupported instead
of running.

A guest that restarts itself boots again in the same process, and
`runtime-restart-*` (one process per mode, `sequential_run_tests.cpp`) covers
it with a test kernel that leaves behind what a real one does: descriptors,
pthread keys, allocations and mappings through the imports the loader follows,
changed signal state, a guest RAM root, kernel threads and a guest file. At the
moment each of 64 hand-backs completes the test checks that none of it is left
and that the process is as it was before the first run; the other modes cover
the ends that stay spent, another handle, a host service that borrows guest RAM
and a kernel thread that still runs, and a stop or a resume that is inside the
hibernation's abort request while the restart drops the hibernation
(`abort-stop`, `abort-resume`). The default malloc zone keeps what a boot freed,
so the test gates the heap's in-use bytes, not its size; under a sanitizer it
reads the sanitizer allocator's live bytes.

The gadget cache census
([`tests/perf/README.md`](../tests/perf/README.md#cache-census)) compiles
diagnostic code into both gadget engines. A change to either gadget cache or
to the census runs:

```sh
cmake --workflow --preset check-unit-census
```

It builds the unit suite with `UML_GADGET_CACHE_CENSUS=ON` and runs every unit
test with the census writing to `/dev/null`, every stream sampled and a
snapshot due each second, so a census build must give a default build's
results, under failed allocations too.

## Integration prerequisites and workflows

Integration needs four local inputs:

- the accepted kernel bundle below `.build/kernel/artifacts`;
- the accepted boot base archive below `.build/rootfs`, which `uml-darwin`
  embeds ([boot ramdisk](../tools/rootfs/boot-ramdisk/README.md));
- the verified dependency prefix; and
- the unified `rootfs.erofs` image.

It does not require a kernel source checkout or compiler. Run:

```sh
cmake --workflow --preset check-integration
```

The workflow configures the product, builds it, runs tooling checks, and
executes the fast integration suite. `integration-full` is the explicit CTest
preset when the complete registered guest set is required.

The shared serial harness boots each guest through the launcher's per-run
initramfs: its `/init` mounts the image beneath a tmpfs overlay, installs the
files a test needs as payload files, and starts the
[harness session](../tools/rootfs/boot-ramdisk/README.md#harness-session),
which prepares the requested runtime mounts, network, workspace, swap and
cgroups and gives the harness a root shell. The console carries commands and
their output, never files. The harness sends commands through a nonblocking
pipe and keeps PTY output. This keeps host input pollable when every guest CPU
is busy while preserving terminal output behavior and bounded writes under
backpressure. The emulator stays in the harness's session and process group,
so a signal to that group, such as a timeout's, a Ctrl-C or CTest's, reaches
the guest. A watchdog in the same group kills the guest when the harness dies
alone without closing it, so no way of ending a harness leaves its guest
running.

`ctest -N -L integration` lists the registered guest tests. Each runner or
guest probe states in its header what it checks, and a contract's owner, such
as [`../kernel/common/README.md`](../kernel/common/README.md), names the tests
that check it.

A behaviour that one program checks inside the guest runs through
`tests/integration/support/run_guest_probe.py`: a `.py` probe runs under
`python3`, and a `.cpp` probe is built in the guest with the registration's
`--cxxflags`. The probe's exit status is its failure count, and the test also
fails on a console line that the registration's `--console-failure` pattern
matches.

A guest that ends behind a closed GPU request gate must still be able to stop:
`gpu-service-fd-ownership` completes the requests of a held gate with their
fences when the guest-end registry reports the guest's end, runs none of them,
and holds the next guest plainly; `runtime-guest-shutdown` covers the report
that reaches the registry once per new report.

The Virtio vsock device has its unit tests without a guest: `virtio-vsock-device`
drives it through its queues with synthetic packets and real socketpairs (the
open, refusal and timeout paths, stalls in both directions, half-closes, resets,
the 32-connection limit, the never-defer and keep-on-disabled-ring rules, and an
echo run of several threads opening and closing at once), and
`virtio-vsock-connection-table` holds the credit arithmetic, its 32-bit wrap
and the host port allocator. `vsock-forwarder` covers the `--vsock-forward`
grammar, which accepts a loopback address only, and the listeners that attach
each accepted connection to a guest port. `kernarium-adb-channel` covers the
kernel arguments that carry an Android guest's adb key and the request that adbd
not listen on TCP, and the limit on the whole kernel command line.

`guest-vsock-stream` (integration) runs the device against a Linux guest's
driver through the launcher's loopback forward, with a Python `AF_VSOCK` server
in the guest: 64 MiB each way, four concurrent streams, a stalled reader at
either end (the sender must be held back and a second stream must stay
responsive), half-closes in both directions, a refused port, a suspend and
resume in the middle of a stream, and a guest restart, which every open stream
must read as its end. Every body is checked by SHA-256 and each direction's
throughput is printed. `tests/integration/android/run_guest_android_adb_vsock.py`
is the opt-in Android counterpart: it boots the accepted release with a vsock
forward to adbd and drives the stock `adb` through it (`connect`, a pulled
system APK installed with `install -r` and restored with `pm uninstall`, and
`bugreport`), then checks that the TCP forward answers beside it. It needs the
`macos-android17` executable, an installed release that carries the vsock
kernel, and `adb`.

`guest-restart` runs the macOS launcher through a guest that restarts itself.
Under each of the gadgets and the JIT engine, one `uml-darwin` process boots,
restarts twice and powers off, with the same pid throughout. The test loads a
process audit (`tests/integration/restart/process_audit.cpp`) into the launcher
and, at the settle point of each boot, compares what it lists with boot 1's:
descriptors by kind, threads by name, Mach ports, the next pthread key, signal
dispositions and the flags of descriptors 0 to 2. A pattern the first boot
writes into anonymous memory and tmpfs must be found in guest RAM before the
restart and nowhere in the process, window included, after it. A second phase
runs a guest that restarts at once and checks that the launcher ends the loop
with exit status 3 after five early restarts. `emulator-restart-loop-guard`
covers the guard's policy and its escaping of the guest's command, and
`slirp-network-reset-peer` covers a SLiRP connection whose peer resets while
the guest has data to send, which must not end the process.
`android-partition-boot-selection` covers the consumption of a
`bootonce-bootloader` command and the boot a restart selects.

`slirp-network-dns` covers the guest's DNS through the system resolver and the
dropped guest multicast, with no network: the resolver is confined to local
answers (`localhost`), and the test fails if SLiRP opens any host Internet
socket for a guest's DNS or multicast. `network-dns-rules` is the unit test of
the wire rules. The probe also runs in the iOS Simulator, where the container's
tmp is too long for a Unix socket path: configure `ios-simulator` with
`-DUML_BUILD_NETWORK_PROVIDER=ON -DUML_BUILD_TOOLING_TESTS=ON`, build
`uml_slirp_dns_tests`, and start its `.app` executable with
`xcrun simctl spawn <booted device>`.

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
[x86-64 LTP rootfs](../tools/rootfs/ltp-x86_64/README.md); the tooling test
`x86-64-guest-runners` checks their options, refusals and probe compiler
commands without a guest. That `uml-darwin`
embeds the x86-64 boot base, so
`tests/integration/boot/run_guest_boot_initramfs.py --engine gadgets` checks it
with the x86-64 base and a `uml-initramfs`.
`tests/integration/x86_64/run_guest_x86_64_rflags.py` and
`run_guest_x86_64_exceptions.py` cross-build freestanding x86-64 and i386
probes with `clang++` and `ld.lld` and check the slice's user-RFLAGS and
exception contracts in the [mainline README](../kernel/mainline-6.18/README.md)
against native x86 Linux's values. The smoke, RFLAGS and exception runners
take `--engine jit` to run x86-64 and i386 code on the macOS-only x86-64 JIT
instead of the default gadget engine, and `UML_TEST_JIT_TIER=N` runs the JIT
as the [tiered engine](../darwin/cpu/x86_64/jit/README.md#tiered-execution),
except that the checks of which dispatches the JIT ran need it from the first
instruction, since a tiered space may leave before it reaches the JIT; run
them on both engines when a change reaches either.
`run_guest_x86_64_mm_shared.py`, which takes the same options, checks that
the host sees an address space as shared exactly when the kernel's contract in
the [common kernel README](../kernel/common/README.md) says so, including
when the sharing ends, and on the JIT that exactly the marked dispatches run
ordered code.
`run_guest_x86_64_fork_cow.py` forks from a busy multi-threaded process on
four vCPUs, as glibc's fork() does, and checks that the workers keep their
counts and that the parent's releases of the locks it held across the fork
reach its copy. `run_guest_x86_64_code_change.py`, which takes the same
options, cross-builds a probe that changes the code of functions an engine has
translated by stores, mprotect round trips, forks, remaps, another vCPU and a
flood of changes that makes the engine drop everything, and requires every
later call to run the new bytes. `run_guest_x86_64_tso_litmus.py` runs x86
memory-order litmus tests on four vCPUs, between threads and between
processes over shared memory, in both modes, and in cycles that share, stop
sharing and share again, whose log must show the space marked and ordered
exactly while shared. Its negative control is the unit test
`x86-64-jit-store-order`, which shows message passing catching a JIT space
that keeps no order. Both engines pass every exception case, unmasked `#XM`,
`#MF` and `#AC`, selector loads, the LDT, segment limits and far transfers
included, in both modes.
`run_guest_x86_64_fenv.py`, which takes the same options, builds
`fenv_probe.cpp` with the image's g++, so it needs an image with a toolchain
such as the x86-64 LTP rootfs, and checks what glibc's `fetestexcept`,
STMXCSR and `fadd` show of the sticky SIMD exception flags against an Intel
Xeon's, on either engine. `run_guest_x86_64_vnni.py`, which takes the same
options, runs a freestanding probe that dispatches on CPUID leaf 7 subleaf 1
the way ggml and oneDNN do and requires the AVX-VNNI path, with the AVX2 and
AVX-VNNI forms of its kernels agreeing exactly, and requires `avx_vnni` in the
guest's `/proc/cpuinfo`. The slice's SMBIOS tables and
thread names, which `guest-kernel-dmi` and `guest-kernel-thread-names` check on
arm64, are checked by running `tests/integration/support/run_guest_probe.py
--probe tests/integration/kernel/dmi_probe.py --ncpus 2 --probe-timeout 120` and
`tests/integration/kernel/run_guest_thread_names.py` the same way.

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

Kernarium has runtime presets for Linux and Android on the iOS device
(`kernarium-ios-device*`) and the same on the iOS Simulator
(`kernarium-ios-simulator*`), whose GPU product is the [software
renderer](graphics.md#ios-simulator-software-renderer).
`Kernarium/scripts/package-ios-frameworks.sh` packages all selected runtime and
graphics slices plus the static GPT fdisk and decode-only zstd XCFrameworks for
Xcode. `--sdk device` (the default) and `--sdk simulator` choose the slices, and
both together give XCFrameworks with an `ios-arm64` and an `ios-arm64-simulator`
slice. No Simulator product is built for x86_64: both Xcode projects exclude
that architecture for the Simulator SDK, a concrete Simulator destination
(`-destination 'platform=iOS Simulator,name=iPad Pro 11-inch (M5)'`) builds that
device's arm64 alone, and a generic Simulator destination also builds the Swift
packages for x86_64 unless `EXCLUDED_ARCHS=x86_64` is on the `xcodebuild`
command line, which reaches package targets that a project setting does not.
KosmicKrisp has a device slice alone, so the app targets embed it with a
Run Script phase, `Kernarium/scripts/embed-device-framework.sh`, which embeds it
for an `iphoneos` build and nothing for the Simulator, and leaves an unchanged
embedded framework untouched so the application's seal stays valid
(`kernarium-device-framework-embed`). It also packages the
Apple battery provider's archive and the video encoder's, as the Android
runtime links them, as `HostBattery.xcframework` and
`HostVideoEncoder.xcframework`, with a slice for each packaged SDK; only
`KernariumTests` links them, so the test process reaches each without loading a
second runtime.

Both projects sign with the team in the ignored
`GuestHost/Config/Signing.local.xcconfig` ([setup](../GuestHost/README.md#signing)).
Without it a Simulator build works unsigned and a device build stops asking for
a development team.

The Simulator renders with llvmpipe, which runs one rasterizer thread per host
core; `LP_NUM_THREADS` caps them for a launch (`SIMCTL_CHILD_LP_NUM_THREADS=2`
with `xcrun simctl launch`) on a loaded host, at the cost of frame rate.
`dependencies/build-ios-storage.sh --verify-only` and
`dependencies/build-ios-zstd.sh --verify-only` check both arm64 Apple slices,
their provenance and deployment targets, required APIs, and the absence of
unused command-line providers. Apple builds
validate the kernel bridge ABI, page profile, product family, and `6.18` series
before embedding an artifact; they do not bind to its patch release or source
commit and never prepare sources or invoke a kernel or Android build. Rootfs bundles
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
launches leave the variable unset and publish no host port. A device driver
that cannot answer the first-connection "Allow USB debugging" prompt launches
the automation Android runtime
([`Kernarium/README.md`](../Kernarium/README.md#automation-runs)) with
`KERNARIUM_ANDROID_ADB_AUTH=0` and a loopback forward, and reaches the forward
over USB with `iproxy`.

On a connected iOS device that driver is
[`tests/perf/run_ios_device_android.py`](../tests/perf/README.md): Android runs
on a device go through Andarium
([`Andarium/README.md`](../Andarium/README.md#automation)), and Kernarium's
device runners (`run_ios_device_workload.py`, `run_ios_device_discard_bench.py`
and the device CTS lane) keep Linux. Both share
`tests/integration/support/ios_device.py`, and its tooling tests
(`andarium-android-runner`, `andarium-device-support`) run the runner against a
fake `devicectl`, `iproxy`, `adb` and `afcclient`, so no device is needed.

Android recovery qualification uses a fresh disposable bundle and sets
`TEST_RUNNER_KERNARIUM_ANDROID_BOOT_MODE=recovery`. Verify the LineageOS
recovery UI, touch input, ADB, `adb sideload` of a signed package, fixed-GPT
partition discovery, read-only payload mounts, userdata/metadata mount and
factory reset, battery display, and normal/recovery reboot paths. Never run
wipe checks against an existing user bundle.

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

`KernariumTests` and `KernariumUITests` also run in the iOS Simulator, for UI
tests of the application, through `Kernarium/scripts/run-ios-simulator-tests.sh`
after the Simulator slices are packaged. The script turns on the Simulator's
application accessibility preference, which the keyboard tests need for the
reason the Mac fails them, and links the accepted KDE desktop image into the
container, for `anEndedRunIsNeverStartedAgain`, which needs a guest to boot,
and so that the application's runtime is the Linux one `HostGraphicsStackTests`
select.
`HostVideoEncoderTests` skip there, as the Simulator has no hardware video
encoder, and `HostGraphicsStackTests` states the software renderer's contract
(llvmpipe, no Vulkan driver) in place of Zink on KosmicKrisp.
`KernariumAutomation` is the iPad's automation host and builds for the device
alone.

`kernarium-device-framework-symbols` (tooling) checks that the device-only
embed script places its dSYM beside the app's symbols for an archive, refuses
missing symbols, and leaves Simulator builds alone. The GPU prefix verifier
also rejects supplied dSYMs whose UUIDs differ from their frameworks.

### Andarium

`andarium-core` (unit, macOS hosts) runs the tests of the Swift package
`Andarium/AndariumCore`, the application's model and its adb client, with
`swift test`: the adb client runs against a scripted adbd on a socket pair
(authorization off, on and rejected, streams, shell v2, sync, a bug report,
timeouts on a clock the test moves, a cancel from another thread, the end of
a stream announced right after its last bytes), its key
encoder against moduli with the text `adb keygen` wrote for them, and the
listener of ADB over Network on loopback against a fake guest (bytes both
ways, backpressure each way, half close, a failed stream, the limit of
computers, stopping, a port in use).
The package also tests orientation ownership for phones, full-screen and
windowed iPads, loss of a scene lock, coalesced guest commands, partial failures
and stale completions after a guest restart. `SessionOrientationHostingTests`
checks the real scene root's session lock and the built bundle's portrait-only
iPhone declaration. The opt-in `RotationOverVsockTests`
(`TEST_RUNNER_ANDARIUM_ROTATION_TESTS=1`, selecting only that suite) boots Android
and verifies that the host override preserves and restores locked and unset
guest rotation preferences. A hands-on iPad check additionally covers rotation
with Control Center's lock off and transitions between a full-screen scene and
a window (`TEST_RUNNER_ANDARIUM_ROTATION_VISUAL=1`, selecting
`RotationOverVsockTests`); pure policy tests do not establish the visible result.
`andarium-motion-protocol` (unit) decodes independent motion wire fixtures
and rejects truncated records, unknown sensor types and non-finite values.
The core package also checks SI conversion, the saved choice, socket delivery,
stop during connection and stale callbacks. Hosted
`AndroidOperationTests` checks that the motion toggle persists and reaches
the runtime driver. Physical sensor delivery requires a sensor-capable iOS
device and a guest image carrying the motion sub-HAL; Simulator and Mac tests
do not establish that path. The opt-in `MotionOverVsockTests` drives the real
model and runtime on the iPad and reads both sensors through Android's public
SensorManager API in a foreground probe. Its automated mode checks sustained
delivery and off/on/off; its separate `ANDARIUM_MOTION_INTERACTIVE=1` mode checks
physical tilt and rotation ([probe and invocation](../tests/integration/android/motion/README.md)).
`andarium-stage-system-image` (tooling) covers the system-image staging script
and the option checks of `package-ios-frameworks.sh --runtimes android`. The
application itself builds with Xcode: `AndariumPreview` is its Simulator build
with mock data, and `Andarium` is the device application
([`Andarium/README.md`](../Andarium/README.md#targets)). `AndariumTests` is
hosted in `Andarium` and runs on an iPad, in the iOS Simulator and, as an iPad
app, on an Apple silicon Mac (`xcodebuild test -scheme Andarium`); its
`MultitouchTests` cover independent finger paths and slots, cancellation,
viewport crossings, the ten-contact limit and releasing all contacts. The host
`virtio-input-protocol` test covers the advertised multitouch axes, tracking-ID
lifetimes and wraparound, and legacy primary-finger coordinates. Its
`UpdateFlowTests` drive
the update, interrupted-update (Start retries, Restore, Erase) and blocked-start flows
against a stub runtime, including a clean shutdown returning Home with a fresh
session and a failed run refusing another Start. `ShutdownFlowTests` runs the
production views and runtime with `TEST_RUNNER_ANDARIUM_SHUTDOWN_TESTS=1` and
`TEST_RUNNER_ANDARIUM_GUEST_INSTANCE=shutdown-flow`, without `ANDARIUM_START`:
two clean power-offs each return Home, three boots reuse the app process, and a
host-stopped third run stays failed until relaunch. It deletes only that isolated
test guest.

The hosted tests share the application's one key window, and Swift Testing
runs suites in parallel, each yielding wherever it awaits. A test that replaces
the window's root view controller or connects a window of its own takes the
window with `HostWindow.take()` (`Andarium/AndariumTests/HostWindow.swift`) for
its whole body and gives it back with `defer`, so no test waits on a scene
lock that another test's root has taken away. A test never opens the system's
Settings or any other app: the scene applies an orientation lock only while
the application is active.

Run `ShutdownFlowTests/cleanShutdownMemorySnapshots()` alone with
`TEST_RUNNER_ANDARIUM_MEMORY_SNAPSHOTS=1` and the same isolated guest setting,
without `ANDARIUM_START`, for five clean boot/power-off cycles. It saves allocation
totals, thread names and VM-region metadata (no memory contents) to the app's
`tmp/ShutdownMemory/` before boot, while running, and immediately and ten seconds
after each return Home. Each settled snapshot requires zero wired memory and
zero dirty or swapped pages in the guest-domain and scratch heaps; the retired
guest display must also deallocate. The initial thirty-second pause allows
Instruments Allocations and VM Tracker to attach. Collect the snapshots from the
app container afterward. Compare live allocations across cycles: the runtime's
address reservations, signed code, framework caches and malloc's freed capacity
may remain in the process. `RuntimeDisplayLifetimeTests` also checks that a late
layout cannot retain a retired display while Home is showing.

The opt-in `RetinaMemoryTests` compares repeated system-app launches at Retina
and point resolution at the guest memory and vCPU count the launch states,
which every snapshot records. Its optional full-resolution
WebGL Aquarium workload records actual renderer, frames and FPS alongside
process, guest and GPU memory. See the [WebGL workload](../tests/integration/android/webgl/README.md)
for invocation and snapshot collection.

Its `AndroidOperationTests` and
`AdbOverNetworkModelTests` play Create Bug Report, Install APK and ADB over
Network the same way. `AdbOverVsockTests` drive a real Android guest with the
application's own client and authorization on (`TEST_RUNNER_ANDARIUM_ADB_TESTS=1`
with `TEST_RUNNER_ANDARIUM_START=normal`): a shell command, the multitouch
features Android advertises to applications, two runtime touch
contacts reaching Android's input reader through movement and independent
release, the USB debugging setting and the channel with it on and off, a
computer's connection through the
bridge, a bug report (a zip above 1 MB, and its time), a cancel, a system APK
pulled from the image and installed as an update (and its time), the image's
key rule (the application's key accepted whatever the debugging setting; a
foreign key refused with no prompt and nothing queued while debugging is off,
and prompting while it is on), and the key after Erase Guest. The tests read the
key the application made for the boot, again on every attempt until adbd accepts
it, and never make one: a key made before an `ANDARIUM_ERASE=1` launch's erase
deletes it would be one the guest was not given. `AdbOverVsockTests/aPromptWhoseDialogDiedIsRaisedAgain()` kills
SystemUI while the dialog is up and waits for the dialog to come back without a new offer: stock Android leaves
adbd's prompt pending, so it needs an image whose framework starts the dialog again and runs only with
`TEST_RUNNER_ANDARIUM_ADB_REARM_TESTS=1`. `aPromptWhoseDialogIsUpIsNotRaisedTwice()` holds a dialog
up for 50 s and requires that the framework's look leaves it alone (one activity record throughout).

`AdbOverVsockTests/androidReportsExactStorageSizes()` is a standalone storage
run with `TEST_RUNNER_ANDARIUM_STORAGE_TESTS=1` in addition to the ADB and normal
start settings. Build `AndariumStorageTests`
([`tests/integration/android/storage`](../tests/integration/android/storage/README.md))
as an explicit Soong module, which signs it with the AOSP test key (never a release
key), and copy it into the application's `tmp/AndariumStorageTests.apk`
before running only that method. It installs the APK through the application's
client, sets the guest's `hidden_api_policy` to 1 for the run (the test calls one
hidden API) and restores it, runs `StorageAccountingTest#exactFilesystemBytes`,
which checks both primary-storage helpers and `StorageStatsManager` against the
mounted filesystem sizes, removes the APK and powers Android off cleanly.
With the same ADB, storage and normal-start settings,
`AdbOverVsockTests/storageSettingsShowNonnegativeUsage()` opens Android's internal
Storage settings, waits for the summary, checks for negative displayed values
while scrolling through the category rows, saves PNG screenshots and UI hierarchies
to the application's `tmp/StorageSettings/`, and powers Android off. Run this method
alone and collect that directory from the app container afterward.

With `TEST_RUNNER_ANDARIUM_ADB_NET_HOLD_SECONDS` one more test holds the bridge up for
a real adb on the Mac, in a Mac run or in an iPad run (`platform=iOS,id=<udid>`
with `test-without-building`): it prints the address, and `tmp/adbkey` in the
application's container is the key to give the Mac's adb (`devicectl device copy
from` on an iPad). Give that adb its own server port and key home.
`StatisticsLoopTests` drive the statistics loop against a scripted runtime with
the pause between readings held by the test: no reading without a viewer, the
quarter-second then one-second cadence with the memory after every other
reading, the loop stopping with the foreground, with a guest that is not
running and with its last viewer, a memory the kernel does not give keeping the
last figure, and a new boot starting the figures over. `ConsoleReport`, the
harness's console lines, is in the application and tested there
(`ConsoleReportTests`); `StatsMeter` and `StatsSampling` are in
`andarium-core`. A Mac launch of the guest
itself uses the same command with `-only-testing:AndariumTests/AutomationHoldTests`,
`TEST_RUNNER_ANDARIUM_START` and `TEST_RUNNER_ANDARIUM_HOLD_SECONDS`
([`Andarium/README.md`](../Andarium/README.md#automation)). macOS asks once, per
bundle ID, to let the application read a removable volume when its derived data
is on one, and the launch hangs ("the test runner hung before establishing
connection") until a person answers; build to the internal disk instead.

`ios-privacy-manifests` (tooling) covers `scripts/check-privacy-manifests.py`,
which fails when a built application or framework uses a required-reason API
(found in its undefined symbols and Objective-C selectors) that its
`PrivacyInfo.xcprivacy` does not declare, or declares tracking, collected data
or a reason code Apple does not define, or has an Info.plist with an empty
version or no `MinimumOSVersion`. `package-ios-frameworks.sh` runs it on the
device slice of every framework it packages (the manifests are
`Kernarium/PrivacyManifests`), and it runs on any built bundle:
`python3 scripts/check-privacy-manifests.py Andarium.app`. Its fixtures are
small libraries compiled with the iOS SDK.

`andarium-app-store-bundle` (tooling) covers
`Andarium/scripts/check-app-store-bundle.py`, the last build phase of the
AppStore configuration (`Andarium/README.md#configurations`) and the check of
`package-ios-frameworks.sh --app-store`: Info.plist keys,
no environment knob, harness console line or debugging and
automation support (the frame mirror, console input, the guest sampler, the
footprint log, the activity logs and the automation entries; the symbols and
log text are listed in the script) in host code, no test bundle and no XCTest
or Testing framework in the application, no trace
of the build host in an executable (the linker's debug map of a framework,
which `strip -S` removes, and a path under `/Users/<name>/`,
`/Volumes/<name>/` or `/var/folders/`, found outside the strings of the debug
map, which Xcode strips from the application's own executable when it
archives), and the privacy check. Its fixtures are compiled with the iOS SDK
too. The configuration itself is
validated by building it unsigned (`CODE_SIGNING_ALLOWED=NO`).

`andarium-acknowledgements` (tooling) checks that the Acknowledgements
resource, `Andarium/Andarium/Shared/Acknowledgements.json`, names a project,
its copyright holders, its license and a license text for every framework the
Andarium project embeds and every component of the iOS device prefixes'
contracts (`dependencies/platforms`), on the checked-in resource and on copies
with one entry removed. The resource is generated by
`Andarium/scripts/acknowledgements.py` from the verified dependency sources and
the kernel's license files; `--verify` checks it against them, which needs
those sources, and the test does not. Hosted `AcknowledgementsTests` decodes the
bundled resource.

### UI automation on a device

Unattended device automation goes through the long-lived
`KernariumAutomation` UI-test host, because each new XCTest session on a
device with a passcode needs the owner's approval at the device. Its commands,
approval rules, and failure modes are in
`Kernarium/scripts/ipad-automation.py --help`. Keep all automation code in the
test targets.

### Window layout (device only)

Window layout follows iPadOS windowing, which only a device has. Check it on
an iPad whenever the layout of the guest view changes, under an iPad lease
with the automation host and a guest running, in Full Screen Apps, Windowed
Apps, and Stage Manager. The guest view's frame in the application's tree
starts below the window controls (`window-controls:com.imxieyi.Kernarium` in
SpringBoard's tree) and at the safe area where there are none. Change
Multitasking under the running application with the Settings identifiers
`com.apple.settings.multitaskingAndGestures.{fullScreenApps,windowedApps,
stageManager}`, in both directions: the view moves at once, and Full Screen
Apps gives the frame of a fresh launch. A zoomed window, the tiles and Slide
Over of its Window menu resize the guest display: Android's `wm size` follows
the view, a tap's raw touchscreen coordinates map to it (the range is fixed),
and the console shows no lost GPU backend. Restore Full Screen Apps and check
that its option is selected before releasing the lease.

### Background lifecycle (device only)

What the application does as iOS backgrounds it cannot be tested on a host:
only a device revokes the GPU, freezes the process, and reclaims it. Check it
on an iPad whenever the lifecycle handlers, the GPU hold, or the quiesce
change, under an iPad lease with the automation host above and the
application's console (`xcrun devicectl device process launch --console`).
Background the application by activating another app, wait, and activate
Kernarium again, for a KDE guest and for an Android guest, at about 20 s and
two minutes. Each cycle must show, in order:

- `guest GPU requests held on background (1)`, then the guest's
  `PM: suspend entry`, before the application is suspended;
- on return, `guest GPU requests released on foreground (1)`, then
  `request gate opened with N request(s) waiting`, then `PM: suspend exit`;
- the guest display rendering on return, and no `virtio_gpu` response errors,
  Metal errors, RCU stalls, or watchdog lines.

A `0` after the hold means a GPU request was still running after a second; it
is a finding, not a pass.

On a device each cycle must also show `uml: hibernated guest RAM` after the
suspend entry, naming the threads held and the KiB written (a later cycle
rewrites far less than the first), and `uml: restored guest RAM` before
`request gate opened`; `guest RAM stays in the process` is a finding (the hold
failed), and a restore failure ends the process and shows the restart notice at
the next launch. The unit tests cover the order and the fence on any host
(`runtime-thread-fence`, `runtime-guest-hibernation`, `emulator-lifecycle`,
`memory-guest-ram-image`); a Mac run forces the same code with
`--guest-hibernation=on`.

To check the Picture in Picture mode, launch with
`KERNARIUM_BACKGROUND_MODE=pictureInPicture`, which overrides the stored mode
for that launch only, and background the application the same way. The console
must show the mirror at its trickle rate before and its glanceable rate after
`picture in picture started`, the policy moving to `keptRunning` with the GPU
released and no guest `PM: suspend entry`, the guest's own progress (a probe
counting work) continuing through the whole stay, and the window on screen
showing the guest's display. Returning must stop the window, drop the mirror to
its trickle, and leave the guest display rendering; closing the window while
still in the background must quiesce the guest, and the return must resume it,
and tapping the window's return button must come back with the guest
uninterrupted. Springboard exposes the window as `PIP-SBInteractionPassThroughView`;
its close and return buttons appear only after a tap on the window and fade
within about three seconds, so tap the window and then the button in quick
succession. `KERNARIUM_BACKGROUND_THROTTLE=0` keeps the vCPUs at full priority
for a comparison, and with `KERNARIUM_FOOTPRINT_LOG_MS` set the footprint lines
carry the process's CPU time, estimated energy, and performance-core time, from
which the guest's rate, core use, and power in the window follow.

To check the set-time job, choose the keep-running mode (or launch with
`KERNARIUM_BACKGROUND_MODE=keepRunning`, and `KERNARIUM_BACKGROUND_LIMIT_MINUTES`
for a short time), tap the application's `backgroundJobButton`, and background
it. The console must show the job submitted and running, the policy moving to
`jobRunning` with the GPU held and never released, the guest's work continuing
and the process's core use staying low, and the job's end (its time is up, or
stopped) quiescing the guest while still in the background; the return must
release the GPU, resume the guest, and render the display. Stopping the job from
the app's button while in front must change nothing about the guest. For
Android the console must also show the guest suspending itself soon after the
power key (`PM: suspend entry`) and the screen coming back on the return.
The screen goes off first: the console shows `guest display off after N ms`
(or `still on` at the 1.5 s bound, which is a finding) before the hold line, and
under a launcher that is drawing, such as a `input swipe` loop in the guest, a
ten-minute stay must leave no task in D state, no `am_anr`, and the same
`system_server` and `surfaceflinger` pids at the return. A guest whose screen
was off already must stay off through the stay and the return.
`KERNARIUM_BACKGROUND_HOLD_DELAY_MS=N` (automation runtimes only) holds a job's
GPU N milliseconds after the edge instead, on a guest whose screen stays on and
keeps drawing, to find when iOS stops serving Metal: the first Metal
access-revoked error in the console is the answer, and the 1.5 s bound stays
well inside it.

A Mac runs the guest's side of this edge without a device, with no GPU
revoked: `Kernarium/scripts/repro-android-hold-mac.py` launches the Android
automation runtime in Designed for iPad mode, applies the real policy through
`AndroidHoldReproTests`, and checks the guest through adb. The tests skip
unless that script names their channel. It needs an Android bundle whose disk
has the layout the host expects. `--expect fail` is for a build with the old
order, which must show tasks waiting on the GPU control queue and the launcher
not responding to the screen-off broadcast.

To check the restart notice, background the
application, terminate its process with `devicectl device process terminate`,
and launch it again: the console reports that the previous process ended in
the background and the guest display carries the dismissible Guest Restarted
card. A guest this large can also be reclaimed by iOS in the first seconds of
the background, which looks like a missing hold and is not; the next launch
shows the notice. Remove the guest bundles the check copied to the
application's Documents afterwards, and any payload it launched without
`KERNARIUM_PAYLOAD_REMOVE=1`.

### Guest statistics

The host side of Andarium's live statistics (the vCPU load, the frames
presented, the guest's memory in use) is checked on any host by unit tests, one
per source: `runtime-guest-statistics` (the carriers' CPU time, found by name,
and the RAM held), `guest-console-memory` (the `mem` reply parser and request
against a fake console), `gpu-display-statistics` (the display counters through
the service), `emulator-statistics` and `emulator-lifecycle` (the public calls
and their gating: not while quiesced or when a suspend gave no final answer,
not beside another lifecycle call, a suspend starting included, the wait
outside the mutex), `kernarium-statistics` (the runtime's composition and
its refusals: quiesced, display off, lifecycle busy), `emulator-statistics-report`
(the line `--stats` prints) and the package consumers, which call both
functions on an idle handle.

`guest-statistics` (integration, GPU) boots a two-vCPU Linux guest with
`--stats=500` and an offscreen display, with the launcher's standard error in a
file so that the lines neither fill the console nor land inside a command's
markers. An idle guest reads under 5% and a display nobody writes to presents 0
frames a second; a guest writing to its display presents some; and one busy loop
reads half of two vCPUs, within 5 points of libproc's count of the same carriers
and no more than 5 above the guest's own `/proc/stat`. The guest counts the time
the host took from a vCPU as busy, so on a loaded host its figure is the higher
one and bounds the host's from above only. The guest's kernel answers the `mem`
console request, so the memory part must be there, show `MemTotal` in whole MiB
and equal `MemTotal` less `MemAvailable` within 16 MiB; a kernel without the
request fails the test. The test then stands a console of its own where the
guest's was, one that refuses every request as such a kernel does, and checks
that the launcher sends only the `mem` datagram in the console's layout and
shows no memory part. For Android, an
opt-in check runs `uml-darwin-android17 --stats=1000` under enforcing SELinux:
the memory part must appear with no `avc` denial in the guest's log and match
`adb shell cat /proc/meminfo`.

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

A case's bound in the manifest, and the `--candidate-timeout` of a candidate,
is sized on the gadget engine, as is the `LTP_TIMEOUT_MUL` of 4 the runner
gives LTP's own timers. Under `--engine jit` the runner multiplies both by the
guest's factor in `JIT_CASE_BOUND_FACTORS`, 5 for arm64 and 4 for x86_64,
because a JIT compiles every exec'd process's code from scratch: the measured
cost per exec is about four times the gadget engine's on the arm64 JIT
([Cold code](../darwin/cpu/arm64/jit/README.md#cold-code)) and about 2.7 times
on the x86-64 JIT, and a case such as `zram01`, which runs one `dd` per KiB it
writes, is bound by it. The factor is the same for every case, and the
results summary records it as `case_bound_factor`.

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
the gadget and JIT engines in one process, and the
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
the corpus README describes it.

The virtual CPU implements no SVE outside streaming mode, so no corpus case
holds a vector length or non-streaming SVE state. The `arm64-absent-sve`
records pin every SVE encoding that the removed profile executed UNDEFINED
outside the SM = 0 family below, in both engines on every host.

The SM = 0 family is the one set of SVE words that a virtual CPU with SME and
without SVE executes outside streaming mode, at a vector length of 16 bytes: a
deliberate departure from the architecture that [CPU execution and
scheduling](architecture.md#cpu-execution-and-scheduling) defines. No host
observes its results, because an M4 traps those words, so the `arm64-sm0-vl`
record holds the project's own, written from the pseudocode, and
`arm64-instruction-corpus-sm0-vl` re-derives it. The `arm64-sme-native`
records hold no family word at SM = 0, and
`arm64-instruction-corpus-sm0-vl-profile` runs the `arm64-sm0-vl`,
`arm64-absent-sve` and `arm64-absent-sme` records for a virtual CPU with the
SME profile assumed on any host. The unit tests `arm64-sm0-vector-length`,
`arm64-gadget-sme`, `arm64-jit-sme-control` and `arm64-jit-sme-codegen` check
the family against a model of the pseudocode and run it in each engine.

Top-byte ignore (TBI), which every arm64 guest runs with unless the host
passes `--guest-no-top-byte-ignore`, ignores the top byte of every data
address, SP-based or not. The `arm64-tbi-memory` record
holds the atomic, compare-and-swap, ordered, RCpc and exclusive forms reaching
memory through a pointer tagged in its top byte, with the virtual CPU's
top-byte ignore on, and the controls with it off. Its expectations come from
the architecture's pseudocode, it needs no host feature, and
`arm64-instruction-corpus-tbi-memory` re-derives it. The `arm64-tlb` unit test
pins that an entry filled through any tag goes with its page, and the
`guest_tagged_*_loop.S` probes time the form and the aliasing a tag could
cause.

The `arm64-tbi-equivalence` records rerun existing memory cases (general,
SIMD&FP, pair, structure, SVE and SME forms) with a tagged base register and
expect the source's result, add the tag-in-index, carry and SP-base forms, and
pin that a tagged pointer faults at itself with TBI off;
`arm64-instruction-corpus-tbi-equivalence` re-derives them, and
`arm64-jit-top-byte-ignore` runs the JIT's strip across the forms. The SME
cases need an SME host.

The `arm64-floating-trap-native` records, and the outcomes of the
register-only `arm64-floating-trap-scalar`, `-vector` and `-control` cases,
were observed on the M1, which traps floating-point exceptions natively; any
arm64 Mac regenerates them with the same oracle. The `afp` and `bf16` records
need FEAT_AFP and FEAT_BF16 and were observed on the M4. The
`arm64-instruction-corpus-floating-trap-native` tooling test re-derives their
cases and holds every trap to one cause, and one at a program's entry to no
state change.

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

`arm64-aarch32-decoder` (unit) checks the decoder against the answers
`tests/tooling/arm64/a32_expected.py` computes independently of the C++
translation, and against LLVM MC. The `arm64-a32-encoding-table` tooling test
checks the generated tables and the test data against each other offline.

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

`arm64-aarch32-corpus` (unit) checks every case of the generated AArch32
corpus against the decoder, running no engine. The `arm64-a32-corpus` tooling
test regenerates the pack from `tests/unit/arm64/aarch32_corpus/source` and
compares it, `provenance.json`, `manifest.tsv`, `coverage.tsv` and
`excluded.tsv` with the checked-in bytes, and requires cases for every
executed base inventory entry.
Neither needs an oracle. The corpus README owns the format, the oracles and how
to record a suite with `tools/arm64-tests/a32-corpus.py`; recording runs
`qemu-arm` on a build host over ssh and is opt-in. With
`-DUML_A32_ARCHITECTURE_ARCHIVE=<archive>`, the opt-in
`arm64-a32-corpus-plan-freshness` tooling test plans every suite again without
an oracle and requires each source table to hold exactly the planned cases in
order, so a change to the forms or the planner that moves a case is caught
before the table goes stale; it skips without the archive.

## AArch32 semantic library

`arm64-aarch32-semantics` (unit) checks the library's own contract, which the
corpus cannot record. `arm64-aarch32-semantics-corpus` is the corpus's model
lane: it runs every case of the pack through the library on a model of the
oracle's address space and requires 0 failures. It runs the BFloat16, 8-bit
matrix multiply and FHM forms on any host, because the library does not need
those features. Run one record with
`uml_arm64_aarch32_semantics_corpus_tests <pack> --record <name>`, one case
with `--case`, and `--trace` for its steps and initial state; `--skipped` lists
the forms of a case the library does not cover, which fail the lane.

## Guest SME policy

Unit binaries take the guest-SME policy from their environment, like any
run: on by default, off with `DARWIN_UML_GUEST_SME=0`. The suites hold
either way on every host, and a host without SME sees no difference. On an SME
host, an M4 Mac, also run `arm64-sme-foundation`, `arm64-advertised-features`,
`arm64-jit-sme-control`, `arm64-jit-sme-register`, `arm64-jit-sme-memory` and
`arm64-instruction-corpus`, which then runs the native SME records on both
engines, with the policy at its default and with `DARWIN_UML_GUEST_SME=0`.
Elsewhere `arm64-jit-sme-control` runs only its SM=0 cases, on an injected
profile, `arm64-jit-sme-register` and `arm64-jit-sme-memory` skip, and the
corpus skips its native SME records. Integration runs read the policy the same
way: on an M4, `guest-gadgets-smoke` and `guest-jit-smoke` expect the SME
HWCAP2 bits and `guest-hwcap-sanity` executes the SME families.

`guest-sme-gadgets` and `guest-sme-jit` check the guest kernel's SME contract
with the policy at its default, in the modes
`tests/integration/sme/run_guest_sme.py` lists. Each skips, naming its reason,
unless the host has SME and its engine's SME support is `control` or better.
The `kernel`, `spin` and `terminate` modes need no engine support; on an SME
host they run by hand with
`run_guest_sme.py --minimum-support none --modes kernel,spin,terminate`.
`guest-sme-off-gadgets` and `guest-sme-off-jit` run on every host and check
that with the policy forced off the probe sees no SME.

Two opt-in tools list what SME code executes outside streaming mode, where
an SVE word is UNDEFINED on an SME CPU without SVE unless it belongs to the
SM=0 family (CNTB/H/W/D, RDVL, ADDVL, ADDPL, the scalar INC/DEC
B/H/W/D, and LDR and STR of Z and P). `tools/arm64-tests/sme-sm0-scan.py` takes the shipped images (the
desktop EROFS images and the Android release directories), extracts them
with `fsck.erofs`, `debugfs` and `zstd`, and walks every SME function of
every AArch64 ELF file, APK and APEX payloads included, from its inferred
entry mode; any other SVE word reachable at SM = 0 is a limit of the family
(SVE data processing in a body that is not streaming), reported so that
SME is withheld from that workload or the family is extended, and `--expect` pins a
binary's words (Chromium's libyuv executes only CNTD there).
`tools/arm64-tests/sme-idiom-corpus.py` builds `sme_idioms.cpp` with the
given clang and GCC compilers for SVE and SME-only targets, every
optimization level and callee-saved register pressure, and reports the
same classification for each compiler idiom, callee-saved Z and P spills
among the family words. The `arm64-sme-sm0-scan` tooling test checks the classifier and
both tools on synthetic ELF fixtures.

## No guest SVE

`arm64-no-sve` checks that the virtual CPU presents no SVE outside streaming
mode whatever the host reports, and `arm64-advertised-features` pins the words
and registers of named hosts. `guest-no-sve-gadgets` and `guest-no-sve-jit` run
on every host: the freestanding `no_sve_probe.cpp` boots as `/init` and checks
that the guest kernel and the engine agree, and `guest-sme-gadgets` and
`guest-sme-jit` check the streaming record.

## Guest AArch32 foundation

The AArch32 foundation and gadget-engine tests are `arm64-a32-foundation`,
`arm64-a32-guest-key`, `arm64-a32-host` and `arm64-gadget-a32-*`;
`ctest -N -R '^arm64-(a32-|gadget-a32-)'` lists them, with the AArch32
tooling tests, and each test's header states what it checks. Instruction
semantics belong to the instruction corpus, not to `arm64-gadget-a32-control`,
which pins the translator's control behaviour, or `arm64-gadget-a32-forms`,
which holds the engine to the decoder's answer for every stored sample word.

`arm64-gadget-a32-corpus` runs the cases of the AArch32 corpus whose
instructions the engine decodes. A case whose forms need an extension the host
lacks (BF16 and I8MM on the M1 Ultra) is skipped. The `unaligned` and
`fpscr-reserved` suites are recorded from hardware (a Raspberry Pi 4), as the
corpus README describes. A store-exclusive may fail without cause, and the
gadgets' fails whenever a host interrupt lands between the load and the store,
which a loaded host makes frequent enough to see in a gate; a case whose only
difference is that failure runs again, up to 16 attempts in all, and the
summary counts the cases that needed it. `arm64-gadget-a32-corpus-interrupted`
makes the failure happen on purpose: it runs the `exclusive-qemu` and
`sequences-qemu` records (`--record` takes a comma-separated list) under a
timer signal aimed at the guest's thread, and requires that a store-exclusive
lost its reservation and that every case then agreed (it waits for that at
most 60 s, because whether a signal lands in the window is chance).

`guest-aarch32-engine-gadgets` and `guest-aarch32-engine-jit` run the engine
probe, a freestanding armhf program from `tests/integration/aarch32/engine_probe/`
that boots as the guest's `/init`. The build compiles it with the configured
compiler's ARM backend and links it with `link_elf32.py`, aligned to 64 KiB and
aligned to 4 KiB only. Each group's source states what it checks, and a group
needing an extension the HWCAP words leave out is skipped inside the probe.
Every SIGILL is counted: one the probe did not expect fails the run.
`A32_PROBE_GROUPS` selects groups by hand through the runner's `--groups`, and
`A32_PROBE_VERBOSE=1` makes the runner print every line of the guest's output,
passes included.

## AArch32 JIT

The JIT's AArch32 tests run on the macOS JIT only. They are `arm64-jit-a32-*`
and `arm64-jit-executor-paths`; `ctest -N -R '^arm64-jit-(a32-|executor-paths)'`
lists them, and each test's header states what it checks.

`arm64-jit-a32-corpus` runs every case of the AArch32 corpus pack through
`CpuEngine::run_aarch32` on the oracle's memory map. A case whose form is of
an extension the host lacks (BFloat16 and the 8-bit matrix forms on an M1) is
skipped, because the JIT decodes it as UNDEFINED there. A store-exclusive may
fail without cause, and the JIT's fails whenever a host interrupt lands between
the load and the store; a case whose only difference is that failure (a status
of 1 where the pack has 0, and the store not made) runs again, up to 16
attempts in all, and the summary counts the cases that needed it.
`arm64-jit-a32-corpus-interrupted` makes it happen on purpose: it runs the
`exclusive-qemu` record under a signal storm aimed at the guest's thread until
cases have lost a reservation, and requires that every case then agreed.

## Android 32-bit userspace

The Android product runs `armeabi-v7a` and legacy `armeabi` applications beside `arm64-v8a` ones
([`../android/lineage-24/README.md`](../android/lineage-24/README.md#android-product)).
The build side needs no Android build. `lineage24-target-files-fixtures` drives the
target-files verifier over fixtures (ABI lists, the 32-bit runtime, ELF32
placement and alignment, per-ISA ART artifacts, the 32-bit Mesa and HAL files
and their 64-suffixed `mmap` and `lseek` imports, the compressed-EROFS contract of
the read-only partitions (the two parameters, `misc_info`, the compress hints, each
image's superblock, the EROFS kernel options and the fstabs; each APEX payload's
filesystem and superblock, and the example HALs' service binaries read from the payload's
directories), the userdata inode count,
the application read of the vendor security patch level, the built-in
microphone and companion device setup feature declarations, the userdata `quota` flag, and the
suspend blocker with the adb host-connected property it follows, the adb daemon's host-key and
TCP-policy launch properties (read from its APEX payload, with adbd's right to read their labels), the network
setup tool with the ARP policy init sets before it, its library, label and domain
transition, the script that saves the virtual Wi-Fi network once, after boot and
without touching the user's Wi-Fi switch, with its core domain, its binder transfer to the Wi-Fi service's process and the call back, and its marker, the Wi-Fi
HAL and supplicant with their manifest fragments and services, the Wi-Fi feature
declaration and the absent Ethernet, Aware, RTT, Direct and Passpoint ones, the built-in macvlan, cfg80211 and virt_wifi
options of the packaged kernel configuration, the absent wificond, the ClearKey HAL's access to the memfd files of applications, the
policy without the LineageOS grants that Android's neverallow tests refuse, and the
discrete app-op list that keeps AOSP's default ops, the 1024 pixel tablet
default wallpaper in the framework overlay with the checks of the overlay's copy, the example thermal HAL
with its manifest fragment and service, the default CAS HAL with its ClearKey
plugin, the vendor `build.prop` without the unreliable-present-fence property
and with `ro.incremental.enable=1`, the board named `andarium`, the SoC pair in
`ODM/etc/build.prop` (the same name the host gives the virtual CPU, which
`lineage24-builder-lane` requires) and no `ro.soc.*` in the vendor's, the
software decoders' 3840x2160 limits, the
hardware video encoder service, a codec list of its two encoders and two
decoders, their properties and no other enabled decoder, the video nodes' group,
the service's seccomp calls and its label), and `lineage24-builder-lane` the
builder's other assertions;
`lineage24-first-boot-precompile` covers the
ISA set of a package and the per-ISA artifacts and manifest;
`lineage24-mesa-venus-port-generator` covers the per-ISA generators of the
Mesa port and that every arm module is built with Meson's large-file define;
and `lineage24-mesa-venus-source-contract` pins the regenerated series.
`KernariumTests` covers the bundle layout and the data disk capacity range.

A release candidate is qualified on its image, opt-in, on every engine that
runs Android: the Mac gadgets and JIT engines and the iPad gadgets. A
`SIGILL` anywhere is a blocker, as everywhere.

- **Boot.** `boot_completed` with `ro.product.cpu.abilist` listing
  `arm64-v8a,armeabi-v7a,armeabi`, `ro.zygote=zygote64_32` and the secondary zygote
  running without restarts; SELinux enforcing with no denial from the 32-bit
  zygote, `crash_dump32`, the WebView zygote or its RELRO process; native
  daemons and HALs 64-bit. ART Service's first-boot pass skips every package,
  the WebView's `arm` artifacts included, and `odrefresh` compiles nothing for
  either ISA.
- **Applications.** A set of real 32-bit applications: `armeabi`-only and
  `armeabi-v7a`-only APKs, some dual-ABI installed with
  `adb install --abi armeabi-v7a` against a 64-bit control install, and a Java-only
  one forced to 32 bits. Each native package shows its selected `armeabi` or
  `armeabi-v7a` primary ABI in `dumpsys package`, descends from the
  secondary zygote, draws through GLES and, where it uses it, Vulkan, and
  leaves no tombstone. A deliberate `SIGSEGV` in one must leave a symbolized
  `crash_dump32` tombstone. The 64-bit browser's pages load through the 64-bit
  WebView renderer, and a 32-bit application's WebView talks to it.
- **CTS.** The 32-bit and 64-bit variants of the bionic, binder, OS, JNI,
  seccomp, ABI-override and ASLR modules run on the same candidate; a test that
  fails only on `armeabi-v7a` is triaged before acceptance.
- **Alignment.** The engine probe (`tests/integration/aarch32/engine_probe`)
  runs from `/data/local/tmp` on both engines: its fault section must give
  `SIGBUS` with `BUS_ADRALN` for misaligned LDM, STM, LDRD, STRD and VLDM while
  unaligned LDR, LDRH, STR and STRH succeed, as on Linux guests (the kernel has
  no `COMPAT_ALIGNMENT_FIXUPS`).
- **Cost.** Boot to `sys.boot_completed` (dmesg), cold and warm, in an
  ABBA order against the previous release on the same host build; `MemAvailable`
  after a settle and `dumpsys meminfo` for the zygotes and the WebView zygote;
  and the image and compressed sizes. The secondary zygote stays, preloaded only
  while a 32-bit package is installed; it is replaced if it costs more than 3 s
  of iPad cold boot or 100 MiB of guest `MemAvailable`. A 32-bit application
  installed on a device that had none starts the preload, so the first launch
  after that install is the one to time.

Legacy `armeabi` application smoke coverage runs on the physical iPad with
these unmodified F-Droid archive builds:

| APK | Must exercise |
| --- | --- |
| [Simon Tatham's Puzzles 9561.1 (72)](https://f-droid.org/archive/name.boyle.chris.sgtpuzzles_72.apk) | Native Fifteen board generation, three moves, undo and redo. |
| [Gears ES2 7](https://f-droid.org/archive/com.jeffboody.GearsES2eclair_7.apk) | Animated native OpenGL ES 2 gears and touch-controlled view rotation. |
| [Gloomy Dungeons 2013.03.07.0416](https://f-droid.org/archive/zame.GloomyDungeons.opensource.game_1358421967.apk) | Native OpenGL ES 1 level rendering, forward movement, turning, opening a door and firing two shots, which lower the ammunition count by two. |

Each APK's only ARM ABI is `armeabi` (Puzzles also contains x86), and its
native libraries declare ARMv5TE. Install with the explicit
`--bypass-low-target-sdk-block` option and no `--abi` override. Each must
select `primaryCpuAbi=armeabi`, load its native libraries and run as a child
of the 32-bit zygote. The run must keep SELinux enforcing, leave the native
crash buffer empty and power off with runtime result 0. Use
`ANDARIUM_GUEST_INSTANCE` for an isolated compatibility-test guest; full release
qualification still uses the gates above.

## Android guest storage

An Android guest has a read-only system disk and the instance's writable data
disk
([`../android/lineage-24/README.md`](../android/lineage-24/README.md#static-disk-and-boot-contract),
[`../darwin/device/block/README.md`](../darwin/device/block/README.md)). These
tests cover them without an Android build:

- `lineage24-builder-lane` and `lineage24-release-contract` cover the producer:
  the disk policy, the exact-fit system disk and its layout file, the data
  disk template, and the manifest fields.
- `android-partition-boot-selection` (unit) covers the host library over a
  small release that `tests/support/android/android_release_fixture.py`
  assembles with the producer's tools: the strict layout parser, the partition
  tables byte for byte against the producer's, creating, validating and
  growing the data disk, the refusal of any third disk or storage argument,
  and the bootloader message.
- `android17-instance-rules` and `android17-macos-launcher-options` cover the
  instance rules (create, update with a snapshot, refused downgrade, an
  unfinished update, an update that boots counting as applied) over the shared
  vectors and transitions in
  `android/lineage-24/support/instance/update-decisions.json`, and the macOS
  launcher built on them.
- `guest-android-disks` (integration) boots the Linux test guest on that
  release and a data disk the library created. The guest sees the layout's
  partitions at its offsets, a read-only system disk whose partition images
  hash to the layout's digests, and a blank data disk that it formats and
  writes. The library still accepts both disks afterwards, the data disk is
  still sparse, a grown disk shows the larger table with the file intact and
  its filesystem grows into it, and shrinking is refused.

A release candidate is qualified on the Mac, opt-in, with the macOS launcher and
a blank instance:

- the md5 of every read-only partition in the guest equals its target file;
- the guest reaches `sys.boot_completed` on the blank data disk, which it
  formats itself;
- `adb reboot recovery` boots recovery, and its "wipe data" resets `userdata`
  on the data disk;
- starting a newer release on an instance of an older one keeps its apps and
  data and records the update, and starting the older release on the newer
  instance is refused.
- `android-oobe` (integration, registered only when
  `UML_ANDROID_OOBE_RELEASE` names the release and the Android macOS executable
  is built) boots two fresh instances. On the first it checks that the guest
  starts unprovisioned in LineageOS Setup Wizard, which HOME does not leave,
  and completes the wizard through the pages the product shows, exactly
  Welcome, Language, Date and time, Theme and Finish in that order (a hidden
  page or another order fails; the flow is the contract of release r17 and
  later). It then checks the end state: both settings 1, the wizard's HOME
  activity and flow manager disabled, HOME reaching the launcher, and the
  defaults the shortened flow leaves to the platform: three-button navigation
  (`navigation_mode` 0, the gestural overlay off, both mode overlays present),
  no screen lock, location on with the settings it had before the wizard, Wi-Fi
  connected to VirtWifi, and no status bar disable record held by the wizard.
  The second is started with `boot_android.py --provision`, the harness path of
  `tools/android_provision.py`, and must end with both settings 1 and HOME on
  the launcher, the wizard package disabled for user 0 (the shell may not
  disable its components), no wizard process and no status bar record (the
  helper stops the process that holds it), the same defaults, and nothing left
  for the helper to do. On both guests the launcher's data model holds exactly
  the six applications of the default layout on the first page, none in the
  dock, and the log shows no item dropped while the launcher read its layout.
  Measurements on an erased guest use that helper, so they do not land in the
  wizard.
- `android-adb-debugging` (integration, registered only when
  `UML_ANDROID_ADB_DEBUGGING_RELEASE` names the release and the Android macOS
  executable is built) boots a blank instance and checks the adb topics of the
  product: Developer options and USB debugging are off (`adb_enabled` 0,
  `persist.sys.usb.config` without `adb`, adbd's own rule: empty or `none`, the
  value init's property service gives a user build at boot; a debuggable build
  gets `adb`, so the test qualifies the user variant); `adb root` is refused at
  once, adbd stays the shell user and the connection stays (the macOS launcher's
  guest checks no keys, the case that once made adbd announce a restart it never
  made); turning USB debugging on
  records `adb` and leaves adbd's process alone; turning it off drops `adb`,
  restarts adbd once and leaves adbd serving the host; both survive a reboot.
  Where the guest checks keys (`ro.boot.kernarium.adb_auth` 1, which the macOS
  launcher never sets, so only a guest the Kernarium runtime started, which the
  runner's `--serial` attaches to), a second throwaway key stays unauthorized
  with no prompt while debugging is off and raises the prompt while it is on.

## Android hardware video encoding and decoding

A release candidate whose product carries the hardware codecs
([`../android/lineage-24/README.md`](../android/lineage-24/README.md#hardware-video-encoding-and-decoding))
is qualified opt-in, on a guest started with the host's Virtio video device
enabled (`--video`).

- **Codec list.** `MediaCodecList` shows `c2.v4l2.avc.encoder`,
  `c2.v4l2.hevc.encoder`, `c2.v4l2.avc.decoder` and `c2.v4l2.hevc.decoder`
  beside the `c2.android.*` components; the service runs without restarts,
  with no SELinux denial and no seccomp kill in its log.
- **CTS.** The encoder tests of `CtsMediaEncoderTestCases`,
  `CtsMediaV2TestCases`, `CtsVideoCodecTestCases`, `CtsVideoTestCases`,
  `CtsMediaCodecTestCases` and `CtsMediaRecorderTestCases`, filtered to the two
  encoder names, and the decoder classes of `CtsMediaV2TestCases` and
  `CtsVideoTestCases` (the performance and availability classes among them) and
  `AdaptivePlaybackTest`, `DecoderBlockModelTest` and `VideoDecoderPerfTest` of
  `CtsMediaDecoderTestCases`, filtered to the two decoder names, pass on
  `arm64-v8a` and `armeabi-v7a`, and the software `c2.android.*` results do
  not regress. The encoder tests that decode their output pass through the
  hardware decoders. The decoders' rates swing with the load of the host, so
  the performance classes run on a quiet one.
- **Host.** The host's video activity log shows hardware encodes and decodes
  for those runs; a pass that never reached the host codec does not qualify.

## Android UI rendering CTS

`CtsUiRenderingTestCases` qualifies on the iPad's gadget engine in the
automation application or in Andarium (`run_ios_device_android.py --push
Cts.apk:/data/local/tmp/Cts.apk --script`, whose script installs and
instruments the APK), not on the shared Mac. Its
`ColorSpaceTests#testMultiAtlasGlyphsWithColorSpace` paints a thousand glyphs
of the COLRv1 emoji font at 13 sizes on the CPU in each of two renders, and
each render must finish within the test's 60 s wait. On the iPad mini 6 the
slowest render took 37.74 s, about 1.6 times inside the wait. A quiet Mac
renders about as fast, but on the shared Mac the test fails when other guests
load the host, so a Mac failure of that test is recorded as caused by load. The
product keeps the COLRv1 font: no change in view shortens the render by more
than a few percent
([G289](optimization-history.md#ios-graphics-and-later-gadget-work)).

## x86 instruction conformance

The x86 gadget backend uses both handwritten x86-64 and i386 ASM suites
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
context. The macOS unit tree also runs the build-generated x86-64 corpus
through the x86-64 JIT. `x86-64-jit-instruction-corpus` holds it to
`tests/unit/x86_64/jit_corpus_exclusions.tsv`, whose rows name each record the
engine is known to fail with its class and fail the test when the record passes
or names nothing. `x86-64-tier-switch-corpus` runs the corpus on the gadget
engine for the first K retired instructions and on the JIT for the rest, over a
sweep of K, and requires each case to end as the engines alone do;
`tests/unit/x86_64/tier_switch_exclusions.tsv` lists the one record that
differs, in a result the architecture leaves undefined.

The other x86-64 unit tests (`ctest -N -L x86-64` lists them) state in their
headers, or in the comment above their registration in
`tests/unit/x86_64/CMakeLists.txt`, what they check. The `x86-64-jit-*` and
tier-switch tests exist only where the JIT does, on macOS, and the `tsan` label
marks those the TSan workflow also runs. The `ios-products-carry-no-fexcore`
tooling test checks the script the iOS packaging runs to keep FEXCore out of
every iOS product.

## Performance and external campaigns

Benchmarks, full LTP, external networking, KDE/VNC/WebGL desktop campaigns,
GPU conformance, SMP stress, official A64 semantics, and connected-device runs
are opt-in. Run them only when the change or question crosses that boundary.

Geekbench runs the image-local payload, or an attached payload image checked by
its SHA-256 for an image without one, such as every x86-64 and armhf image,
through the checked-in workload script with `--no-upload` and `--iterations 1`,
and requires no Internet preflight. The workload runs Geekbench 6, or Geekbench
5's armv7 build on an armhf image. Manual performance tools and the probe
inventory are under [`../tests/perf/README.md`](../tests/perf/README.md).
Record conclusions in [`optimization-history.md`](optimization-history.md), not
raw logs or a new investigation document.

GPU capabilities, qualification boundaries, and unsupported Vulkan work are
summarized in [`graphics.md`](graphics.md).

## Graphics conformance campaigns

Guest CTS qualification is opt-in, receipt-bound, and separate from the
`check-gpu` workflows above, which never launch a guest or make a Vulkan support
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
