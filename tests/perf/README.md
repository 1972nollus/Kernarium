# Performance tools

This directory owns manual performance campaigns. It is deliberately outside
the unit and integration correctness presets: a benchmark result is meaningful
only with a recorded host, power state, build, rootfs digest, engine, topology,
and repeated matched runs.

Run the cheap entry-point check after changing shared integration helpers:

```sh
python3 tests/perf/test_cli.py
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
