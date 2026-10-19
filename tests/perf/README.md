# Performance tools

This directory owns manual benchmark runners and focused assembly probes.
Performance campaigns are opt-in and are not unit or integration correctness
gates.

## Benchmark policy

Use matched builds and record the revision/dirty state, relevant configuration,
host and power state, page profile, engine, guest topology, command, repeated
result, and exit/shutdown status. Diagnostic profiler runs are not score
evidence.

The Mac runners default to `--cpus 4`, `run_ios_device_workload.py` and
`run_ios_device_discard_bench.py` to `--cpus 2`, and a runner's `--workers`
defaults to its `--cpus`. These are defaults, not host requirements. Each
runner's report line or JSON record carries the vCPU count; do not compare
results taken at different counts, and keep one count for a whole series.

Geekbench uses the payload already installed in `rootfs.erofs` and always runs
with `--no-upload` and `--iterations 1`; scores taken before 2026-10-12 used
Geekbench's default iteration count and are not comparable. The workload is
[`guest/geekbench.sh`](guest/geekbench.sh), which the runner installs as a
payload file and which prints the `UML_GEEKBENCH_READY` and
`UML_GEEKBENCH_END:<status>` markers; `/etc/uml-geekbench.env` may name
single-core workloads to run instead of the whole benchmark. The runner creates
disposable writable state and does not start systemd or background desktop
services. See current options with:

```sh
python3 tests/perf/run_guest_geekbench.py --help
```

No x86-64 image carries Geekbench. For such an image, `--payload IMAGE`
attaches a read-only EROFS image holding a Geekbench 6 release
at its root, once it matches `--payload-sha256`; the workload mounts it and
runs its `geekbench6`. The x86-64 product, built with the `macos-x86_64`
preset, runs the same command line on either engine:

```sh
python3 tests/perf/run_guest_geekbench.py \
  --emulator .build/presets/macos-x86_64/bin/uml-darwin \
  --rootfs X86-64-ROOTFS.erofs --payload GEEKBENCH.erofs \
  --payload-sha256 DIGEST --engine gadgets --workloads 102,201,101,104,401 \
  --console-log console.log
```

A 32-bit Arm guest runs Geekbench 5's `geekbench_armv7`, which the workload
finds by name beside Geekbench 6's `geekbench6` and runs with the same options.
The armhf starter disk carries no Geekbench: [`make_geekbench_images.py`](make_geekbench_images.py) packages the
release, checked by the SHA-256 of its
program, into images that stay local and that the tool refuses to write into
the repository outside `.build`. It makes two kinds. `payload` is the EROFS
image `--payload` attaches, with `geekbench_armv7` and `geekbench.plar` at its
root and a 4 KiB block size, which the guest mounts. `rootfs`
cuts the starter disk's ext4 filesystem into an unpartitioned `.ext4` image,
which `--rootfs` takes with `--root-type ext4`; with `--release-dir` it also
installs the release under `/opt/geekbench`, for a device with no second disk
such as an iPad (`KERNARIUM_ROOTFS`). `--program geekbench_aarch64`, before
the subcommand, packages the same release's 64-bit program from an arm64 starter
disk, the control for an armv7 comparison. The armhf image has no `ip`, so a run
passes `--no-network`, and a measurement never needs a network anyway:

```sh
python3 tests/perf/make_geekbench_images.py payload \
  --release-dir RELEASE --program-sha256 DIGEST --output .build/gb5.erofs
python3 tests/perf/make_geekbench_images.py rootfs \
  --starter-image debian-13-minimal-armhf.img --output .build/armhf.ext4
python3 tests/perf/run_guest_geekbench.py --emulator EMULATOR \
  --rootfs .build/armhf.ext4 --root-type ext4 --no-network \
  --payload .build/gb5.erofs --payload-sha256 PAYLOAD-DIGEST \
  --console-log console.log
```

