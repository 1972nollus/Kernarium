# Android 17 iOS cold-boot CPU profile (2026-08-25)

## Result

The accepted Android 17 guest was CPU-profiled through a fresh-COW cold boot
on the physical iPad and the iOS Simulator. Both products were Release builds
of the fixed host code, used the gadgets engine, 4 vCPUs, 2 GiB of RAM, 16 KiB
guest pages, normal boot, point-mode 160 DPI, and hardware Virgl.

| Host product | Profile wall | Process CPU | Average process cores | Attribution confidence |
| --- | ---: | ---: | ---: | --- |
| iPad mini 6, iOS 18.5 | 303.145 s | 573.213 CPU-s | 1.891 | Time Profiler; 95.916% of carrier CPU classified |
| iOS 26.5 iPad mini Simulator | 153.688 s | 251.115 CPU-s | 1.634 | `sample(1)` fallback; approximate, 0.810 active-sample/CPU cross-check |

These wall values delimit the profiled samples; they are not replacements for
the uninstrumented boot-timing results. Simulator sampling is particularly
intrusive: the same uninstrumented product previously completed cold boot in
77.500 seconds, while this sampled run took 153.688 seconds. The physical
profile also represents one cold run and has ordinary first-boot variance.

Real gadget execution is the largest CPU owner on both products. It consumes
356.384 CPU-s (62.17%) on the iPad and approximately 128.402 CPU-s (51.13%)
on Simulator. The iPad's next owner is memory/MMU runtime at 100.187 CPU-s
(17.48%). Translation plus software translation-cache lookup and maintenance
consume only 37.035 CPU-s (6.46%) on the iPad. The corresponding approximate
Simulator subtotal is 35.977 CPU-s (14.33%), driven by a larger late-boot
program-cache-maintenance share rather than by translation itself.

From `artd` start through boot completion, the iPad consumes 414.117 CPU-s
(72.24% of its whole-boot CPU) and Simulator consumes approximately 182.575
CPU-s (72.71%). The single largest interval on each product is `artd` start to
codec-list generation start: 228.461 CPU-s (39.85% of total) on the iPad and
94.301 CPU-s (37.55%) on Simulator. The next largest interval is codec exit to
boot-animation exit: 130.100 CPU-s (22.70%) and 50.630 CPU-s (20.16%). The
post-ART Android convergence path, not partition mounting or APEX activation,
therefore owns most cold-boot CPU on both products.

## Whole-boot CPU attribution

| Category | iPad CPU-s | iPad % | Simulator CPU-s | Simulator % |
| --- | ---: | ---: | ---: | ---: |
| Gadget compute/control | 245.990 | 42.91% | 87.477 | 34.84% |
| Gadget memory instructions | 110.394 | 19.26% | 40.925 | 16.30% |
| Translation/decode/build | 19.635 | 3.43% | 7.469 | 2.97% |
| Translation-cache lookup/validation | 9.767 | 1.70% | 8.287 | 3.30% |
| Translation-cache maintenance | 7.633 | 1.33% | 20.221 | 8.05% |
| Memory/MMU runtime | 100.187 | 17.48% | 41.888 | 16.68% |
| I/O/device paths on CPU | 5.775 | 1.01% | 15.357 | 6.12% |
| Native guest-kernel slice | 37.170 | 6.48% | 3.797 | 1.51% |
| Host synchronization | 1.138 | 0.20% | 3.860 | 1.54% |
| Other emulator runtime | 0.999 | 0.17% | 5.251 | 2.09% |
| Unresolved carrier | 23.152 | 4.04% | - | - |
| Host-system active work | 5.081 | 0.89% | 16.430 | 6.54% |
| Support threads | 6.292 | 1.10% | 0.154 | 0.06% |

The rows are mutually exclusive and sum to each product's process CPU. The
following definitions matter when interpreting them:

- Real gadget time is the sum of gadget compute/control and gadget memory
  instructions. These are samples whose leaf instruction is inside a generated
  `uml_arm64_gadget_*` body or its local assembly label; they are not time in
  the executor's outer dispatch loop.
- Gadget memory is the execution of guest load, store, atomic, exclusive, and
  prefetch instruction bodies. Memory/MMU runtime is the separate host work in
  guest address translation, TLB preparation, mapping batches, slow memory
  paths, and bulk memory operations. Their diagnostic memory-related subtotal
  is 210.581 CPU-s (36.74%) on the iPad and approximately 82.813 CPU-s
  (32.98%) on Simulator, but gadget memory remains part of the real-gadget
  subtotal too.
- Translation is instruction decode and program construction. Cache lookup is
  `ProgramCache::find`, stream lookup, validation, and shared-translation
  lookup. Cache maintenance is software program-cache indexing, publication,
  activation, deactivation, retirement, and link-record bookkeeping. It does
  not mean hardware data-cache or instruction-cache flushing.
