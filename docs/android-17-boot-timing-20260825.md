# Android 17 4-vCPU / 2-GiB platform boot timing (2026-08-25)

## Result

The accepted ext4/APEX Android 17 guest reached init's first
`sys.boot_completed=1` action at these host wall times:

| Host product | First boot | Second boot | First-to-second saving |
| --- | ---: | ---: | ---: |
| macOS standalone | 788.926 s | 171.793 s | 617.133 s (78.2%) |
| iOS Simulator Kernarium | 109.657 s | 41.186 s | 68.471 s (62.4%) |

In this campaign the Simulator was 7.19 times faster on first boot and 4.17
times faster on second boot. These are observations of one first/second pair
per platform, not general product-performance claims. The macOS first boot is
also flagged: its initial `artd` process aborted once and Android recovered by
restarting it. The second macOS boot and both Simulator boots were clean.

The physical-iPad first boot measured immediately before this campaign used
the same release, engine, CPU, memory, page-size and storage-freshness policy
and completed in 525.731 seconds. Thus the observed macOS first boot was 1.50
times the iPad time, while the Simulator was 4.79 times faster than the iPad.
There is no matched physical-iPad second-boot sample yet.

## Measurement boundary and setup

The host timer starts immediately before process launch. Completion is the
first `ttyS0` line for Android init processing `sys.boot_completed=1` from
`/system/etc/init/hw/init.rc:1237`. Build, installation, the 30-second
post-completion dwell, diagnostic extraction, storage settlement and shutdown
are excluded.

Both products consumed accepted release
`android17-ack6.18-16k-20260825-8c5881ab-ext4-apex` at outer revision
`c529aeaa1a520ba0c0b870e170e72c8967b31a98`. It carries built-in-only ACK
`6.18.21-ga64f141f6266`, 16 KiB pages and the ext4 GPT/APEX payload policy.
Both products used:

| Property | Value |
| --- | --- |
| Execution engine | gadgets |
| Guest memory | 2,048 MiB |
| Guest vCPUs | 4 |
| Android boot mode | normal |
| Graphics | hardware Virgl through ANGLE/Metal |
| Density policy | point mode, 160 DPI |
| Storage | fresh COW for first boot; exact cleanly persisted COW for second boot |
| Logs | kernel/init on `ttyS0`; Android console/logcat kept separate |

macOS used an explicit 1133 x 695 surface, matching the physical iPad's
landscape point-mode run. The iPad mini Simulator was in its native portrait
orientation and reported 744 x 1081. Simulator orientation is not exposed by
`simctl`, and macOS Accessibility permission was unavailable for the UI
shortcut, so the run did not fake an orientation change. The Simulator surface
has 804,264 pixels versus macOS's 787,435, only 2.14% more; the material CPU,
memory, storage and density settings remain matched.

The host was a 64 GiB Apple M1 Ultra (`Mac13,2`, 20 processors) running macOS
26.3 (`25D125`). The mobile product ran on the iOS 26.5 iPad mini (A17 Pro)
Simulator with Xcode 26.6 (`17F113`). Both Simulator launches reported nominal
thermal state. The Simulator was shut down during both standalone macOS runs,
and the products were measured serially.

The macOS first run wrote the guest console through a dedicated Unix socket,
then executed `sync; reboot -p`; init reached `reboot: Power down` and the
process returned zero. Its second run reopened that exact COW. Each accepted
Simulator run was backgrounded through Settings after the dwell; Kernarium
reported both inactive and background storage settlement before termination.
The second run reopened the COW created by the accepted first run.

## Milestones

All values are host wall seconds from launch. APEX is the bootstrap completion
line for activation of all 67 ext4-backed packages.

| Milestone | macOS first | macOS second | Simulator first | Simulator second |
| --- | ---: | ---: | ---: | ---: |
| Kernel banner | 0.444 | 0.383 | 1.009 | 1.232 |
| Five GPT payload partitions mounted | 0.560 | 0.478 | 1.132 | 1.352 |
| 67 bootstrap APEX packages activated | 2.160 | 2.336 | 3.198 | 2.775 |
| Zygote started | 11.277 | 7.185 | 11.212 | 8.259 |
| SurfaceFlinger started | 12.663 | 8.568 | 12.467 | 9.407 |
| Boot animation started | 14.274 | 10.148 | 14.091 | 10.696 |
| `artd` first started | 30.510 | - | 29.972 | - |
| `artd` clean final exit | 513.657 | - | 104.400 | - |
| Codec-list generator exited | 578.723 | 28.597 | 94.466 | 28.998 |
| Boot animation exited | 772.129 | 161.210 | 107.346 | 40.036 |
| `sys.boot_completed=1` | **788.926** | **171.793** | **109.657** | **41.186** |