On a physical device, `run_ios_device_workload.py` runs the same script, or
any POSIX shell script with `--script` and the files it reads with `--file`,
in Kernarium. The script and its files reach the guest as a boot payload,
which only the automation build of the runtime boots
([Kernarium/README.md](../../Kernarium/README.md#automation-runs)); the
harness session runs it over the image's overlay root without systemd or a
desktop, prints `UML_SESSION_EXIT:<status>` and powers the guest off. Nothing
is typed, so a script's own markers are safe to key on. A successful run
reaches its markers, a zero session status, and a status-zero managed
emulator return:

```sh
python3 tests/perf/run_ios_device_workload.py --device DEVICE-IDENTIFIER \
  --geekbench --geekbench-workloads 101
```

Attached Instruments or other sampling changes the run into a diagnostic
capture. Back-to-back device runs need a settle pause, because iOS releases
the previous guest's wired window asynchronously and a run started too soon
fails to wire its own.

The physical-iPad UBD discard control compares the shipped 128 MiB request
limit with a 32 KiB soft limit on the same 512 MiB raw range. It installs an
already-built automation Kernarium app, reuses an existing named rootfs file in
the app container, and boots [`guest/ubd-discard.sh`](guest/ubd-discard.sh)
once per mode under a tmpfs writable layer, so the script owns the recreated
overlay disk; it requires exact discard request and sector counts:

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
views with matching native RGBA32 views through the same compute shader, at
four sizes up to 1,048,579 texels, and checks every result component, the
implicit alpha included, exactly. The default `vulkan-queries` mode times each
dispatch with timestamp queries; `metal-whole-submit` uses the
[whole-submit callback](#native-whole-submit-timing), whose interval covers
the entire GPU submission. The output labels the two timing domains, which
must not be compared. Every raw sample is reported and none is clamped or
discarded; an invalid retained query sample leaves its row without a GPU
median, and the submit-to-fence time is a wall-clock bound, not GPU time.
These repeated-resource figures are not physical memory bandwidth, cold-cache
results, or uninstrumented CPU costs: record host, power, compiler, and
provider identity and exclude other GPU work when comparing them. `--help`
gives the options, the sampling and CPU-timing rules, and the exit statuses.

For the native texture-view residency regression, run correctness-only with
Metal API and shader validation enabled:

```sh
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
  BUILD/tests/perf/uml-kosmickrisp-rgb32-texel-bench --correctness-only
```

Pair it with `MTL_SHADER_VALIDATION=0`; both runs must pass all 48 exact
data/guard controls, and the enabled run must report no Metal shader-validation
errors.

Guest coverage is the OpenGL CTS case
`KHR-GL46.texture_buffer.texture_buffer_texture_buffer_range` through
Zink/Venus, which samples all three RGB32 formats in a one-texel range; it
counts only when it executes rather than stopping at a format-support check.
An AArch64 Linux build of this source can also run correctness-only in a Venus
guest through the guest-probe runner. Neither replaces the native
large-resource, implicit-alpha, and guard controls, and neither is guest
performance evidence.

```sh
python3 tests/integration/gpu/run_vulkan_probe.py \
  --emulator EMULATOR --rootfs VULKAN_ROOTFS --probe RGB32_PROBE \
  -- --correctness-only --max-elements 4099
```

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
download. It works for any image that uses the Kernarium init and session
supervisor, so two images or two emulator builds are directly comparable.

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
guest code sampler below. Guests are not serialized by the tool: run one
guest per measurement, and alternate arms (ABBA) across runs.

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
bank publishes one symbol per gadget, so a sampled PC in the bank attributes
straight to the gadget that owned it; assembler-local labels fold into their
routine, and self time is a frame's count less its children's. The cold
section names only its start, so a PC there reads as
`uml_x86_64_gadget_cold_section_begin` plus an offset rather than as the
gadget that branched to it. Threaded dispatch leaves no usable frame chain,
because `x29` holds the retired-instruction count rather than a frame pointer,
but the deepest frame is the real PC, which is all a gadget profile needs. A
5 ms interval leaves the scored rate unchanged.

## Guest code sampling

Every arm64 gadgets build carries a statistical profiler of translated guest
code that needs nothing from the guest, so it also profiles applications a
production Android build keeps from `simpleperf`. Set
`DARWIN_UML_GUEST_SAMPLES` to `PATH` or `PATH:HZ` (default 1000 passes a
second); the profile is rewritten there every two seconds, when a run ends,
and at exit. A sampler thread suspends each vCPU carrier in turn; when the
carrier is inside gadget code, its executor names the translated block and the
guest instruction words it came from, and any other host PC is counted by host
symbol (the guest kernel, the executor, translation, MMU). The `gadget` lines
count the block samples again by the gadget symbol they stopped in. Each block
sample also records the guest thread pointer (`TPIDR_EL0`) it ran under, which
tells one busy guest thread from a pool. Guest entry and exit store one flag each;
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
cache holds and how it came to hold it. Set `DARWIN_UML_GADGET_CACHE_CENSUS`
to a path; each cache appends a snapshot of itself when its executor is
destroyed, and another every `DARWIN_UML_GADGET_CACHE_CENSUS_SECONDS` seconds
when that is set, taken by its own vCPU at its next lookup. Summarize a
capture with `analyze_gadget_cache_census.py --help`: it reports one snapshot
(`--snapshot`, by default the one taken at teardown) or one line per snapshot
(`--series`).

A snapshot is an `E` record with the cache's geometry, measured footprint and
history, a `G` record per resident address space, a `P` record per resident
translation, and, for one in every `DARWIN_UML_GADGET_CACHE_CENSUS_WORDS`
bodies and link records, a `W` record listing the body's stream word by word
as a gadget, a link or indirect record named by guest PC, or another operand,
and an `L` record naming the link record's guest PC. Both gadget engines write
the same records, so a private cache costs measured bytes rather than an
estimate on either; a field an engine has no counterpart for is simply absent.
The footprint is the committed slabs, every resident translation's allocations
as the allocator sized them, the link and indirect records and target tables,
and the per-address-space indexes the cache owns; on arm64 it also says how
much of the body arena is chunks that hold no live body. The `E` record also
counts the link sweeps, the link records and indirect sites they released, and
the time they took. A `P` record carries its translation's allocation and the
header, stream and validation bytes in it.

When arm64 executors share an address space's translations, an `E` record
counts only what its cache owns (slot slabs and slot lists), each `G` record
names the space its slots point into, every cache holding a space writes the
space's arena, link and indirect bytes on an `S` record, and a `P` record
names its body. The report counts each space and each body once, and the
history counts the misses that adopted another cache's body. Each `G` and `S`
record also says whether the MMU still has the address space's generation
(`alive=1`) or has destroyed it (`alive=0`; negative when it cannot say from the
thread taking the snapshot), and the report counts the bytes of the destroyed
ones. An x86 cache, which keeps its address spaces to itself, writes `alive=`
and the bytes of the address space's translations (`held=`) on its `G` record.

The history counts how each lookup ended, why each departing translation
left (way eviction, address-space retirement, or a discard), and, for every
store, whether the cache had held the same code under the same key before and
why it was gone. A store of code that another cache translated first is
counted against the executors of its own address space or against another
address space. For a translation evicted and later stored again unchanged,
the number of evictions in between goes into a power-of-two histogram, which
says how many of those misses an exclusive victim tier of a given size would
have served.

A census build pays for this on every store and pauses a vCPU for each
snapshot, so it runs slower than a default build; its timings are not
measurements. `DARWIN_UML_GADGET_CACHE_CENSUS_TOTALS=1` keeps only the `E` and
`G` records and leaves stores unmatched against other caches, which is what a
browser guest on a loaded host can afford; read such a capture with
`--series`.

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

The LSE atomic probe needs assembler support for that extension. Other probes declare any restriction in
the manifest.

## Loader corpus

A probe isolates one mechanism; an application's load phase is ordinary
scalar code spread across many. [`guest_loader_corpus.sh`](guest_loader_corpus.sh)
runs real programs from the Debian rootfs whose work resembles one: gzip,
xz and bzip2 decompression, a bind-now dynamic link of a large library,
Python imports, sort, glibc regular expressions, a C front end, Perl hash
churn, JVM start-up and Node start-up. It runs inside the guest under any
console runner (on an iOS device as `run_ios_device_workload.py --script`),
prepares its inputs from rootfs files, runs every workload once to warm the
page and translation caches, and prints
`CORPUS:<name>:<round>:<runs>:<centiseconds>`
per workload and round. A workload the rootfs cannot run prints
`CORPUS_SKIP:<name>` once and is left out; an iOS device's image has no
`cc1`, so it skips the C front end. `CORPUS_ROUNDS` and `CORPUS_ONLY` select
the work. Each workload is sized to about two seconds on a gadgets guest, so
compare arms round by round rather than across launches.

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

After configuring the `macos-unit` preset, build and run one workload:

```sh
cmake --build .build/presets/macos-unit \
  --target gadget-ceiling-binaries performance-probe-binaries
.build/presets/macos-unit/bin/uml-gadget-ceiling-bench \
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
  --binary .build/presets/macos-unit/tests/perf/probes/guest_scalar_mix_loop \
  --engine gadgets --workers 1 --repetitions 7 \
  --core-class performance --overlay-shell \
  --boot-timeout 180 --command-timeout 120
```

Repeat the guest command with `--engine jit` for the matched JIT control.

Summarize an accepted or rejected trial in
[`../../docs/optimization-history.md`](../../docs/optimization-history.md).
Do not add raw profiles, temporary paths, or a separate investigation document
to the repository.
