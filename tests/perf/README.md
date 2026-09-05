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

`gadget-ceiling-binaries` builds a matched native/ideal scalar benchmark. Both
modes execute the exact labeled instruction words from
`gadget_ceiling_workload.s`. The ideal mode translates that body once with the
production gadget Builder, then keeps every timed iteration inside the linked
gadget stream. It bypasses the UML executor, translation cache, software MMU,
guest kernel, UML scheduler, and the normal 4,096-instruction fairness exit;
the single final interrupt returns only after the complete workload. Host
scheduling and the engine's intrinsic threaded dispatch remain.

After configuring the `macos-unit-16k` preset, build and run the matched control:

```sh
cmake --build .build/presets/macos-unit-16k \
  --target gadget-ceiling-binaries performance-probe-binaries
.build/presets/macos-unit-16k/bin/uml-gadget-ceiling-bench \
  --iterations 100000000 --repetitions 7
```

The reported ratio is a best-case dependent ADD-immediate ceiling. It is not a
general estimate for memory, SIMD, syscall, or whole-guest workloads. The hot
body is instruction-for-instruction identical to
`guest_add_immediate_loop.S`. Use that one static Linux ELF unchanged for both
the arm64 Linux control and `run_guest_link_bench.py` real-guest run; its outer
wall time is a conservative end-to-end guest rate because it also includes
command and process boundaries.

```sh
python3 tests/perf/run_guest_link_bench.py \
  --emulator .build/presets/macos-integration/bin/uml-darwin \
  --rootfs rootfs.erofs \
  --binary .build/presets/macos-unit-16k/tests/perf/probes/guest_add_immediate_loop \
  --engine gadgets --cpus 1 --workers 1 --repetitions 7 \
  --core-class performance --overlay-shell \
  --boot-timeout 180 --command-timeout 120
```

Repeat the guest command with `--engine jit` for the matched JIT control.

Summarize an accepted or rejected trial in
[`../../docs/optimization-history.md`](../../docs/optimization-history.md).
Do not add raw profiles, temporary paths, or a separate investigation document
to the repository.
