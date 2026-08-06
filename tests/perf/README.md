# Performance tools

This directory owns manual performance campaigns. It is deliberately outside
the unit and integration correctness presets: a benchmark result is meaningful
only with a recorded host, power state, build, rootfs digest, engine, topology,
and repeated matched runs.

Run the cheap entry-point check after changing shared integration helpers:

```sh
python3 tests/tooling/performance/test_cli.py
```

Geekbench runs without a profiler by default. Pass `--profile-dir` only for a
separate diagnostic run; matched gadget/JIT score campaigns must omit it.
The runner uses the shared `systemd_overlay` boot mode: the immutable image is
mounted read-only, a tmpfs-backed OverlayFS is constructed from a temporary
PID-1 shell, and systemd becomes the real PID 1 before measurement. Do not
replace this with `overlay_shell`; historical comparison points used systemd.

```sh
tools/rootfs/with-verified-rootfs.sh --rootfs rootfs.img -- \
  python3 tests/perf/run_guest_geekbench.py \
    --emulator .build/presets/macos-integration/bin/uml-darwin \
    --rootfs rootfs.img \
    --console-log .build/results/geekbench-gadgets-16k.log \
    --engine gadgets --cpus 10 --memory 8G --allow-network
```

`--allow-network` is mandatory. The runner checks host connectivity before
boot and guest DNS/HTTPS after systemd login, so a missing host-network grant
fails in seconds instead of after the benchmark has finished computing.

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
source inspection found a distinct purpose for all 30 payloads, so none was
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
  --rootfs rootfs.img \
  --binary .build/presets/macos-unit-16k/tests/perf/probes/guest_link_loop \
  --engine gadgets --cpus 10 --workers 10
```

`guest_atomic_add_loop.S` is the sole payload requiring the host toolchain's
LSE assembler support. `guest_tlb_128m_alias_loop.S` is meaningful only for a
16 KiB guest build. The remaining payloads are usable with either supported
guest page profile. Benchmark execution still requires the integration rootfs
and should record the same host/build/engine context as every other campaign.
