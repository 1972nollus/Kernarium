# Android 17 matched-platform boot timing (2026-08-24)

## Result

The matched Android 17 guest reached `sys.boot_completed=1` at these host wall
times:

| Host product | First boot | Second boot | First-to-second saving |
| --- | ---: | ---: | ---: |
| macOS standalone | 786.169 s | 84.795 s | 701.373 s (89.2%) |
| iOS Simulator Kernarium | 130.991 s | 42.780 s | 88.210 s (67.3%) |

The first-boot macOS observation is 6.00 times the Simulator observation; the
second boot is 1.98 times the Simulator observation. These are the measured
ratios for this campaign, not a general product-performance claim. The
macOS variability boundary is recorded below.

The time is not being spent in kernel startup, userdata recognition, late
mount, or initial graphics startup. Every run reached late mount by 7.866 s,
Zygote by 13.600 s, SurfaceFlinger by 15.386 s, and boot animation by
16.753 s. First-boot ART/package verification is the dominant cost. On macOS,
`artd` lived for 579.415 s and the interval after boot animation accounted for
771.291 s, or 98.1% of the complete first boot. On Simulator, the corresponding
quiet ART/package interval was about 66 s and the interval after boot animation
accounted for 114.237 s, or 87.2% of the complete first boot.

## Measurement boundary

Both products used the accepted release
`android17-ack6.18-16k-20260824-c3febf12-theme1`, produced from outer revision
`c3febf1273e129f43305f3c7cfb9f0bf0a03c8ca`. The host products were measured
at revision `5652dc6bd6d4f30e9a260dd3a877318d6977edda`. The ACK kernel is
`6.18.21-gc345c80dbb03`, built-in-only, and 16 KiB-only. The first run started
without an instance disk; the second run reopened the exact disk persisted by
the first run. Build and installation time is excluded.

The host timer starts immediately before process launch. Completion is the
first `ttyS0` line for Android init processing
`sys.boot_completed=1` from `/system/etc/init/hw/init.rc:1237`. This keeps the
boundary identical even though Simulator later emits
`VIRTUAL_DEVICE_BOOT_COMPLETED` at 152.195 s cold and 53.360 s warm. The
post-completion dwell, diagnostic extraction, guest sync/quiesce, and shutdown
are excluded.

The guest configuration was identical:

| Property | Value |
| --- | --- |
| Guest release and kernel | Android 17, ACK 6.18.21, 16 KiB pages |
| Execution engine | gadgets |
| Guest memory | 4,096 MiB |
| Guest vCPUs | 10 |
| Display | 440 x 860 at 160 DPI |
| Graphics | hardware Virgl through ANGLE/Metal |
| Logs | kernel/init on `ttyS0`; Android console/logcat kept separate |

The host was a 64 GiB Apple M1 Ultra (`Mac13,2`) running macOS 26.3
(`25D125`). The iOS product ran in the already booted iPhone 17 Pro Max iOS
26.5 Simulator with Xcode 26.6 (`17F113`). Both Simulator starts reported a
nominal thermal state.

## Milestones

All values below are host wall seconds from launch:

| Milestone | macOS first | macOS second | Simulator first | Simulator second |
| --- | ---: | ---: | ---: | ---: |
| Kernel banner | 0.544 | 0.765 | 2.089 | 1.826 |
| `/init` entry | 0.661 | 0.875 | 2.209 | 1.935 |
| Late mount complete | 5.659 | 4.793 | 7.865 | 6.688 |
| Zygote started | 12.043 | 8.181 | 13.600 | 10.020 |
| SurfaceFlinger started | 13.508 | 9.606 | 15.386 | 11.742 |
| Boot animation started | 14.878 | 10.692 | 16.753 | 12.598 |
| `sys.boot_completed=1` | **786.169** | **84.795** | **130.991** | **42.780** |

The interval breakdown makes the dominant region explicit:

| Interval | macOS first | macOS second | Simulator first | Simulator second |
| --- | ---: | ---: | ---: | ---: |
| Launch to kernel | 0.544 s | 0.765 s | 2.089 s | 1.826 s |
| Kernel to `/init` | 0.117 s | 0.110 s | 0.120 s | 0.109 s |
| `/init` to late mount | 4.997 s | 3.918 s | 5.656 s | 4.753 s |
| Late mount to Zygote | 6.384 s | 3.388 s | 5.735 s | 3.332 s |
| Zygote to SurfaceFlinger | 1.465 s | 1.424 s | 1.786 s | 1.722 s |
| SurfaceFlinger to boot animation | 1.370 s | 1.087 s | 1.367 s | 0.856 s |
| Boot animation to completion | **771.291 s** | **74.103 s** | **114.237 s** | **30.182 s** |

