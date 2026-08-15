# Performance tools

This directory owns manual performance campaigns. It is deliberately outside
the unit and integration correctness presets: a benchmark result is meaningful
only with a recorded host, power state, build, rootfs path, engine, topology,
and repeated matched runs.

Run the cheap entry-point check after changing shared integration helpers:

```sh
python3 tests/tooling/performance/test_cli.py
```

Geekbench runs without a profiler by default. Pass `--profile-dir` only for a
separate diagnostic run; matched gadget/JIT score campaigns must omit it.
The runner invokes the image-local payload at
`/opt/geekbench/release/geekbench6 --no-upload`, so it
does not prompt to upload results and has no network permission flag or
host/guest connectivity preflight. The runner constructs a tmpfs-backed
OverlayFS over the EROFS image and benchmarks from its direct PID-1 shell.
It deliberately does not start systemd, Docker, containerd, getty, or periodic
maintenance services; stopping them after boot is both slower and noisier than
never starting them. Correctness coverage for the production systemd path
remains owned by the separate integration tests.

```sh
python3 tests/perf/run_guest_geekbench.py \
  --emulator .build/presets/macos-integration/bin/uml-darwin \
  --rootfs rootfs.erofs \
  --console-log .build/results/geekbench-gadgets-16k.log \
  --engine gadgets --cpus 10 --memory 8G
```

The payload is already present in `rootfs.erofs`; the runner neither downloads
it nor opts into an upload. No host-network grant is part of this local
campaign.

Kernarium exposes the same direct-shell policy as an opt-in physical-device
campaign. Install the Release app and place the authenticated `rootfs.erofs`
in its Documents directory before launching it with the environment flag:

```sh
xcrun devicectl device process launch \
  --device DEVICE-UDID \
  --environment-variables '{"KERNARIUM_GEEKBENCH":"1"}' \
  --console --timeout 7200 com.imxieyi.Kernarium
```

For a focused single-core confirmation, add
`KERNARIUM_GEEKBENCH_WORKLOADS` as a comma-separated numeric Geekbench
workload list. The runtime validates the value before passing it to the guest;
for example, Asset Compression, Background Blur, Horizon Detection, and HDR
are `203,302,401,403`:

```sh
xcrun devicectl device process launch \
  --device DEVICE-UDID \
  --environment-variables \
  '{"KERNARIUM_GEEKBENCH":"1","KERNARIUM_GEEKBENCH_WORKLOADS":"203,302,401,403"}' \
  --console --timeout 7200 com.imxieyi.Kernarium
```

The app recreates its normal 8 GiB temporary ext4 disk, uses it as the upper
and work storage for an OverlayFS over the EROFS image, mounts only the kernel
filesystems needed by the payload, and runs Geekbench from the PID-1 shell.
It does not start systemd. The physical-device application policy still owns
guest RAM and vCPU count; on the current 4 GiB acceptance iPad those are 1 GiB
and the device's physical processor count. A successful console contains both
`KERNARIUM_GEEKBENCH_READY` and `KERNARIUM_GEEKBENCH_END:0`, followed by a
clean guest poweroff and status-zero managed-emulator completion. The SwiftUI
application itself remains open after the managed guest ends.

Use a separate, explicitly diagnostic launch for Instruments sampling. For
example, attach short Time Profiler windows to named workload phases while the
console remains visible:

```sh
xcrun xctrace record --template 'Time Profiler' \
  --device DEVICE-UDID --attach Kernarium --time-limit 15s \
  --output .build/results/kernarium-geekbench-clang.trace
```

Attaching Instruments perturbs the measured process, so scores from that run
are workload context rather than acceptance evidence.

For a diagnostic run created with `--profile-dir`, keep analysis outside the
scored comparison and use the checked-in readers directly:

```sh
python3 tests/perf/analyze_geekbench_profile.py PROFILE_DIR
python3 tests/perf/analyze_geekbench_workloads.py PROFILE_DIR
python3 tests/perf/compare_geekbench_profiles.py BASELINE_DIR CANDIDATE_DIR
```

The first command summarizes host utilization and aggregate stacks, the second
groups hotspots by workload, and the third compares completed workload timing
and host-core use between two diagnostic captures.

## Focused assembly probes

