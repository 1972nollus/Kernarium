# Android 17 build-time-ART platform boot timing (2026-08-25)

## Result

The accepted build-time-ART Android 17 guest reached Android init's first
`sys.boot_completed=1` action at these host wall times:

| Host product | First boot | Second boot | First-to-second saving |
| --- | ---: | ---: | ---: |
| macOS standalone | 72.858 s | 54.594 s | 18.264 s (25.1%) |
| iOS Simulator Kernarium | 77.500 s | 38.940 s | 38.561 s (49.8%) |
| Physical-iOS Kernarium | 384.418 s | 182.400 s | 202.018 s (52.6%) |

This is one fresh-COW and exact-same-COW pair per platform, not a statistical
benchmark. In this campaign Simulator cold boot was 6.4% slower than macOS,
while Simulator warm boot was 28.7% faster. The physical iPad was 5.28 times
the macOS time and 4.96 times the Simulator time on first boot; on second boot
the factors were 3.34 and 4.68. The physical device is therefore the slowest
host in both states even after ordinary application dexpreopt moved to build
time.

The six boots completed without a timeout or forced termination. Exact scans
found no Darwin signal, kernel panic, oops, RCU stall, lockup, OOM, EROFS
worker, unrecognized-userdata fallback, Virtio-GPU `0x1204`/`0x1205`, or ART
crash signature. This is the accepted replacement for the provisional timing
collected before the gadget cache slot-replacement crash was fixed.

## Measurement boundary and setup

The host timer starts immediately before product launch. Completion is the
first `ttyS0` line for init processing `sys.boot_completed=1` from
`/system/etc/init/hw/init.rc:1237`. Build and installation, the 30-second
post-completion dwell, storage settlement, shutdown, and process exit are not
part of boot time.

All products consumed release
`android17-ack6.18-16k-20260825-87eaee1e-artpreopt1`, whose guest payload was
produced from clean outer revision
`87eaee1e433ce8db423cf48819084755ebd5c014`. The Apple consumers were rebuilt
after host fix `051260e3` (`gadgets: rebind a cache slot taken by another
address space`). The two later repository commits before the physical run
changed only `AGENTS.md` transfer guidance.

The matched guest policy was:

| Property | Value |
| --- | --- |
| Execution engine | gadgets |
| Guest memory | 2,048 MiB |
| Guest vCPUs | 4 |
| Guest page size | 16 KiB |
| Android boot mode | normal |
| Graphics | hardware Virgl through ANGLE/Metal |
| Density policy | point mode, 160 DPI |
| Storage | fresh COW, then the exact cleanly persisted COW |
| Logs | kernel/init on `ttyS0`; Android console/logcat kept separate |

macOS and the physical iPad used a 1133 x 695 surface. The iOS 26.5 iPad mini
(A17 Pro) Simulator used its native 744 x 1081 portrait surface. That is
804,264 pixels versus 787,435, only 2.14% more. The physical device was a wired
iPad mini 6 running iOS 18.5. Both Simulator launches and both physical-device
launches reported nominal thermal state.

Each macOS run completed a Virtio-console `sync; reboot -p`; the guest reached
power down and the process returned zero. Each Simulator run reported inactive
and background storage settlement before successful termination. Physical iOS
cannot bind the UML management console, so after the dwell the harness sent
`SIGTERM` through the attached `devicectl` console; the app performed its host
flush and exited with the expected status 143. The physical second boot
successfully reopened the COW created by its first boot.

## Wall-clock milestones

All values are host wall seconds from product launch. A dash means that the
service did not run during that boot.

| Milestone | macOS first | macOS second | Simulator first | Simulator second | iPad first | iPad second |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Kernel banner | 1.815 | 0.486 | 1.109 | 1.110 | 1.264 | 1.217 |
| Five GPT payload partitions mounted | 1.919 | 0.593 | 1.243 | 1.226 | 1.369 | 1.323 |
| APEX activated | 7.302 | 4.851 | 6.334 | 6.138 | 7.918 | 6.644 |
| Zygote started | 10.922 | 6.702 | 10.708 | 7.997 | 29.520 | 8.501 |
| SurfaceFlinger started | 12.126 | 7.926 | 12.236 | 9.316 | 40.299 | 27.811 |
| Boot animation started | 13.613 | 9.261 | 13.810 | 10.564 | 65.357 | 29.823 |
| `artd` started | 29.528 | - | 29.704 | - | 123.225 | - |
| Codec-list generator started | 52.464 | 25.377 | 53.244 | 26.073 | 267.197 | 93.253 |
| `artd` exited cleanly | 69.789 | - | 78.802 | - | 273.593 | - |
| Codec-list generator exited | 59.809 | 33.218 | 58.794 | 34.152 | 299.578 | 149.505 |
| Boot animation exited | 71.416 | 37.891 | 75.910 | 38.103 | 371.464 | 178.202 |
| `sys.boot_completed=1` | **72.858** | **54.594** | **77.500** | **38.940** | **384.418** | **182.400** |

Simulator first boot reached the completion action 1.301 seconds before its
clean `artd` exit. The process remained alive for the full dwell, so both
events are retained rather than forcing service exit to be the completion
boundary.

## Dominant work

The following intervals overlap and are diagnostic boundaries, not additive
parts of the total:

