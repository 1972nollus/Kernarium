# Performance tools

This directory owns manual benchmark runners and focused assembly probes.
Performance campaigns are opt-in and are not unit or integration correctness
gates.

## Benchmark policy

Use matched builds and record the revision/dirty state, relevant configuration,
host and power state, page profile, engine, guest topology, command, repeated
result, and exit/shutdown status. Diagnostic profiler runs are not score
evidence.

Geekbench uses the payload already installed in `rootfs.erofs` and always runs
with `--no-upload`. The runner creates disposable writable state and does not
start systemd or background desktop services. See current options with:

```sh
python3 tests/perf/run_guest_geekbench.py --help
```

Kernarium has an explicit physical-device Geekbench mode using the same direct
guest shell. A successful run must reach the ready/end markers, guest poweroff,
and status-zero managed emulator return. Attached Instruments or other sampling
changes the run into a diagnostic capture.

`KERNARIUM_GUEST_SCRIPT` carries one base64 POSIX shell script over that same
overlay, which is how a device runs the probe payloads and a Geekbench command
line the fixed modes do not express. It is mutually exclusive with the other
console modes. A host driving it must not key completion on a marker inside the
script: the guest console echoes what it is fed and kernel timestamps interleave
with that echo, so only the emulator's own return line is unambiguous. Back-to-
back device runs also need a settle pause, because iOS releases the previous
guest's wired window asynchronously and a run started too soon fails to wire
its own.

Profile readers under this directory summarize aggregate stacks, group them by
workload, and compare two diagnostic captures. They do not decide acceptance
without a matched benchmark or focused mechanism control.

The physical-iPad UBD discard control compares the shipped 128 MiB request
limit with a 32 KiB soft limit on the same 512 MiB raw range. It installs an
already-built Kernarium app, reuses an existing named rootfs file in the app
container, recreates the disposable overlay for each mode, and requires exact
discard request and sector counts:

```sh
python3 tests/perf/run_ios_device_discard_bench.py \
  --device DEVICE-IDENTIFIER
```

## Native RGB32 texel buffers

The manual `uml-kosmickrisp-rgb32-texel-bench` target links only the verified
KosmicKrisp provider. Configure a `macos-unit-gpu-venus` build with explicit
accepted non-GPU and experimental GPU prefixes, then build and run it directly:

```sh
cmake --build BUILD --target uml-kosmickrisp-rgb32-texel-bench
BUILD/tests/perf/uml-kosmickrisp-rgb32-texel-bench
MESA_KK_DEBUG=submit_timing BUILD/tests/perf/uml-kosmickrisp-rgb32-texel-bench \
  --timing metal-whole-submit
```

It compares RGB32 float, signed integer, and unsigned integer uniform texel
views with matching native RGBA32 views through the same compute shader per
type. Four bounded sizes (4,099 through 1,048,579 texels) exercise partial work
groups and nonzero aligned view offsets. Every result component, including the
implicit alpha, is checked exactly; input/output guards and a zero-active-count
dispatch are separate correctness controls. Native measurements do not use a
guest, shared draw fixture, CTest registration, or ordinary suite aggregate.