- I/O/device time is CPU spent executing UBD, Virtio, serial, network, GPU, and
  host `read`/`write`/`pread`/`pwrite` paths. Blocked I/O wait is off-CPU and
  cannot appear as CPU time in this table.
- Native guest-kernel time is execution inside the signed ACK kernel mapping.
  Host-system work is active libc, libsystem, allocation, and similar work not
  assigned to a more specific emulator owner. Support threads cover the app,
  GPU service, console/I/O support, and UI outside the vCPU carriers.

The physical trace records 566.921 of 573.213 CPU-s on the four named vCPU
carrier threads. Thus 98.90% of all app CPU is on the carriers. The profile
averages only 1.891 active process cores despite a 4-vCPU guest, so CPU hot
functions alone do not account for all wall time; dependency chains, idle
periods, and off-CPU waits remain relevant.

## Physical-iPad stage breakdown

Percentages below are of each row's process CPU, not of whole-boot CPU.
`Gadgets` combines compute/control and memory gadget bodies. The remainder of
each row is host, support, synchronization, or unresolved work.

| Stage | Wall s | CPU-s | Avg cores | Gadgets | Translation | Cache lookup | Cache maintenance | Memory/MMU | I/O/device | Kernel |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Profile start -> kernel banner | 1.300 | 0.647 | 0.498 | 0.0% | 0.0% | 0.0% | 0.0% | 7.9% | 0.5% | 45.3% |
| Kernel banner -> system partitions | 0.126 | 0.074 | 0.585 | 6.8% | 8.1% | 4.1% | 0.0% | 16.2% | 9.5% | 2.7% |
| System partitions -> APEX activated | 5.731 | 10.900 | 1.902 | 39.5% | 4.3% | 1.9% | 1.7% | 32.1% | 2.1% | 6.4% |
| APEX activated -> zygote | 9.985 | 11.165 | 1.118 | 60.1% | 2.1% | 0.8% | 0.5% | 24.9% | 1.0% | 4.7% |
| Zygote -> boot animation | 40.802 | 69.243 | 1.697 | 58.9% | 1.8% | 0.5% | 0.5% | 23.2% | 0.8% | 7.2% |
| Boot animation -> `artd` | 47.622 | 67.067 | 1.408 | 65.9% | 2.6% | 1.9% | 1.5% | 14.9% | 0.7% | 4.9% |
| `artd` -> codec generation start | 104.543 | 228.461 | 2.185 | 67.8% | 2.0% | 1.3% | 1.1% | 16.8% | 1.7% | 4.1% |
| Codec generation start -> exit | 18.506 | 29.030 | 1.569 | 46.7% | 10.0% | 4.6% | 3.4% | 19.0% | 0.4% | 9.5% |
| Codec exit -> boot-animation exit | 60.945 | 130.100 | 2.135 | 59.4% | 5.6% | 2.3% | 1.7% | 14.6% | 0.3% | 9.3% |
| Boot-animation exit -> boot complete | 13.585 | 26.526 | 1.953 | 55.3% | 4.8% | 1.9% | 1.6% | 17.8% | 0.2% | 11.9% |

The iPad's gadget share is 58.9--67.8% through the four long initialization
stages. Translation is only 2.0% in the largest CPU interval. It rises to
10.0% during codec-list generation, but that service is only 5.06% of
whole-boot CPU. Software cache maintenance never exceeds 3.4% in a material
stage. I/O/device execution peaks at 2.1% after partition mounting and is only
1.01% across the whole boot, so it is not a dominant on-CPU iPad cost.

## iOS Simulator stage breakdown

The Simulator percentages are approximate. Five-second sampling windows were
intersected with the wall stages and scaled by 250-millisecond process-CPU
telemetry. Stages shorter than one sampling window, especially the first two
rows, do not have independent category resolution.

| Stage | Wall s | CPU-s | Avg cores | Gadgets | Translation | Cache lookup | Cache maintenance | Memory/MMU | I/O/device | Kernel |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Profile start -> kernel banner | 1.573 | 1.069 | 0.680 | 33.1% | 0.7% | 0.8% | 1.3% | 22.5% | 6.9% | 12.0% |
| Kernel banner -> system partitions | 0.144 | 0.093 | 0.647 | 33.1% | 0.7% | 0.8% | 1.3% | 22.5% | 6.9% | 12.0% |
| System partitions -> APEX activated | 9.210 | 12.096 | 1.313 | 33.9% | 2.0% | 1.4% | 3.1% | 25.9% | 6.5% | 7.2% |
| APEX activated -> zygote | 6.045 | 5.613 | 0.929 | 32.5% | 2.3% | 2.0% | 4.3% | 30.3% | 6.0% | 3.9% |
| Zygote -> boot animation | 7.884 | 15.554 | 1.973 | 30.3% | 3.3% | 2.9% | 7.3% | 29.9% | 6.0% | 3.1% |
| Boot animation -> `artd` | 27.576 | 34.114 | 1.237 | 59.3% | 1.9% | 3.7% | 6.6% | 13.0% | 6.7% | 1.2% |
| `artd` -> codec generation start | 47.697 | 94.301 | 1.977 | 65.9% | 1.6% | 2.5% | 4.4% | 10.8% | 6.9% | 1.0% |
| Codec generation start -> exit | 11.609 | 20.870 | 1.798 | 49.9% | 4.4% | 4.5% | 11.9% | 17.0% | 3.2% | 0.5% |
| Codec exit -> boot-animation exit | 31.662 | 50.630 | 1.599 | 37.0% | 5.1% | 4.5% | 14.4% | 20.3% | 5.1% | 1.0% |
| Boot-animation exit -> boot complete | 10.289 | 16.773 | 1.630 | 34.6% | 5.5% | 3.9% | 13.4% | 22.1% | 6.8% | 0.7% |