The initial path is not the bottleneck. All four boots mounted the five
immutable ext4 filesystems by 1.352 seconds and activated all ext4 APEX
payloads by 3.198 seconds. Once the Simulator's fixed application launch cost
is accounted for, both products remain close through Zygote, SurfaceFlinger
and boot-animation start.

## Dominant work

The following intervals overlap and are diagnostic boundaries, not additive
parts of the total:

| Interval | macOS first | Simulator first | macOS second | Simulator second |
| --- | ---: | ---: | ---: | ---: |
| First `artd` start to clean final exit | 483.147 s | 74.428 s | - | - |
| Codec-list service lifetime | 42.651 s | 6.389 s | 4.167 s | 3.333 s |
| Boot-animation service lifetime | 757.855 s | 93.255 s | 151.062 s | 29.341 s |
| Codec-list exit to boot completion | 210.203 s | 15.191 s | 143.196 s | 12.188 s |

Cold performance diverges in ART/package verification. macOS started `artd`
at 30.510 seconds. The initial process received `SIGABRT` at 443.417 seconds
after a reported 412.942-second lifetime; Android restarted it at 443.676 and
the replacement exited normally at 513.657. The persisted tombstone records:

```text
getAndExecuteCommand(fd=4) returned unexpected error -14, aborting
```

`-14` is `EFAULT`. This was a Binder-command failure in `artd`, not an OOM,
filesystem stall or timeout. Because the restart completed and the guest
subsequently booted and powered down cleanly, 788.926 seconds is the real wall
observation. It is not a clean macOS cold baseline, and the 483.147-second ART
window must not be interpreted as uninterrupted successful compilation.

The Simulator's first `artd` ran once and exited cleanly after 74.428 seconds.
Its codec-list generation took 6.389 seconds and Android completed 15.191
seconds after that service's first exit. The accepted cold repeat was also
consistent with a direct-termination preflight, which completed at 113.865
seconds; only the cleanly settled 109.657-second run is retained as the result.

Warm performance isolates a different boundary. Both products completed the
codec-list service at essentially the same wall point: 28.597 seconds on
macOS and 28.998 on Simulator. From there to boot completion, macOS took
143.196 seconds while Simulator took 12.188, an 11.75-times difference. The
warm platform delta is therefore in post-codec UI/system convergence and
boot-animation exit, not GPT mounting, APEX activation, ART or codec setup.

## Integrity and limits

Exact scans of the four accepted `ttyS0` logs found no EROFS use or
`erofs_worker`, RCU stall, unrecognized-userdata/full-device-encryption
fallback, Virtio-GPU `0x1204`/`0x1205`, OOM, oops, panic or Darwin-signal
signature. The sole matched error is the recovered macOS first-boot `artd`
`SIGABRT` above. Every run selected the hardware Virgl provider, brought four
vCPUs online and mounted all five immutable GPT filesystems as read-only ext4.

One first/second pair per platform identifies current phase distribution but
is not a statistically powered benchmark. In particular, do not normalize
away the macOS `artd` restart or use this cold pair to claim an intrinsic
7.19-times platform advantage. The clean warm pair does establish that the
current macOS post-codec/UI tail is the main second-boot difference.

## Retained evidence

Raw wall-prefixed logs and machine-readable summaries are under
`.build/android/measurements/platform-compare-ext4-apex-4vcpu-2g-20260825`:

- `macos-first` and `macos-second` are the accepted standalone pair; each also
  has a separate `hvc0-wall.log` with Android properties and ART logs, and the
  second additionally extracts the tombstone persisted by the first;
- `ios-simulator-first` and `ios-simulator-second` are the accepted,
  storage-settled Simulator pair;
- `macos-first-sandbox-preflight` stopped before kernel launch because the
  sandbox exposed no Metal device and is not timing evidence; and
- `ios-simulator-first-direct-terminate-preflight` is the consistent cold
  observation excluded because direct termination did not acknowledge storage
  settlement.

The preceding physical-device evidence remains under
`.build/android/measurements/ipad-ext4-apex-retry-20260825`.

The macOS COW and both Simulator bundles were removed after reduction, freeing
about 4.4 GiB while preserving the compact evidence above. They are not
recoverable in place, but the accepted release can recreate fresh equivalents;
the measured persisted states themselves are intentionally not retained.