| Interval | macOS first | macOS second | Simulator first | Simulator second | iPad first | iPad second |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `artd` start to exit | 40.261 s | - | 49.097 s | - | 150.368 s | - |
| Codec-list service | 7.345 s | 7.840 s | 5.550 s | 8.079 s | 32.382 s | 56.252 s |
| Boot-animation start to exit | 57.803 s | 28.630 s | 62.101 s | 27.539 s | 306.107 s | 148.379 s |
| Codec-list exit to completion | 13.048 s | 21.376 s | 18.706 s | 4.788 s | 84.839 s | 32.895 s |
| Boot-animation exit to completion | 1.442 s | 16.703 s | 1.590 s | 0.837 s | 12.954 s | 4.198 s |

On macOS and Simulator, first-boot ART remains the largest named worker even
after ordinary applications moved to build time, but it now runs for only
40--49 wall seconds and overlaps other initialization. On the physical iPad,
`artd` is much more expensive at 150.368 seconds and the entire UI/package
convergence keeps boot animation alive for 306.107 seconds. Physical warm boot
does not run `artd`; its largest directly measured worker is codec-list
generation at 56.252 seconds, followed by another 32.895 seconds from codec
exit to completion.

The clean macOS warm run still has a 16.703-second tail after boot animation
exits, versus 0.837 seconds on Simulator and 4.198 seconds on the iPad. Thus
build-time ART does not remove every host-specific UI/framework convergence
cost.

## Build-time ART result

The macOS first-boot Virtio-console capture records 22 unique runtime dex2oat
input locations. Every one is below an active `/apex` mount. There is no
runtime dex2oat input below `/system`, `/system_ext`, `/product`, `/vendor`, or
`/odm`, and there is no ordinary-partition "no usable artifacts" diagnostic.
Representative ordinary applications, including CredentialManager, Jelly,
LatinIME, Settings, SystemUI, and ThemePicker, are skipped because their
adjacent build artifacts satisfy the requested filter. The macOS warm capture
contains neither an `artd` start nor a runtime dex2oat command.

The pre-change capture exposed ten APEX-contained inputs; the accepted
candidate's complete capture exposes 22. The earlier ten were therefore an
observed lower bound, not the complete residual set. Pinned AOSP 17 still
deliberately disables dexpreopt for ordinary non-system-server APEX variants,
so the remaining first-boot work is within the already documented APEX
boundary rather than a regression in ordinary partition packaging.

In the macOS capture, the first APEX dex2oat command starts at guest timestamp
28.283 and the last performed result is reported at 45.777, a 17.494-second
guest-clock span. The separate hvc0 channel was drained independently, so its
host receive prefixes are not used as cross-platform wall milestones; the
wall-clock `artd` intervals above come from `ttyS0` init events.

## Comparison with the preceding release

The immediately preceding matched campaign used the same compute, page-size,
graphics, density, storage-freshness, and completion policies with release
`android17-ack6.18-16k-20260825-8c5881ab-ext4-apex`:

| Host and state | Pre-ART-placement result | Current result | Observed saving |
| --- | ---: | ---: | ---: |
| macOS first | 788.926 s | 72.858 s | 716.068 s (90.8%) |
| macOS second | 171.793 s | 54.594 s | 117.199 s (68.2%) |
| Simulator first | 109.657 s | 77.500 s | 32.157 s (29.3%) |
| Simulator second | 41.186 s | 38.940 s | 2.246 s (5.5%) |
| Physical iPad first | 525.731 s | 384.418 s | 141.313 s (26.9%) |

The old macOS first boot is not a clean causal baseline: its initial `artd`
aborted with a Binder `-EFAULT`, restarted, and eventually completed. The old
macOS warm run also had an unusually long post-codec/UI tail. The Simulator and
physical cold reductions are cleaner evidence that removing ordinary runtime
dex2oat helps first boot, but one pair per release cannot assign every second
of the observed delta to that one policy change. Warm differences in
particular are run-to-run convergence observations, not an expected dexpreopt
effect.

## Integrity, transfer incident, and retained evidence

All six accepted logs selected hardware Virgl, brought four vCPUs online,
mounted the immutable GPT payloads read-only as ext4, and followed the normal
userdata path. The cold boots created a COW; each warm boot reopened exactly
that platform's COW. The iPad COW was explicitly listed on-device between the
two runs.

During physical-device deployment, an incorrect `devicectl` directory copy
first flattened one `.kern` bundle into `Documents`, and a staged-parent retry
then replaced the contents of `Documents`. That removed three older on-device
timing bundles and their COWs; those persisted device states are not
recoverable, although their host-side compact logs remain. The current bundle
was then transferred and recursively verified with the correct wrapper and no
pre-existing COW. `AGENTS.md` now requires an explicit
`Documents/<bundle-name>.kern` destination, before/after sibling listings, and
forbids both a broad `Documents` target and staged-parent repair.

Raw wall-prefixed logs and machine-readable summaries are retained under:

```text
.build/android/measurements/
  platform-compare-artpreopt-fixed-4vcpu-2g-20260825-051260e3/
    macos-first/
    macos-second/
    ios-simulator-first/
    ios-simulator-second/
    ios-device-first/
    ios-device-second/
```

The macOS first and second directories also retain the separate hvc0 Android
console/logcat captures. These ignored artifacts are local evidence, not
repository source or release payloads.
