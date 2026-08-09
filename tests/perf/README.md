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
The runner invokes the image-local payload at
`/opt/geekbench/release/geekbench6 --no-upload`, so it
does not prompt to upload results and has no network permission flag or
host/guest connectivity preflight. The shared
`systemd_overlay` boot mode uses the EROFS image as the lower filesystem,
constructs a tmpfs-backed OverlayFS from a temporary PID-1 shell, and makes
systemd the real PID 1 before measurement. The image owns the tty0 getty and
login policy; the runner does not replace it in the overlay. Do not replace
this with `overlay_shell`; historical comparison points used systemd. After
login and before starting either the optional profiler or Geekbench, the
runner stops Docker, its socket, containerd, and the `apt-daily` and
`apt-daily-upgrade` service/timer pairs. It verifies that none remains active,
then syncs the guest. This preparation is bounded by `--quiesce-timeout`
(120 seconds by default), and a failure or timeout aborts the run before any
measurement begins. All service-state changes live in the disposable overlay.

```sh
tools/rootfs/with-verified-rootfs.sh --rootfs rootfs.erofs -- \
  python3 tests/perf/run_guest_geekbench.py \
    --emulator .build/presets/macos-integration/bin/uml-darwin \
    --rootfs rootfs.erofs \
    --console-log .build/results/geekbench-gadgets-16k.log \
    --engine gadgets --cpus 10 --memory 8G
```

The payload is already present in `rootfs.erofs`; the runner neither downloads
it nor opts into an upload. No host-network grant is part of this local
campaign.

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
source inspection found a distinct purpose for all 37 payloads, so none was
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
tools/rootfs/with-verified-rootfs.sh --rootfs rootfs.erofs -- \
  python3 tests/perf/run_guest_link_bench.py \
    --emulator .build/presets/macos-integration/bin/uml-darwin \
    --rootfs rootfs.erofs \
    --binary .build/presets/macos-unit-16k/tests/perf/probes/guest_link_loop \
    --engine gadgets --cpus 10 --workers 10
```

`guest_atomic_add_loop.S` is the sole payload requiring the host toolchain's
LSE assembler support. `guest_tlb_128m_alias_loop.S` is meaningful only for a
16 KiB guest build. The remaining payloads are usable with either supported
guest page profile. Benchmark execution still requires the integration EROFS
image and should record the same host/build/engine context as every other
campaign.