RGB/RGBA order alternates across three warmups and nine retained samples.
`--samples`, `--warmup`, and `--max-elements` allow bounded manual subsets; see
`--help`. The default `vulkan-queries` mode preserves raw dispatch timestamp
intervals; `metal-whole-submit` instead uses the provider's opt-in
[`MESA_KK_DEBUG=submit_timing` callback contract](#native-whole-submit-timing) and emits no
Vulkan query commands. Its interval covers the entire GPU submission, including
barriers and output stores, not just the texel loads. These timing domains are
explicitly labeled and must not be compared as equivalent intervals.

Both modes report medians and every raw sample, instrumented CPU command
recording and view-creation costs, and logical texel completions per nanosecond.
CPU view creation uses fixed batches of 32 simultaneous views to span the host
clock's finite resolution. Raw batch totals and their medians are reported
separately from amortized per-view medians. These include call setup and loop
overhead, exclude all destruction, and are not single-call latency measurements.
Initial CPU filling, pipeline creation, mapped readback checks, and view
destruction are outside the respective timed regions. Submission through fence
wait is recorded separately as a wall-time bound, not as GPU time. The
whole-submit collector permits exactly one outstanding command-buffer submission
and requires one valid callback for every control, warmup, and retained sample;
it drains the callback stream before accepting the final coverage count.
Missing, duplicate, late, zero, nonfinite, reversed, device-changing, error, or
wall-bound-violating callback records fail closed. In query mode any invalid
tick pair, including a control or warmup, causes exit 2; an invalid retained
pair also suppresses that row's GPU median/throughput. Raw values are never
clamped or selectively discarded. Exit 1 denotes a correctness, API, CPU timer,
or callback-validation error. No speed threshold determines success.

These repeated-resource measurements are not physical memory bandwidth,
cold-cache results, or uninstrumented CPU costs. Record host/power/compiler/
provider identity and exclude other GPU work when comparing results.

`--correctness-only` reuses the same shader, formats, aligned offsets, exact
component checks, and guards, with one zero-active control and one full-data
dispatch per format/size. It creates no query pool or callback collector,
records no CPU/GPU timings, and reports no throughput. It cannot be combined
with `--timing`, `--samples`, or `--warmup`.

For the native texture-view residency regression, run correctness-only with
Metal API and shader validation enabled:

```sh
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
  BUILD/tests/perf/uml-kosmickrisp-rgb32-texel-bench --correctness-only
```

Pair it with `MTL_SHADER_VALIDATION=0`; both runs must pass all 48 exact
data/guard controls. The enabled run also must report no Metal shader-validation
errors. This checks native RGBA view identity registration alongside unchanged
RGB raw-address reads, not performance.

Released guest coverage can use the unchanged OpenGL case
`KHR-GL46.texture_buffer.texture_buffer_texture_buffer_range` through Zink/Venus.
Its vertex shader samples all three RGB32 formats and checks their three stored
components; this one-texel range check does not replace the native large-resource,
implicit-alpha, or guard controls. Released Vulkan RGBA32 uniform-buffer controls
provide separate native-format coverage. A case that stops at a format-support
check is not execution coverage.

For an optional supplemental guest regression, an offline AArch64/Linux build of
this same source must run in correctness-only mode and prove `VK_DRIVER_ID_MESA_VENUS`
with `UML_VULKAN_REQUIRE_VENUS=1`. The existing authenticated guest-probe runner
owns device selection, payload injection, the final success marker, and clean
shutdown:

```sh
python3 tests/integration/gpu/run_vulkan_probe.py \
  --emulator EMULATOR --rootfs VULKAN_ROOTFS --probe RGB32_PROBE \
  --page-size 16384 -- --correctness-only --max-elements 4099
```

This separate RGB32/RGBA32 behavioral regression supplements targeted released
CTS controls; it is neither CTS nor guest performance evidence.

The shader source and generated header are owned here. Regenerate with an
explicit offline compiler (append `--verify` for a read-only freshness check):

```sh
python3 -B tests/perf/generate_rgb32_texel.py --glslc /path/to/glslc
```

## Native whole-submit timing

`MESA_KK_DEBUG=submit_timing` enables optional raw Metal commit-feedback
diagnostics on stderr: an opaque device identity, GPU start and end host
seconds, and the commit error code for the whole committed command-buffer
batch. It is not a Vulkan timestamp-query result, adds no GPU commands, and is
absent from the disabled path.

The KosmicKrisp line and geometry unit executables accept the opt-in arguments
`performance` (render-only Vulkan queries) and `performance-whole-submit`
(native Metal commit feedback through `MESA_KK_DEBUG=submit_timing`), and the
RGB32 texel bench uses the same collector through `--timing
metal-whole-submit`. Registered CTests never run these modes. The collector
permits one outstanding submission and requires exactly one finite, ordered,
wall-bounded callback per draw; missing, malformed, duplicate, late, or
unmatched records fail closed rather than being filtered. Whole-submit
intervals include every clear, barrier, and copy in the Metal commit and are
not comparable with render-only query intervals. No speed ratio is a
correctness threshold.

## Steam appliance boot time

`run_steam_boot_bench.py` measures how long a Kernarium Steam appliance takes
to reach its own readiness contract: an X window named `Steam Big Picture Mode`
whose map state is `IsViewable`. One run creates a fresh GPT user-data disk,
times a first boot while Steam downloads and installs its own update, shuts the
guest down through systemd, then times a second boot on that same disk with no
download. It works for any image that uses the shared Kernarium init and
session supervisor, so two images or two emulator builds are directly
comparable.

```sh
python3 tests/perf/run_steam_boot_bench.py \
  --image base      --emulator BUILD-A/bin/uml-darwin --rootfs A.erofs \
  --image candidate --emulator BUILD-B/bin/uml-darwin --rootfs B.erofs \
  --output-dir RESULTS --runs 3
```

The readiness query runs as the user who owns the display socket rather than as
the console's root shell, because the session's X server refuses an
unauthorized client whatever its window state is.

Runs are strictly sequential and counterbalanced across images, so host drift
cannot masquerade as an image difference. Each run records host load and memory
pressure at its start; that is evidence, not an acceptance gate. Results are a
`run.json` per run, a combined `results.tsv`/`results.json`, and a summary that
gives median/min/max totals and then the per-phase medians side by side: a
total says one image is slower, only the phases say whether that is the kernel,
the seed copy, a network-bound download, or Steam's own install.
`--summarize RESULTS` re-prints those tables from a finished campaign's
`results.json` without running anything. See `--help` for guest topology,
timeout, and poll options.

A run that reaches Big Picture has its 32 GiB user-data disk deleted afterwards.
A run that does not is kept: the guest journal and Steam's own logs live on that
disk, and deleting it is what makes such a failure unclassifiable once the guest
is gone. The kept path is printed to the run log and recorded as
`userdata_image` in `run.json` and in the campaign's `results.json`.
`--discard-failed-disks` deletes it anyway, for a long unattended campaign on a
full volume; `--keep-disks` keeps every run's disk, successful ones included.

The harness never modifies an image. Phase times therefore come from records an
unmodified appliance already keeps — systemd's monotonic unit timestamps, the
storage stage's progress file, and Steam's `bootstrap_log.txt` — read once per
boot over the ttyS0 console and converted to seconds since emulator exec through
one clock anchor that is read in a command of its own and bracketed by host
monotonic readings. Readiness is polled, so its resolution is the poll interval;
the other phases resolve to half an anchor round trip, reported per boot as
`phase_anchor_round_trip`. Storage preparation completes before the console
exists, so it is recovered from that progress file's modification time rather
than observed live.

The display is `--gpu-display=vnc` with no client attached, which accepts and
drops each scanout: measured boots are unattended and free of window-server
scheduling, and they exclude the cost of presenting real pixels. Because the
first boot includes a network download, report its total, the download bytes
and duration, and the total excluding the download window together.

## Android application launch time

`android_launch_suite.py` times cold launches of the applications in
`android_launch_apps.tsv` in one Android guest boot. Each run boots an APFS
clone of a prepared source disk, so every arm starts from the same installed
and compiled state and the source disk is never written. Each round force-stops
every application and times it from the activity manager's `START` line to the
first logcat line that matches its completion pattern, both by the guest's
logcat timestamps. A row's pattern marks the end of the launch's work: for a
game, its first playable scene rather than its splash screen.

```sh
python3 tests/perf/android_launch_suite.py --emulator BUILD/bin/uml-darwin-android17 \
  --release RELEASE --source-disk PREPARED.img --work-disk RUN.img \
  --label candidate --output launches.jsonl
python3 tests/perf/android_launch_suite.py --summary launches.jsonl
```

Each launch record also carries `am start -W`'s `TotalTime` (first frame), the
emulator's CPU seconds during the launch, its threads' scheduling priorities (a
napped emulator drops from 31 to 4 and launches several times slower), and the
host load. A launch that misses its limit is recorded with no time and left out
of the medians. The source disk's `.release-id` must name the release, because
the launcher re-creates a disk from another release. `--env KEY=VALUE` sets one
arm's emulator environment; `--guest-samples PATH` profiles the boot with the
guest code sampler below. Guests are not serialized by the tool: take the GPU
lock and a guest slot around each run, and alternate arms (ABBA) across runs.

`--scenario android_scenarios/NAME.json` follows each successful launch with a
fixed sequence of sleeps and taps and waits for the scenario's completion line;
the record adds the guest seconds to that line, the emulator's CPU seconds, and
the values the scenario's patterns extract from it, such as the phase times an
application logs. Taps are display coordinates, so a scenario states the
display size it was written for. `--display vnc --vnc-port N` boots on a VNC
display instead of the native window, which lets a person watch or steer an
unattended run.

## Gadget forwarding controls

Scalar and SIMD/FP forwarding are always built; `BuildOptions` selects them per
program, so controls compare forwarding in one binary.

The manual `uml_scalar_ceiling_bench` target has its sources in
`scalar_ceiling/`, separately from the freestanding guest probes. The
production builder translates the identical A64 bodies used by the native
control. Forwarding-off and forwarding-on runs share the stream, CPU-state,
TLB, and link addresses, which isolates operand forwarding from code and data
layout changes. The default sample executes 800 million useful instructions;
six repetitions alternate execution order and verify results.

The ideal loop enters the gadget engine once, executes entirely in precompiled
gadgets, and exits once. Its private zero-retirement loop link suppresses
budget replenishment and scheduler exits; ordinary engine behavior is not
changed. Intrinsic gadget dispatch and canonical state access remain part of
the measured cost. This is an ideal scalar-workload ceiling, not a universal
application/native ratio. The same two sources also compile native-only on
AArch64 Linux; that binary can run through `run_guest_command_bench.py` for
the real-guest comparison. See the executable's `--help` for workload selection.

This bench stays separate from the [ideal gadget ceiling](#ideal-gadget-ceiling):
it measures what forwarding changes, swapping forwarding-off and forwarding-on
streams in place so both run at the same addresses, while the ceiling compares
the native control with one ideal stream of the bodies it shares with the
guest probes.

## Execution histogram

A separate `UML_GADGET_EXECUTION_HISTOGRAM=ON` build counts every executed
gadget entry by its (predecessor, entry) pair and every executor round trip by
reason. Set `DARWIN_UML_GADGET_EXECUTION_HISTOGRAM` to a path; the merged
counts are rewritten there every two seconds and at exit, most frequent
first, so a run that ends on a signal still leaves its last snapshot. The
Geekbench and Aquarium runners need no option: copy the file at each console
marker for per-workload differences. Weighting each entry's count by the
shortest helper-free host-instruction path of its symbol (from `objdump -d`)
models the fast-path cost per guest instruction; per-thread hardware
counters from an `xctrace` CPU Counters recording measure the whole engine.
Both remain diagnostics, not scores. The switch changes the gadget executor's
layout, so every target that includes the gadget headers takes it from the
build. The unit tests that run the executor pass in such a build; the structure
tests fail there, because they pin the default stream, which has no counter
entries.

## Host sampling of the x86-64 gadget engine

Host sampling profiles the x86-64 gadget engine without instrumenting it. The
bank publishes one symbol per gadget, so a sampled host PC attributes straight
to the gadget that owned it; assembler-local labels fold into their routine,
and self time is a frame's count less its children's. Threaded dispatch leaves
no usable frame chain, because `x29` holds the retired-instruction count rather
than a frame pointer, but the deepest frame is the real PC, which is all a
gadget profile needs. A 5 ms interval leaves the scored rate unchanged.

## Guest code sampling

Every arm64 gadgets build carries a statistical profiler of translated guest
code that needs nothing from the guest, so it also profiles applications a
production Android build keeps from `simpleperf`. Set
`DARWIN_UML_GUEST_SAMPLES` to `PATH` or `PATH:HZ` (default 1000 passes a
second); the profile is rewritten there every two seconds, when a run ends,
and at exit. A sampler thread suspends each vCPU carrier in turn; when the
carrier is inside gadget code, its executor names the translated block and the
guest instruction words it came from, and any other host PC is counted by host
symbol (the guest kernel, the executor, translation, MMU). Each block sample
also records the guest thread pointer (`TPIDR_EL0`) it ran under, which tells
one busy guest thread from a pool. Guest entry and exit store one flag each;
the translated code is unchanged. Only the gadgets engine is sampled, so a
macOS run, whose default engine is the JIT, needs
`--execution-engine=gadgets`.

`symbolize_guest_samples.py PROFILE ELF_OR_DIR...` finds each block's words in
the executable segments of the ELF files given, which places it in a library
and function regardless of where the guest loaded it. For an Android
application, pass its APK's `lib/arm64-v8a` and the guest's `/system/lib64`,
`/apex/*/lib64` and `/system/bin` (all readable through `adb pull` on user
builds). A block is attributed at its first instruction, so time in a block
that falls through a function boundary counts against the first function. The
report ranks guest threads; `--thread TPIDR` restricts it to one of them. The
profile is cumulative, so copy it at a phase boundary and pass that copy as
`--baseline` to report only the interval after it.

## Cache census

A separate `UML_GADGET_CACHE_CENSUS=ON` build reports what every executor's
cache still held when the run ended. Set `DARWIN_UML_GADGET_CACHE_CENSUS` to a
path; each executor appends its measured footprint and one record per resident
translation, naming the address space, guest PC, stream length and a digest of
the guest code the translation consumed. Summarize a capture with
`analyze_gadget_cache_census.py --help`.

Both gadget engines report in the same record format, so a private cache costs
measured bytes rather than an estimate on either. The footprint is the
committed slabs, every resident translation's own allocations, the link and
indirect records, and the per-address-space indexes the cache owns; a field an
engine has no counterpart for is simply absent from its records. Only arm64
owns a stream lookup index and counts the lookups against it, and only x86-64
owns a target link table per address space and reports the slot size it was
compiled with.

Identity is the consumed guest code, not the produced stream, because two
executors translating the same code embed pointers to their own link records.
That is what lets the report separate duplication between executors sharing an
address space from the same code translated again for another address space.

## Focused assembly probes

[`probes.tsv`](probes.tsv) is the complete inventory of freestanding A64 probe
sources. It records the mechanism, required guest feature, runner, and retained
purpose. [`CMakeLists.txt`](CMakeLists.txt) builds every row, so a row without
its source fails the build; keep one row per `guest_*.S` file.

Build all probes with the `performance-probe-binaries` target. Run one through
the owning runner shown in `probes.tsv`; use the runner's `--help` for emulator,
rootfs, engine, CPU, and worker options.

Probes are self-checking and succeed only when the measured guest command
finishes, the guest shuts down, and the emulator exits zero. They are not CTest
registrations and do not enter ordinary suite aggregates.

The 16 KiB alias probe is page-profile-specific, and the LSE atomic probe needs
assembler support for that extension. Other probes declare any restriction in
the manifest.

## Loader corpus

A probe isolates one mechanism; an application's load phase is ordinary
scalar code spread across many. [`guest_loader_corpus.sh`](guest_loader_corpus.sh)
runs real programs from the Debian rootfs whose work resembles one: gzip,
xz and bzip2 decompression, a bind-now dynamic link of a large library,
Python imports, sort, glibc regular expressions, a C front end, Perl hash
churn, JVM start-up and Node start-up. It runs inside the guest under any
console runner (on the iPad through `KERNARIUM_GUEST_SCRIPT`), prepares its
inputs from rootfs files, runs every workload once to warm the page and
translation caches, and prints `CORPUS:<name>:<round>:<runs>:<centiseconds>`
per workload and round. A workload the rootfs cannot run prints
`CORPUS_SKIP:<name>` once and is left out; the iPad image has no `cc1`, so it
skips the C front end. `CORPUS_ROUNDS` and `CORPUS_ONLY` select the work.
Each workload is sized to about two seconds on a gadgets guest, so compare
arms round by round rather than across launches.

## Host issue ratios

`uml-host-issue-ratio` measures how many 128-bit loads and vector ALU
operations the host core issues per cycle, alone and mixed, calibrating the
cycle against a dependent ADD chain. Run it once as built for the performance
cores and once under `taskpolicy -c background` for the efficiency cores.

## Ideal gadget ceiling

`gadget-ceiling-binaries` builds a matched native/ideal benchmark with three
workloads. `add-immediate` is the dependent ADD chain, a best case rather than
a mix. `scalar-mix` is a browser-like integer loop: loads of every width with
register and unsigned offsets, a pair load, three stores, flag-setting ALU,
selects, a never-taken bounds check and two data-dependent skips. `simd-mix`
is an FP inner loop: vector pair and register-offset transfers, indexed and
vector FMLA, converts, and an FP compare branch. Each body is a
`gadget_ceiling_*.inc` macro that the matching `guest_*_loop.S` probe includes
too, so the native control, the ideal stream and the real guest execute the
same words.

The ideal mode translates a body once with the production gadget Builder,
gives every branch target inside the body its own block, keeps every timed
iteration inside the linked streams, and maps the guest buffer through the
static MMU so the software TLB is warm. It bypasses the UML executor,
translation cache, guest kernel, UML scheduler, and the normal
4,096-instruction fairness exit; the single final interrupt returns only after
the complete workload. Host scheduling and the engine's intrinsic threaded
dispatch remain.

After configuring the `macos-unit-16k` preset, build and run one workload:

```sh
cmake --build .build/presets/macos-unit-16k \
  --target gadget-ceiling-binaries performance-probe-binaries
.build/presets/macos-unit-16k/bin/uml-gadget-ceiling-bench \
  --workload scalar-mix --repetitions 7
```

`--dump-stream` prints each translated block's words with the gadget entries
the Builder selected, so a body's host cost can be attributed instruction by
instruction against the static entry costs.

Rates count retired guest instructions, loop control included; the two
data-dependent branches report their fall-through counts so the words they
skipped are subtracted. Native and ideal runs must reproduce the same checksum
and path counts, and the `UML_GADGET_CEILING_RESULT` line prints them for the
default iteration count. The mix probes print the same fields from the real
guest, which must agree with the host line. Every ratio is a workload ceiling,
not an application estimate.

Run the matching probe unchanged in a real guest; its outer wall time is a
conservative end-to-end guest rate because it also includes command and
process boundaries.

```sh
python3 tests/perf/run_guest_link_bench.py \
  --emulator .build/presets/macos-integration/bin/uml-darwin \
  --rootfs rootfs.erofs \
  --binary .build/presets/macos-unit-16k/tests/perf/probes/guest_scalar_mix_loop \
  --engine gadgets --cpus 1 --workers 1 --repetitions 7 \
  --core-class performance --overlay-shell \
  --boot-timeout 180 --command-timeout 120
```

Repeat the guest command with `--engine jit` for the matched JIT control.

Summarize an accepted or rejected trial in
[`../../docs/optimization-history.md`](../../docs/optimization-history.md).
Do not add raw profiles, temporary paths, or a separate investigation document
to the repository.