[`probes.tsv`](probes.tsv) is the complete inventory of the freestanding
assembly payloads in this directory. Each row records the mechanism measured,
minimum guest instruction feature, common runner, and why the probe remains a
useful control. The inventory is intentionally conservative: history and
source inspection found a distinct purpose for all 45 payloads, so none was
removed merely because its filename was not referenced by another source
file. The source-tree audit requires a one-to-one match between the inventory
and `guest_*.S`, validates the self-checking entry-point contract, and requires
the opt-in build owner below.

Build every payload without booting the emulator:

```sh
cmake --build .build/presets/macos-unit-16k \
  --target performance-probe-binaries
```

The binaries are written below `tests/perf/probes` in the build tree. They are
not CTest tests and are not dependencies of unit or integration suite
aggregates. Run one explicitly through the generic uploader:

```sh
python3 tests/perf/run_guest_link_bench.py \
  --emulator .build/presets/macos-integration/bin/uml-darwin \
  --rootfs rootfs.erofs \
  --binary .build/presets/macos-unit-16k/tests/perf/probes/guest_link_loop \
  --engine gadgets --cpus 10 --workers 10
```

A timing runner succeeds only after the measured command completes, the guest
halts within 60 seconds, and the emulator exits with status zero. A forced
`close()` remains cleanup for a failed run; it cannot turn that run into
performance evidence.

`guest_atomic_add_loop.S` is the sole payload requiring the host toolchain's
LSE assembler support. `guest_tlb_128m_alias_loop.S` is meaningful only for a
16 KiB guest build. The remaining payloads are usable with either supported
guest page profile. Benchmark execution still requires the integration EROFS
image and should record the same host/build/engine context as every other
campaign.

`guest_vector_store32_unsigned_offset_loop.S` and
`guest_vector_store32_register_offset_loop.S` are an instruction-for-instruction
matched cross-host pair for the gadget entries
`vector_store32_unsigned_fast` and `vector_other_store32_x_0`. Each binary
reports five in-guest `CLOCK_MONOTONIC_RAW` samples. Every sample retires
160,000,000 target `STR S` instructions in a 170,000,000-instruction timed
loop, so app boot, rootfs setup, and adaptive benchmark work counts do not
enter the measured interval. Run with one guest worker when comparing host
architectures; the generic runner prints each `UML_PERF_PROBE` record before
its ordinary host-side timing summary.

`guest_gadget_polymorphic_successor_loop.S` is the focused control for the
default-on `UML_GADGET_ADD_SUB_IMMEDIATE_SUCCESSOR_HOIST` optimization. Its
eight identical ADD-immediate predecessors all select one gadget entry, while
their successors select eight distinct gadget families. Compare separate OFF
and ON build trees on both host core classes when changing this mechanism.

`guest_simd_destination_residency_loop.S` is the sensitive probe for the
default-on `UML_GADGET_SIMD_DESTINATION_RESIDENCY` optimization. It repeats 32
integer MLA instructions with v0 as the destination and v8/v9 as nonaliasing
sources. `guest_simd_destination_residency_control_loop.S` keeps the same timed
instruction topology but places the destination at v8, outside the candidate
bank, with v16/v17 as sources. Compare sensitive and control probes in separate
OFF and ON build trees when changing this mechanism.

`guest_gadget_code_invalidation_loop.S` is the sensitive probe for
the default-on `UML_GADGET_EXACT_CACHE_VALIDATION` optimization. Each iteration
executes IC IVAU against unchanged code, advancing the code-change sequence and
forcing the gadget executor to validate or rebuild its cached programs. The matched
`guest_gadget_code_invalidation_control_loop.S` substitutes DC CVAU, retaining
the same cache-maintenance execution boundary without advancing that sequence.
Compare both probes in separate OFF and ON build trees.

`guest_fixed_x8_x9_link_loop.S` is the sensitive probe for the default-off
JIT x8/x9 cross-block carry experiment: its linked loop keeps exactly guest
x8 and x9 live. `guest_fixed_gpr_link_loop.S` is the historical x0/x1 loop and
matched overhead control; both probes now self-check both live values after the
loop. Compare them from the same OFF and ON build trees; a carry result is
meaningful only when the x8/x9 probe moves beyond the control's variance.