Simulator pays a fixed application/framework launch cost: its kernel banner is
about 1.3-1.5 s later than the standalone product. That cost is small compared
with the Android work after boot animation.

## Dominant work

On the macOS first boot, `artd` started at wall second 29.523 and exited at
608.911; init reported a 579.415-second service lifetime. The retained Android
logcat shows `pm.dexopt.first-boot=verify` and six completed `dex2oat64` jobs,
each configured for ten threads. Their reported wall times total 249.546 s but
overlap; their guest CPU times total 726.516 s. The longest single captured job
took 117.997 s wall and 443.086 s guest CPU. The verification stream includes
large SystemUI, PermissionController, and Bluetooth code sets. This is active
translated guest work, not a timeout or an idle wait.

After that compilation wave, the other material cold costs were the media
codec-list generator (32.759 s on macOS versus 5.429 s on Simulator), package
overlay/idmap work, initial theme application, and user-unlock/boot broadcasts.
On macOS, the theme observer registered at guest second 599.096, forced its
phase-600 fallback at 643.200, confirmed the initial overlay at 757.785, and
notified WindowManager at 760.687. The event buffer records enable-screen
progress at 744.817 and boot-animation completion at 764.022; Android completed
at 786.169 wall seconds. The fixed theme dependency cycle did not recur; there
was no 1,500-second multiplier wait.

On Simulator, `artd` started at 33.089 s. Serial output then went quiet until
system-service/HAL discovery resumed at 99.524 s, bounding a roughly 66-second
first-boot compilation interval. The media codec-list generator finished at
110.760 s and Android completed at 130.991 s. `artd` remained registered
through completion, so its process lifetime is not used as a phase boundary.

Neither second boot started `artd`; the compiled `/data` state removes the
largest first-boot cost. The remaining warm time is still overwhelmingly
after boot animation. In the macOS second boot, the phase-600 theme fallback
ran at guest second 27.845 and notified WindowManager at 28.017, screen enable
occurred at 55.936, animation completion at 75.334, and boot completion at
84.795 wall seconds. The fallback is prompt; the remaining cost is ordinary
system-server, UI, overlay, and user-unlock work under emulation.

## Interpretation and limits

- The repaired userdata path is not a significant contributor. Both fresh
  disks were recognised as clean; no invalid journal, unrecognised-filesystem,
  or full-device-encryption fallback occurred.
- Initial graphics startup is not the bottleneck. Both products selected the
  hardware ANGLE/Metal provider and started SurfaceFlinger and boot animation
  within 17 seconds. No Virtio-GPU `0x1204` or `0x1205` response occurred.
- First-boot `verify` dexopt is the largest removable-by-persistence cost. The
  first-to-second delta is 701.373 s on macOS and 88.210 s on Simulator.
- This campaign does not establish that macOS is intrinsically six times
  slower. Earlier accepted evidence for this same release measured 130.287 s
  fresh and 44.977 s warm on macOS. In the present matched-geometry campaign,
  however, two independent cold observations reported `artd` lifetimes of
  555.631 s and 579.415 s, and removing inherited Darwin background priority
  in an extra warm control did not improve it (95.652 s). The new results are
  therefore reproducible observations of this campaign, while the large
  cross-campaign variance remains unexplained.
- One first/second pair per platform is retained. These timings identify the
  phase distribution and current platform delta; they are not a statistically
  powered benchmark.

Exact scans of all four successful `ttyS0` logs found no RCU stall, watchdog
lockup, oops, panic, Darwin-signal failure, torn-userdata signature, or the GPU
response errors above.

## Retained evidence

Raw wall-prefixed logs and machine-readable summaries are below
`.build/android/measurements/platform-compare-theme-c3febf12-20260824`:

- `macos-first` and `macos-second` are the accepted standalone pair;
- `ios-simulator-first` and `ios-simulator-second` are the accepted Simulator
  pair;
- `macos-first-timeout-observation` is the independent 555.631-second `artd`
  observation whose observer ceiling expired before Android completed; and
- `macos-warm-foreground-control` is the scheduling-policy control.

The accepted macOS runs also retain separate `hvc0-wall.log` files with the
property, events, SystemServerTiming, and ART extracts. Disposable COW disks
and the Simulator bundle are not evidence and were removed after the traces
were reduced.
