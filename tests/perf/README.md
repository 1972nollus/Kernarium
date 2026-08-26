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

Summarize an accepted or rejected trial in
[`../../docs/optimization-history.md`](../../docs/optimization-history.md).
Do not add raw profiles, temporary paths, or a separate investigation document
to the repository.
