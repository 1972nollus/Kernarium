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
[`MESA_KK_DEBUG=submit_timing` callback contract](../../docs/testing.md) and emits no
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
provider identity and exclude other GPU work when comparing results. Native
line and geometry workload modes use the same callback collector; their
measurement contract is also documented in [the testing guide](../../docs/testing.md).

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

## Gadget forwarding controls

Scalar and SIMD/FP forwarding default to ON; dependency coverage remains OFF.
Set the two forwarding options explicitly for A/B controls. Existing CMake
caches retain their selected values when source defaults change.

Build the manual `uml_scalar_ceiling_bench` target with
`UML_GADGET_SCALAR_FORWARDING=ON` and dependency coverage disabled. Its sources
live in `scalar_ceiling/`, separately from the freestanding guest probes. The
production builder translates the identical A64 bodies used by the native
control. Same-binary forwarding-off/on runs share the stream, CPU-state, TLB,
and link addresses; independent controls require byte-identical streams.
This isolates operand forwarding from code and data layout changes. The
default sample executes 800 million useful
instructions; six repetitions alternate execution order and verify results.

The ideal loop enters the gadget engine once, executes entirely in precompiled
gadgets, and exits once. Its private zero-retirement loop link suppresses
budget replenishment and scheduler exits; ordinary engine behavior is not
changed. Intrinsic gadget dispatch and canonical state access remain part of
the measured cost. This is an ideal scalar-workload ceiling, not a universal
application/native ratio. The same two sources also compile native-only on
AArch64 Linux; that binary can run through `run_guest_command_bench.py` for
the real-guest comparison. See the executable's `--help` for workload selection.

`UML_GADGET_VECTOR_FORWARDING=ON` independently enables adjacent SIMD/FP
forwarding. For application-speed comparisons, keep dependency coverage and
sampling disabled and compare matched builds with neither option, scalar only,
and scalar plus vector forwarding. `run_guest_geekbench.py --cpus 4` runs both
single-core and multi-core sets by default; omit an iteration override for
Geekbench's default workload sizing. Alternate or reverse run order and retain
individual results rather than interpreting one score as a stable gain.

For execution-weighted coverage, use a separate
`UML_GADGET_DEPENDENCY_COVERAGE=ON` build. The Geekbench runner's
`--coverage-file` records counters and workload events; use each installed
version's full single-core set with `--iterations 1 --no-upload`. Aquarium
uses `DARWIN_UML_GADGET_DEPENDENCY_COVERAGE` with its ordinary runner. Analyze
the corresponding files with `analyze_gadget_dependencies.py --help`. The
analyzer separates single-core and multi-core sections when both are present.
Use both forwarding options when measuring the enabled combined policy;
coverage alone records the same declared opportunities without selecting
forwarding variants.

Counters are per entered decoder group and declared source accessor. Scalar
sources include memory addresses; SIMD/FP sources follow the vector contracts.
Each group belongs to one catalog or the unsupported bucket, so combined
percentages divide summed counts without double-counting. Reports show scalar,
SIMD/FP, and combined coverage against all entered guest words and declared
eligible sources. Older scalar-only captures report SIMD coverage as unavailable,
not zero. Window attribution uses nominal 100-ms counter snapshots and host
polls (100 ms for Geekbench console markers, 500 ms for Aquarium); carrier
snapshots are not globally atomic. Conditional reads are potential accesses,
and no cross-block dependency is inferred. Report both all-guest-word coverage
and the declared-source denominator. Never use instrumented scores as speed
evidence.

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
Both remain diagnostics, not scores.

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
purpose. The source-tree audit requires a one-to-one match between the manifest
and `guest_*.S` files.

Build all probes with the `performance-probe-binaries` target. Run one through
the owning runner shown in `probes.tsv`; use the runner's `--help` for emulator,
rootfs, engine, CPU, and worker options.

Probes are self-checking and succeed only when the measured guest command
finishes, the guest shuts down, and the emulator exits zero. They are not CTest
registrations and do not enter ordinary suite aggregates.

The 16 KiB alias probe is page-profile-specific, and the LSE atomic probe needs
assembler support for that extension. Other probes declare any restriction in
the manifest.

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