Simulator's translation share remains close to the physical result: 2.97%
whole-boot and 1.6% in the largest stage. Its program-cache-maintenance share
increases after codec generation begins, reaching 11.9--14.4% in the late
stages. A preliminary Debug-app capture showed the same pattern; the accepted
Release rerun excludes build configuration as its cause. Because Simulator
and device use different sampling mechanisms, however, the 8.05% versus 1.33%
whole-boot cache-maintenance delta is a direction for a future matched
Time-Profiler capture, not yet a precise cross-platform regression metric.

## Measurement setup and method

Both profiles consumed release
`android17-ack6.18-16k-20260825-87eaee1e-artpreopt1` and host gadget-cache fix
`051260e3`. Later outer commits before this campaign changed documentation
only. Each `.kern` bundle was copied from the same verified source bundle and
was recursively checked to contain its manifest, command line, and one guest
image with no `userdata.cow` before launch. Each profile created its own COW.
Existing Simulator and physical-device bundle siblings were preserved.

| Property | iPad | Simulator |
| --- | --- | --- |
| App configuration | Release | Release |
| Guest bundle | `AndroidCpuProfileDevice4CPU2G051260e3.kern` | `AndroidCpuProfileSimulatorRelease4CPU2G051260e3.kern` |
| Guest surface | 1133 x 695 | 744 x 1081 |
| Thermal state at launch | nominal | nominal |
| Virgl provider | ANGLE/Metal hardware | ANGLE/Metal hardware |
| Boot completion | first init `sys.boot_completed=1` action | same |

The two surfaces differ by only 2.14% in pixel count, matching the preceding
platform-timing campaign. Both logs confirm 4 CPUs online, 2,048 MiB of guest
RAM, and the expected ACK 6.18.21 kernel.

For the physical device, the app was launched stopped through CoreDevice. A
Time Profiler trace attached to that PID before its first instruction, then the
process was resumed. The trace was exported, process and named-carrier CPU
weights were intersected with wall-prefixed `ttyS0` milestones, and leaf plus
call-path symbols assigned each CPU sample to one mutually exclusive category.
The symbolication gate classifies 95.916% of carrier CPU; 4.084% remains
unresolved and is retained explicitly rather than redistributed.

The current Xcode 26.6 / iOS 26.5 Simulator combination repeatedly disconnected
Time Profiler during the DTServiceHub file-descriptor handshake, including
after short attach gates and a Simulator restart. ETTrace CLI capture was not
available in this environment. The Simulator fallback therefore launched the
Release app stopped, attached `sample(1)` before resume, collected consecutive
five-second windows at a one-millisecond interval, and recorded cumulative
target-process CPU every 250 milliseconds. Known blocked/waiting top frames
were excluded; active symbol shares were scaled to the measured process-CPU
deltas. Active samples account for 0.810 of measured process CPU, which is why
Simulator categories are labeled approximate. The Release rerun replaced an
otherwise complete Debug-app preflight.

The two tools expose different stack detail and treat short intervals
differently. Absolute physical-device categories and broad Simulator owners
are useful; small cross-platform category deltas are not exact A/B evidence.
Elapsed CPU seconds also combine work volume with per-core speed and are not
guest-instruction counts.

Exact log scans found no Darwin signal, kernel panic, oops, RCU stall, lockup,
OOM, EROFS worker, unrecognized-userdata fallback, Virtio-GPU `0x1204` or
`0x1205` response, ART abort, or watchdog signature in either accepted run.

## Retained evidence

Ignored local evidence is retained under:

```text
.build/android/profiling/cold-boot-20260825/
  device-cold/
    cold-boot.trace
    time-profile.xml
    toc.xml
    summary.json
    ttyS0-wall.log
    analysis/{cpu-breakdown.json,cpu-breakdown.md}
  simulator-release-cold/
    samples/window-*.sample.txt
    host-cpu.csv
    summary.json
    ttyS0-wall.log
    analysis/{cpu-breakdown.json,cpu-breakdown.md}
```

For each accepted profile, stage CPU totals and category CPU totals independently
sum exactly to the recorded whole-process CPU. The earlier
`simulator-cold-2` directory is the excluded Debug-app preflight and is not a
reported result.
