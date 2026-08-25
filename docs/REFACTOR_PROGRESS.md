# Refactor progress

## 2026-08-25: gadget cache slot-replacement link fix

- Fixed the `ProgramCache` slot whose memoised `LinkRecord` survived the slot
  being handed to a different address space at the same guest PC, which
  published one address space's translation into another's record and left the
  slot naming storage that retiring the previous owner destroyed.
- Closed the Android 17 Simulator `RenderThread` panic inside
  `ProgramCache::activate`; see G174 in `HOT_PATH_EXPERIMENTS.md`.
- Extended `arm64-gadget-cache-validation` with a deterministic
  cross-address-space replacement regression that fails against the defect.
- Both page-profile unit workflows, the focused test in both profiles and under
  16 KiB ASan/UBSan, and four rebuilt-Simulator Android launches pass. No boot
  timing is claimed; the earlier one-off macOS panic is a different signature
  and stays open.

## 2026-08-25: accepted Android build-time ART packaging

- Cleared the inherited `system_other` odex route in the exact-synced device
  BoardConfig without creating that image or changing compiler filters.
- Added resolved and generated dexpreopt policy gates plus exact adjacent
  ARM64 artifact coverage for the 26 ordinary first-boot applications.
- Added negative fixtures for a `SYSTEM_OTHER` payload, a missing required
  application preopt, and an incomplete `odex`/`vdex` pair. The focused
  `android17-builder-lane` test passes.
- Reused the preserved AOSP Ninja output for an 8-minute-16-second incremental
  build. Producer checks passed for 4,845 target-files entries, 1,206 AArch64
  executables, VINTF, the fixed GPT, and 69/69 ext4 APEX payloads.
- Independently verified and accepted
  `android17-ack6.18-16k-20260825-87eaee1e-artpreopt1` from clean outer revision
  `87eaee1e433ce8db423cf48819084755ebd5c014`. Its ACK ELF is unchanged. Matched
  cold/warm runtime measurements on all three Apple hosts remain the next gate.

## 2026-08-25: Android build-time ART investigation

- Traced first-boot dex2oat to an inherited `system_other` placement mismatch,
  not disabled dexpreopt: AOSP generates ordinary app artifacts but omits them
  because the board flag remains enabled while the product image is disabled.
- Counted 36 unique first-boot `verify` inputs in retained evidence: 26 regular
  partition APKs and ten APEX-contained APKs. The current target-files has 67
  ordinary APK entries but only six adjacent app artifact pairs.
- Defined the next local-only incremental change: clear
  `BOARD_USES_SYSTEM_OTHER_ODEX` in the exact-synced device overlay, add
  fail-closed build/package checks, retain current compiler filters, and
  measure the ten unsupported APEX jobs as an explicit residual. No Android
  source or build output was changed and no build was started. See
  [`android-17-art-build-time-investigation-20260825.md`](android-17-art-build-time-investigation-20260825.md).

## 2026-08-25: matched 4-vCPU / 2-GiB Android boot timing

- Measured the accepted ext4/APEX Android release on standalone macOS and the
  iOS 26.5 iPad mini Simulator with gadgets, 4 vCPUs, 2 GiB, 16 KiB, normal
  boot, point-mode 160 DPI, hardware Virgl, fresh first-boot COWs and their
  exact cleanly persisted second-boot state. macOS reached
  `sys.boot_completed=1` in 788.926 seconds cold and 171.793 seconds warm;
  Simulator measured 109.657 and 41.186 seconds.
- All four runs mounted the five immutable ext4 partitions by 1.352 seconds
  and activated all 67 ext4 APEX payloads by 3.198. The warm codec-list service
  ended at 28.597 seconds on macOS and 28.998 on Simulator, but the remaining
  path to completion took 143.196 versus 12.188 seconds. Post-codec UI and
  boot-animation convergence dominates the clean warm platform delta.
- Simulator cold `artd` ran once and exited cleanly after 74.428 seconds. The
  macOS cold result is flagged: its initial `artd` aborted after 412.942
  seconds because `getAndExecuteCommand(fd=4)` returned `-EFAULT`, restarted,
  and then completed. The measured 788.926-second cold result is real but not a
  clean baseline. Exact scans found no EROFS worker, RCU stall, vold fallback,
  Virtio-GPU `0x1204`/`0x1205`, OOM, oops, panic or Darwin signal.
- Full phase tables, limitations and evidence paths are in
  [`android-17-boot-timing-20260825.md`](android-17-boot-timing-20260825.md).

## 2026-08-25: Android GPT and APEX payloads moved to ext4

- Accepted
  `android17-ack6.18-16k-20260825-8c5881ab-ext4-apex` from clean outer
  revision `8c5881ab23bc1586c6cf2b0d67f201d8e13a9053`. The incremental AOSP
  build completed in 8 minutes 14 seconds and verified 4,608 target-files
  entries, 1,147 target executables, VINTF, and 69/69 ext4 APEX payloads.
- The first partition-only ext4 iPad retry mounted the five immutable GPT
  filesystems as ext4, then reproduced the RCU stall in an inherited EROFS APEX
  loop image at wall second 46.70. Product policy now selects ext4 for ordinary
  APEXes, generated AOSP route `p0008` changes the sole explicit
  `com.android.virt` selection, and target-files verification rejects EROFS,
  compressed APEXes, and other nested payload formats.
- The ACK source and ELF are unchanged. The new 16 GiB GPT compresses to
  704,869,601 bytes and passed producer, independent copied-release, release
  contract, and installed-release verification. The macOS Android target,
  Android device and Simulator runtime slices, XCFramework package, and signed
  physical-iOS app rebuilt against it.
- A fresh wired iPad mini 6 boot passed with nominal thermal state, gadgets,
  4 vCPUs, 2 GiB, 16 KiB pages, normal boot, and hardware Virgl. The five GPT
  payload filesystems mounted read-only ext4 by wall second 0.951 and APEX
  bootstrap activated 67 ext4-backed packages by wall second 2.640. First-boot
  `artd` took 316.137 seconds, codec-list generation took 30.170 seconds, and
  `sys.boot_completed=1` arrived at wall second 525.731. The guest remained
  stable beyond the 30-second dwell. Exact scans found no EROFS worker, RCU
  stall, vold fallback, Virtio-GPU `0x1204`/`0x1205`, OOM, oops, panic, or
  Darwin-signal signature.

## 2026-08-25: earlier minimal Android 17 overlay and recovery checkpoint

- Earlier accepted release
  `android17-ack6.18-16k-20260825-e3cc7bae-overlay2` from clean outer revision
  `e3cc7bae8636bc8d41f42f128138972dc211115b`. The Linux producer verified
  4,609 target-files entries, 1,147 target executables and VINTF against the
  unchanged 34-topic ACK final tree
  `621a0ab42d8990aa0960bf5d341d89b8f1042723`. The 16 KiB kernel reports
  `6.18.21-ga64f141f6266`, has modules disabled and retains common ABI v8 /
  `0x3871ad612337ea76`.
- Added the fresh-only exact-synced `aosp_kernarium_darwin` overlay. It keeps
  ThemePicker and LatinIME, forces the software keyboard visible with a host
  keyboard, removes lock/idle friction, and omits obsolete first-party apps
  and unimplemented hardware services while retaining the compatibility APIs,
  providers, WebView, PackageInstaller, Virtio network and Virgl graphics that
  ordinary Android applications use.
- Added LineageOS Jelly without vendoring its source. Build preparation fetches
  `lineage-23.2` commit `767833067da7bbbb639782bbddec33f602215c3d` and
  replays only the SDK-37 and geolocation-disabled-by-default topics to final
  tree `ce63d63703fbfd2e616e94513ea1e5c63ccae4ef`. Upstream optional camera,
  microphone and location declarations remain; the hardware-omission patch was
  removed.
- Published a schema-2 16 GiB GPT with ten partitions, including `init_boot`,
  writable `misc` and stock AOSP `recovery`. Normal and recovery ramdisks are
  verified ranges in the GPT, not standalone release or guest-bundle files.
  Explicit normal/recovery launch and the standard `boot-recovery` request are
  supported. OrangeFox is intentionally deferred.
- Fixed the new image's `system_server` restart loop by excluding the inherited
  Context Hub APEX, which advertised a Context Hub feature and then required
  the deliberately absent Wi-Fi framework service. A warm normal boot completed
  at guest second 100.21, held one stable `system_server` through a 30-second
  dwell, passed package/settings/feature/browser checks and powered down
  cleanly. Stock recovery started and shut down cleanly. One fresh run reached
  completion at 587.70 and then panicked at 594.44 on a kernel-mode Darwin
  signal 11; it did not reproduce, is excluded from passing evidence and
  remains an open host/kernel stability observation.
- Reverified the installed schema-2 release, all six focused Android
  policy/series tests and the macOS Android product. The Android Device and
  Simulator runtime slices rebuilt with passing architecture audits, and the
  complete three-runtime plus EGL/GLESv2 XCFramework package published
  successfully.

## 2026-08-24: matched Android macOS/Simulator boot timing

- Measured the accepted Android 17 ACK 6.18 16 KiB release with the same
  gadgets/4 GiB/ten-vCPU/440 x 860/160 DPI hardware-Virgl guest on the
  standalone macOS product and Kernarium's iOS 26.5 Simulator product. From
  host launch to init's first `sys.boot_completed=1` action, macOS measured
  786.169 s cold and 84.795 s warm; Simulator measured 130.991 s and 42.780 s.
- Every run reached late mount by 7.866 s and boot animation by 16.753 s.
  First-boot ART/package verification dominates: macOS `artd` reported
  579.415 s and Simulator's corresponding quiet interval was about 66 s.
  Neither warm boot started `artd`; media codec-list, overlay/idmap, theme and
  user-unlock work form the remaining post-animation cost.
- The 6.00x cold and 1.98x warm ratios are campaign observations, not intrinsic
  platform claims. Earlier accepted macOS evidence measured 130.287/44.977 s;
  the new long ART interval reproduced twice, but the cross-campaign macOS
  variance remains unexplained. Exact tables and evidence paths are in
  [`android-17-boot-timing-20260824.md`](android-17-boot-timing-20260824.md).

## 2026-08-24: connected-iPad GLES matrices and lifecycle gate

- Resumed the normally signed, single-worker physical-device campaign on the
  wired iPad mini 6 without changing the 2,048 MiB guest policy. Campaign
  `.build/vk-gl-cts-ios-device-matrix/gles2-16k-0a76cbc1-apple8-restored-resumed-2g`
  passed all 20 immutable shards and 9,460 executions: 8,760 `Pass`, 700
  released-policy `NotSupported`, and no disallowed result. Campaign
  `.build/vk-gl-cts-ios-device-matrix/gles3-16k-0a76cbc1-apple8-restored-resumed-2g`
  passed all 80 immutable shards and 129,960 executions: 125,270 `Pass`, 4,690
  released-policy `NotSupported`, and no disallowed result. Independent
  verify-only reconstruction accepted both complete products.
- Every one of the 100 shards waited for the public thermal preflight and found
  `nominal` on its first probe. The fail-closed joined report at
  `.build/vk-gl-cts-ios-device-matrix/qualification-0a76cbc1/thermal-lifecycle.json`
  authenticated every campaign, attempt, run, console, app, rootfs, device,
  and memory-policy input. All 100 launches shut down cleanly, and all 200
  managed-run start/finish samples were nominal. No emulator OOM occurred, so
  no 64 MiB memory decrement was authorized.
- These results close both connected-iPad Khronos matrices and the physical
  thermal/sustained-lifecycle gate. They do not promote conformance or a guest
  profile: the seven-pair performance campaign and recorded original-image
  provenance gap remain open.

## 2026-08-22: bounded signed-app GLES matrix authority

- Added checked schema-2 authority
  `tests/integration/gpu/vk_gl_cts_apple_matrix.json`. It freezes the truthful
  20-config digest and ordered IDs/surfaces plus every official GLES 2 and
  GLES 3 leaf case. The generator expands this to 100 immutable app campaign
  IDs: one full 473-case GLES 2 process and four hash-pinned interleaved GLES 3
  process slices per config. No app environment can supply a config ID,
  fraction, case, seed, runner argument, or engine. Aggregation reconstructs
  each exact 6,498-case GLES 3 leaf and verifies its frozen hash.
- The Simulator and device adapters now require `--qualification`, the checked
  ext4 derivative, the matching product config inventory, and the exact prior
  product's corresponding evidence before running one matrix ID. They reparse the QPA
  and accept only the successful statuses frozen by the Khronos runner oracle,
  rather than incorrectly requiring every conformant skip to be `Pass`.
  `run_ios_vk_gl_cts_matrix.py` schedules the compiled IDs on one Simulator by
  default or deterministically across as many as four unique booted
  Simulators, with at most one active shard per device. It retains every
  attempt permanently, rejects retry after an incomplete or failed attempt,
  revalidates all predecessor artifacts, and independently reconstructs the
  complete config-by-leaf aggregate. Physical-device campaigns remain
  strictly single-device and single-worker.
- Kernarium now defaults to 4 GiB on Simulator and 2 GiB on physical devices;
  CTS launchers export and record those values explicitly. Device campaigns
  accept only 512--2,048 MiB in 64 MiB steps: start at 2,048 MiB and create a
  new immutable campaign 64 MiB lower only after a confirmed emulator OOM.
  Simulator predecessors captured under another memory policy fail closed.
- Both mobile Apple-GPU prefixes reverified; all four runtime slices rebuilt
  with passing architecture audits; four-XCFramework packaging and the Debug
  Simulator app build passed. A negative preflight rejected the stale app, and
  the rebuilt app then passed the complete read-only GLES 2 preflight across
  its embedded IDs, sealed rootfs, signed inventory, and all 20 macOS
  predecessors. The registered safety suite passes 52 cases. The former
  one-process GLES 3 layout OOM-killed the 1.5 GiB guest; the first bounded
  Simulator slice completed all 1,625 cases without OOM but exposed eight
  depth/stencil shadow texture-repeat failures. Managed UTM WebKit topic
  `fff809d63b` now enables native Metal sampler comparison on modern arm64
  Simulator, whose virtual device omits the GPU-family reports ANGLE formerly
  required and therefore forced every comparison to `Never`. Both fixed
  inventories were recaptured under the explicit 4,096 MiB policy at
  `.build/vk-gl-cts-ios-simulator/20260822-131649-63237-16k-gles3-config-inventory-v1`
  and
  `.build/vk-gl-cts-ios-simulator/20260822-131735-63365-16k-gles2-config-inventory-v1`.
  The exact repaired GLES 3 `cfg00` fraction `0/4` then completed all 1,625
  executions with 1,563 `Pass`, 62 permitted `NotSupported`, zero failure,
  clean artifact export, and zero CPU-fallback frames at
  `.build/vk-gl-cts-ios-simulator/focused/gles3-shadow-sampler-fff809d/20260822-131903-63508-16k-gles3-matrix-cfg00-000of004-v1`.
  All eight formerly failing shadow cases now report `Pass`.
- Final Simulator campaign
  `.build/vk-gl-cts-ios-simulator-matrix/gles3-16k-ce8cfa0f-apple8-restored-4g-4way`
  ran 80 signed fresh-guest shards across four matching booted iOS 26.5
  `iPhone18,2` devices, with exactly 20 shards and at most one active guest per
  Simulator. It reconstructed all 20 frozen 6,498-case leaves and passed
  129,960 executions: 125,270 `Pass`, 4,690 permitted `NotSupported`, zero
  failures, zero dropped frames, and zero CPU-fallback frames. Every attempt,
  app runtime, and `simctl` command returned zero; independent verify-only
  reconstruction accepted all 80 shards. Together with the final 9,460-case
  GLES 2 campaign, this completes both 16 KiB Simulator matrices at the fixed
  4,096 MiB policy. At this checkpoint the connected-iPad matrices remained
  paused; the 2026-08-24 section above records their completion.
- The first 2,048 MiB connected-iPad GLES 3 `cfg00` fraction `2/4` exposed one
  real failure at
  `KHR-GLES3.core.internalformat.texture2d.rgb_float_rgb32f_linear`; the guest
  and emulator completed normally, so this was not an OOM and the device
  memory policy did not change. The A15/Apple8 Metal device does not report
  32-bit-float filtering through ANGLE's former capability query even though
  it executes the operation correctly. Managed UTM WebKit topic `66766da048`
  (stable patch ID `7491b897dad6da71dece5e35a4d32c3c2535fed3`) retains the
  native query and Simulator route while adding only the Apple8
  R/RG/RGBA32F filtering path. The rebuilt, normally signed app then
  passed the exact same 1,624-case slice at 2,048 MiB with 1,563 `Pass`, 61
  permitted `NotSupported`, zero failure, clean shutdown, and zero CPU
  fallback at
  `.build/vk-gl-cts-ios-device-probe/float32-force-56b25e98/20260822-154345-73520-16k-gles3-matrix-cfg00-002of004-v1`.
  This closes the focused finding; at this checkpoint the complete
  connected-iPad matrices remained required, and the 2026-08-24 section above
  records their completion.

## 2026-08-22: signed Simulator GLES configuration inventories

- Added two fixed, generated Kernarium campaigns,
  `gles2-config-inventory-v1` and `gles3-config-inventory-v1`, and taught the
  Simulator and device adapters to select only those identifiers or an
  existing allow-listed sentinel. No arbitrary script, CTS selector, config
  ID, or engine became app-controlled. Inventory runs are inputs to
  qualification and therefore reject `--qualification` themselves.
- Rebuilt and verified both mobile Apple-GPU prefixes, rebuilt all four
  page-profile runtime slices, repackaged the runtime/EGL/GLESv2 frameworks,
  and built the Debug Simulator app. On the booted iOS 26.5 iPhone 17 Pro Max,
  `.build/vk-gl-cts-ios-simulator/20260822-093844-4028-16k-gles3-config-inventory-v1`
  and
  `.build/vk-gl-cts-ios-simulator/20260822-093938-4195-16k-gles2-config-inventory-v1`
  each returned zero and captured the same 20 configs as its matching
  VNC/JIT/ten-vCPU macOS predecessor: IDs 1-4, 13-16, 25-28, 37-40, and
  49-52, all selecting `window` from window/pixmap/pbuffer support, with
  canonical digest
  `843544a7aef987426f74851a8802447a2428bde9ff9a5d893e1c2073ffbb2607`.
  Both used Gadgets in the signed app, the sealed ext4 lower read-only below
  OverlayFS, direct ANGLE/Metal presentation, and ended with zero dropped and
  CPU-fallback frames. The adapters reparse and authenticate predecessor QPA,
  config, GPU, native-frame, rootfs, and artifact evidence and now fail closed
  if the config tuples differ. The connected-device adapter accepted both
  Simulator records as predecessors in dry-run validation. These inventory
  runs did not themselves complete a case matrix; both final Simulator
  matrices are recorded above. At this checkpoint the connected-iPad matrices
  remained open; the 2026-08-24 section records their completion.

## 2026-08-22: truthful GLES depth inventory and complete macOS GLES matrices

- CTS exposed that the GLES Virgl table advertised
  `VIRGL_FORMAT_Z32_UNORM` through a 24-bit depth backing. Guest Mesa rejected
  depth blits for the resulting false 32-bit configurations before the host
  operation; direct ANGLE and instrumented Virgl probes confirmed genuine
  32-bit floating-point MSAA depth resolve works. Managed topic `3a287ef` and
  outer checkpoint `e741cbad` remove only the false GLES normalized-depth
  advertisement while retaining `Z32_FLOAT` and native desktop `Z32_UNORM`.
- The Release product now exposes 20 window/pixmap/pbuffer EGL configurations,
  IDs 1-4, 13-16, 25-28, 37-40, and 49-52, with canonical SHA-256
  `843544a7aef987426f74851a8802447a2428bde9ff9a5d893e1c2073ffbb2607`.
  The earlier 25-config inventory and GLES 2 campaign remain diagnostic
  history and do not qualify the current product. Current campaign
  `.build/vk-gl-cts/matrix/gles2-16k-e741cbad-z32-truthful` replaced that
  evidence and passed all 9,460 executions: 8,760 `Pass`, 700 released-policy
  `NotSupported`, no disallowed status, and 20 verified immutable shards.
- Clean detached campaign
  `.build/vk-gl-cts/matrix/gles3-16k-e741cbad-z32-truthful` passed all 129,960
  current 16 KiB macOS GLES 3 executions: 125,270 `Pass`, 4,690
  released-policy `NotSupported`, and no disallowed result. Independent
  verify-only reconstruction accepted all 20 immutable shards. Every guest
  used VNC, JIT, ten vCPUs, 8 GiB, acknowledged Virtio serial artifacts, and a
  disposable overlay above the sealed ext4 lower; that lower remained mode
  0444 at its authenticated SHA-256. The two current campaigns complete the
  16 KiB macOS GLES 2/GLES 3 matrices. Both final Simulator matrices are
  recorded above. At this checkpoint the connected-iPad matrices and the
  performance/thermal/lifecycle gates remained open; the 2026-08-24 section
  above closes the device matrix/thermal/lifecycle portion, while performance
  remains open. The
  current macOS Apple-GPU prefix was transactionally republished at the
  canonical path; its verifier, normal/sanitizer GPU workflows, both
  architecture audits, all selected unit tests, and both VNC smokes passed.
- The full GLES 3 campaign additionally drove four maintained dependency
  repairs: ES 3.0 multisample renderbuffer surfaces, representable GLES
  separate transform-feedback buffers, per-draw Metal transform-feedback query
  accounting, and strict non-finite shader semantics. Focused emulator reruns
  passed the implicated official cases. The old depth diagnostic is now
  classified as 15 `Pass` plus five permitted `NotSupported` results on
  no-depth configurations, and the repaired transform-feedback query passes
  all 20 configurations. The Simulator-only GLES 2 texture finding remains
  open for the mobile matrix.

## 2026-08-21: common Apple ANGLE/Metal graphics transition

- Replaced the split desktop CGL and mobile ANGLE products atomically with one
  centrally declared `apple-gpu` product, one offline builder, and verified
  macOS, Simulator, and device platform prefixes.
- Added a common fixed-layout `HostGlProvider` and shared Metal-texture
  presentation owner. AppKit/UIKit remain platform shells, while macOS VNC is
  available independently of the fixed ANGLE provider.
- Removed the desktop graphics dependency family, CGL/OpenGL.framework path,
  OpenGL texture/IOSurface bridge, layout-changing backend macro, and retired
  iOS-only graphics builder/CMake authority.
- Hardened installed-prefix and final-link verification for exact provenance,
  payload, platform, deployment target, install names, dynamic closure, and
  forbidden CGL/EAGL/Vulkan provider symbols, including static archives.
- Passed the dependency tooling suite, normal and ASan/UBSan GPU workflows,
  the rebuilt macOS integration product, all four Kernarium runtime builds,
  and four-XCFramework packaging from the final provider source.
- Promoted no guest API and made no rootfs or bridge-ABI change. A focused 16
  KiB macOS diagnostic reached DRM/Xwayland/virgl, measured desktop GL 2.1 and
  GLES 3.0 naturally, and passed one exact GLES 3 CTS case with complete
  artifact egress and clean shutdown. The identical signed-app 16 KiB
  sentinel then passed 1/1 on the iOS 26.5 Simulator and connected iPad mini
  6, measuring the same guest GL 2.1/GLES 3.0 result. The first iPad poweroff
  exposed an intermittent `drm_mm` managed-release warning. Kernel topic 0020
  now retires atomic Virtio-GPU state before unplug; both LLVM 22 page-profile
  kernels rebuilt, and three consecutive Simulator plus three iPad sentinels
  then passed and powered off cleanly. A schema-2 campaign authority then
  froze ordered five-case GLES 2 and twelve-case GLES 3 identity sentinels;
  both passed on 16 KiB and 4 KiB macOS, Simulator, and iPad with matching
  selections, clean shutdown, and zero CPU fallback. Three non-passes were
  encountered while choosing those bounded sets and none was waived; later
  macOS qualification classified or repaired the two host findings, while the
  Simulator-only GLES 2 texture result remained open at this checkpoint.
  Performance, sustained lifecycle, and mobile CTS matrices also remained open;
  qualification is blocked by those missing gates and the original image's
  absent authoritative build receipt. The former global inventory remains a
  diagnostic: native Debian reproduced its failure when the desktop-GL-based
  `dEQP-GL45-ES3` package requested an unsupported context after eight modules
  and 148,476 rows. The corrected `--inventory` path scopes unmodified `glcts`
  to `KHR-GLES2.*` and `KHR-GLES3.*`; a JIT/ten-vCPU emulator run against the
  sealed derivative passed exactly two modules and 6,972 cases with frozen
  SHA-256 `3ec390ed2d36f0fd8e32a02718e19c5750cf9381eab002aac9b71f848ebc6162`.
- Diagnosed the initial 4 KiB macOS Aquarium RCU stalls as generic nested
  `mmu_gather` promotion to a full-mm flush. Kernel topic 0021 retains precise
  Darwin nested ranges behind a default-conservative architecture hook. Fresh
  LLVM 22 replay/builds produced accepted 16 KiB and 4 KiB artifacts at final
  tree `436f25b3c4cad9cbbead1d37bad19e219ce4075d` without changing bridge ABI
  v8. Clean accepted-artifact Aquarium canaries passed on macOS at 2.533/2.867
  FPS and Simulator at 5.767/5.933 FPS for 16 KiB/4 KiB, respectively, with 30
  samples, clean shutdown, and zero dropped or CPU-fallback frames. These are
  single implementation canaries, not the seven-pair performance or physical
  iPad thermal/lifecycle gate.
- Added an opt-in common provider/capset report and final native-display
  counters. The host requests ES 3.0, 3.1, and 3.2 independently, emits sorted
  extensions plus complete raw Virgl capsets, and the runner strictly decodes
  every known v1/v2 field. Simulator now requires a passing native macOS
  gadgets predecessor; iPad still requires the matching Simulator run. The
  accepted ordered 16 KiB chain passed 1/1 and cleanly shut down on all three
  products. Each created ES 3.0, rejected ES 3.1/3.2 with `EGL_BAD_MATCH`, and
  reported zero CPU fallback. Exact semantic capset differences are retained
  instead of requiring hardware-specific masks and limits to be byte-equal.
  Device capability records use the stdout/guest-console stream after a real
  iPad capture proved that `devicectl` can merge separate stdout/stderr streams
  between bytes.
- Added one common allow-list for signed-app CTS campaigns, generated the exact
  page-specific guest launcher into all four Kernarium runtime slices, and
  added thin Simulator and connected-iPad wrappers. Both reject arbitrary
  selectors and non-`Pass` results; the device wrapper also requires a matching
  passing Simulator QPA. The schema-2 allow-list supports exact ordered
  multi-case sets and predecessor revalidation covers campaign/leaf/seed,
  selected-list, QPA, renderer, capability, and frame identity. Twenty-seven
  focused tooling tests, all four runtime
  builds, seven focused Xcode app tests, packaging, and the canonical GPU
  workflow passed. A normal 16 KiB Simulator boot reached direct ANGLE/Metal
  KDE presentation. The exact CTS adapter initially rejected echoed launcher
  text; marker recognition was tightened and regression-tested before the
  matched Simulator/device evidence was accepted. Shutdown validation now
  requires all clean-stop markers and rejects the former dirty DRM signature.

## 2026-08-20: versioned kernel ownership groundwork

- Moved the current stable source, generated series, complete configuration,
  toolchain, and standalone scripts under `kernel/mainline-6.18` without
  changing patch bytes, final-tree identity, products, or page profiles.
- Moved the unchanged bridge ABI and 16 KiB page/task-stack geometry under
  `kernel/common`; series verification now requires an explicit line root.
- Scoped the current source configuration and Git cache to the stable line,
  while leaving the former ignored developer configuration untouched for
  manual migration.
- Converted active next-base results to local carry/drop/rebase/retest language.
  No Android, ACK, AOSP, guest-image, or Android product work has started.

## 2026-08-21: Android multi-vCPU cooperative timer checkpoint

- Diagnosed the Android RCU stall as a fixed-carrier timer-starvation loop:
  ACK's rwsem optimistic spinner remained in `darwin_cpu_relax()` while
  cooperative timers were serviced only at guest/kernel and idle boundaries.
- Added ACK-local topic 24, which retains immediate IRQ-enabled IPI service,
  recomputes the current CPU after possible migration, and polls the timer once
  per 256 spin checkpoints so `need_resched()` can release the carrier.
- Rebuilt the 16 KiB, built-in-only ACK ELF on the x86_64 Linux builder and ran
  a four-vCPU gadgets Android guest continuously to second 198 without an RCU
  stall. The prior kernel reproduced the failure at second 56–58.
- A later 25-topic soak exposed a second starvation path at guest second 816:
  uaccess and completed host-TLB retry boundaries called `cond_resched()`
  before the cooperative timer had raised `need_resched()` or awakened RCU.
- Added ACK-local topics 25 and 26. Safe, non-atomic uaccess and post-host-TLB
  boundaries now poll the cooperative timer before `cond_resched()`; atomic
  futex operations remain excluded. The regenerated 26-topic final tree is
  `d1a4d62471db6569cfe5a11e842a5a433d8d297e`.
- Published and installed release `20260821-5735514a-rcu-timer1`, then ran a
  four-vCPU gadgets cold boot to guest second 982 and a same-COW warm boot to
  second 284. Both dedicated `ttyS0` logs were free of RCU-stall,
  timer-wakeup, oops, and panic signatures and shut down normally.
- The separate `hvc0` Unix-socket transcripts returned an Android shell and
  `logcat -b main` output without Linux printk records. SurfaceFlinger still
  aborts repeatedly and `sys.boot_completed` remains unset, so graphical
  application readiness is explicitly not claimed.
- Diagnosed the remaining default-JIT four-vCPU stop as a host publication
  deadlock rather than another ACK timer failure. Native LSE atomic blocks
  entered their serialized cold fallback before their direct-write publication
  had unwound, allowing two vCPUs to wait on each other.
- Commit `2a28713a` defers that fallback until the block publication ends and
  adds a scalar-atomic code-generation regression plus a semantic-memory
  positive control. Both page-profile unit workflows passed (576/576 at
  16 KiB and 578/578 at 4 KiB); the sole tooling failure in each sandboxed
  matrix was the Unix-socket `EPERM`, and its exact rerun passed outside the
  sandbox.
- A fresh-COW four-vCPU default-JIT run of the accepted Android release kept
  the independent `hvc0` shell/logcat stream responsive through guest second
  319.98 and powered off normally at second 368.49. No RCU-stall, oops, or
  panic report was observed on `ttyS0`; the graphical SurfaceFlinger blocker
  remains unchanged.

## 2026-08-20: required local kernel source location

- Added the Git-ignored `kernel/mainline-6.18/source-location.conf` contract
  and tracked example/parser.
- Kernel preparation, CMake, standalone builds, delta audit, and patch
  regeneration now require the configured case-sensitive checkout.
- Removed the checked preset source path and made missing configuration a hard
  kernel-build failure.

## Git-only dependency provenance (2026-08-20)

External-library preparation now uses only upstream Git repositories and full
commits from the central `dependencies/manifest.tsv`; all iOS/macOS build
recipes, platform receipts, patch series, and CMake verifiers are consolidated
under `dependencies/`. Git-generated patches are maintained by rebasing topic
commits and running `dependencies/rebase-patches.sh`. The Linux source is owned
by `kernel/mainline-6.18` and starts from the pinned `linux-6.18.y` stable commit;
its 19 topics were rebased and regenerated with Git, producing final tree
`f78217b082f6becb8af1656bcc69521307eb729b`. Focused tooling and a fresh
case-sensitive replay without compilation passed.

## Desktop PipeWire userspace requirement (2026-08-19)

The host and kernel audio paths were present in a standard KDE boot, but the
current unified image has PipeWire/Pulse without a session manager and exposed
only Plasma's dummy output. A static sink/router experiment proved a PulseAudio
tone could reach the host, but it was reverted because device discovery and
routing belong to a packaged guest session manager. The authenticated rootfs
must add WirePlumber or an equivalent supported package; the macOS KDE and
Kernarium overlays do not synthesize that policy. The retained host PCM state
machine fix accepts VirtIO's repeatable SET_PARAMS/PREPARE transitions and
PREPARE after RELEASE, which the protocol test pins.

## Apple Virtio Sound playback (2026-08-18)

The macOS CLI and both Kernarium iOS products now share an in-process standard
Virtio Sound device backed by AVAudioEngine. It exposes one ALSA playback
stream—48 kHz, stereo, interleaved S16_LE—and keeps each TX descriptor pending
until AVAudio reports that period consumed. The shared vhost-user transport now
supports a device-selected queue count and asynchronous used-ring completion;
a four-queue fixture pins that ordering. Kernarium attaches sound by default,
while the macOS CLI and `tools/guest-shell.py` expose opt-in `--audio`.

The common kernel enables built-in ALSA PCM/timer and `SND_VIRTIO`, while the
obsolete UML hostaudio relay remains disabled. Both 16 KiB and 4 KiB kernel
rebuilds and macOS product links passed, as did the normal and ASan/UBSan host
workflows and compilation of the audio backend for macOS, iOS Simulator, and
physical iOS. End-to-end guest libasound probes on both page profiles wrote and
drained a 4,800-frame 440 Hz period successfully; the card appeared as
`VirtIO SoundCard`, and the formerly hardware-exempt `snd_timer01` LTP
regression passed on both. Both 16 KiB and 4 KiB Simulator runtime frameworks
linked.
The physical-iOS runtime source and audio archive compile, while its full local
link remains independently blocked by a stale pre-existing iOS network-prefix
source manifest.

## macOS WebGL instancing shader compatibility (2026-08-17)

The pinned desktop virglrenderer patch now treats `gl_InstanceID` as core when
it emits GLSL 1.40 or newer, instead of also requiring the legacy
`GL_ARB_draw_instanced` extension that Apple's core-only OpenGL 4.1 driver does
not advertise. A focused WebGL2 instancing A/B reproduced the illegal shader
and subsequent Virtio-GPU `0x1204` invalid-context replies with the prior
binary, then rendered 512 instances without either error on the rebuilt
binary. The rebuilt guest also rendered the exact current three.js instancing
performance page for 38 verified frames. Authenticated dependency preparation,
the full private-prefix rebuild, dependency tooling, architecture, and normal
plus ASan/UBSan GPU/VNC gates passed.

## Kernarium iOS application prototype (2026-08-15)

The `Kernarium` Xcode project embeds opt-in iOS device and Simulator runtime
slices and boots a 1.5 GiB UML guest by default from a selected Documents EROFS
image. Device and Simulator both default to all processors reported by iOS; a
settings sheet persists bounded vCPU, RAM, overlay-size, rootfs, and 4 KiB or
16 KiB kernel-page choices for the next launch, defaulting to 16 KiB. It also
persists Retina mode, which is off by default. Its basic SwiftUI `ContentView`
owns a safe-area `MTKView`; point mode maps one host point to one guest pixel
at 160 DPI, while Retina mode uses native-pixel dimensions and advertises
`160 * nativeScale` through Virtio-GPU EDID. UIKit scales touch coordinates
into the selected pixel space. The runtime selects VGA8x16 fbcon text in point
mode and the doubled TER16x32 font at Retina density. The real Virtio-GPU
device and virglrenderer use pinned UTM libepoxy plus ANGLE/Metal and present
the native Metal texture directly; the iOS target builds neither AppKit nor
VNC, while the macOS graph remains on its existing
AppKit/CGL/IOSurface/Metal and VNC paths.

Kernarium recreates a sparse temporary image on launch at the configured size
(8 GiB by default). The guest formats it as ext4 and uses separate OverlayFS,
Docker, and containerd directories on the same free-space pool; the app does
not create or activate disk-backed swap.
Both 16 KiB iOS slices and default macOS integration consume the same accepted
16 KiB mainline artifact; the 4 KiB iOS slices and macOS compatibility product
likewise share the accepted 4 KiB artifact. Root kernel CMake is import-only and
derives every ELF/support path from a locally installed, validated manifest;
source replay, toolchain selection, and Kbuild remain standalone. Kernarium
names the two signed
carriers `KernariumRuntime_6_18_41_16K` and
`KernariumRuntime_6_18_41_4K`, embeds both without linking either at launch,
and dynamically loads exactly the saved profile for the process lifetime. The
always-linked loader owns the pre-main `SIGALRM` mask and validates the
selected framework's release, page size, and immutable C API before the
runtime starts. Darwin links both profiles with 16 KiB signed-text boundaries;
packaging rejects unsafe executable/data host-page overlap. macOS embeds the
matching bytes through its existing unsigned CLI path. Both iOS targets use
the same scalable 60.3125--62 GiB native window for the 1.5 GiB default guest
and every product uses the 128 TiB sparse software-MMU task range. The native
window is guest RAM plus a page-rounded
one-eighth auxiliary allowance with a 32 MiB floor; 1.5 GiB therefore receives
exactly 192 MiB. macOS exercises the same scalable-window derivation,
relocation, and native-hole handling at a 96 TiB base. It retains the
standalone app's 8 GiB and ten-vCPU defaults, which derive a 9 GiB active
window. The common kernel enables zswap by default with zstd; normal desktop
launchers provide no backing swap, so no zswap pool is created.

The selected Documents image may now be `pmos-sxmo.erofs`. Its desktop
bootstrap recognizes OpenRC, preserves the image's UID-10000 account, tinydm
default session, and Sxmo/Sway entry point, and starts `/sbin/init`. Disposable
overlay policy enables the image's udev coldplug under UML, supplies minimal
no-swap deviceinfo, classifies Virtio input, and grants `video`/`input` device
access after udev settles. The common static kernel includes the inet-family
nftables expressions used by the image's default firewall. A 440x860 macOS
qualification completed DHCP, udev/coldplug, firewall startup, tinydm, and a
captured Sxmo/Sway frame using the production bootstrap.

Separate Virtio-input touchscreen and keyboard devices now receive UIKit
events, SLiRP supplies guest networking, and the app prevents automatic sleep
only while the guest is active. Settings and keyboard overlays are round
bordered icon buttons and use native circular Liquid Glass on iOS 26. Phosh
images use their stock Phrog/greetd VT session and preserve the image-owned VT
and serial getties plus login policy. Their disposable overlay removes only the
mutually exclusive development `phosh.service` enablement, letting the stock
display-manager alias select greetd/Phrog; the former wait helper and greetd
ordering drop-in remain gone. Kernarium systemd boots now append
`plymouth.enable=0` after a same-build Simulator A/B showed the image otherwise
blocking before greetd; macOS launchers and the direct Geekbench shell remain
unchanged. The disabled arm then authenticated through Kernarium's virtual
keyboard and rendered the logged-in Phosh app screen over the full 1320x2580
guest surface about 45 seconds after login, without another checked-allocation
trap. Both the
Phrog and logged-in Phosh Phoc configs are replaced by empty disposable files,
so neither stage overrides the Virtio-GPU EDID with the image's fixed
720x1440 mode. On macOS, the full Phosh service set now completes the GStreamer
VA probe, renders a VNC-captured lock screen, and enters direct
CGL/IOSurface/Metal presentation without claiming a host codec backend. A cold
1290x2796, 288-DPI run also
proved stock greetd starts once, Phoc accepts the EDID, fbcon owns Virtio-GPU,
and the guest exposes 64 standard VTs, four serial lines, and no legacy PTYs.
A matched native-Metal post-login control at 1320x2580 and 288 DPI recorded a
720x1440 Phoc framebuffer/plane/CRTC with the image session config and the full
1320x2580 geometry with both disposable EDID overrides, directly confirming
the quarter-screen cause and fix.
A 2 GiB configured run also cold-booted Phosh at 1320x2580 and 288 DPI on
the iPhone 17 Pro Max Simulator, reached graphical target, started stock greetd
and its Phrog session, and presented through the native Metal path.
Extended Virtual Addressing and file sharing remain project-owned metadata.
The DRM log client and UML stdio console are disabled; `tty0` renders through
fbcon and the host stdout bridge uses `ttyS0`, the available serial fallback in
the absence of a Virtio-console host backend.
Exactly one persistent host thread named `uml-vcpu-N-carrier` owns each vCPU,
including vCPU0; task creation no longer creates pthreads. Linux task contexts
migrate on 128 KiB kernel stacks, host-stop wakes carriers cooperatively, and
IRQ-enabled spin checkpoints service pending IPIs. The shipped scheduler policy
is 100 Hz with `NO_HZ_IDLE`. Kernarium keeps its launcher on the existing Swift
worker, masks the process-directed early `ITIMER_REAL` signal on application
workers, and lets only vCPU0's carrier unmask `SIGALRM` during calibration.
`CONFIG_PRINTK_TIME` supplies timestamps to both VT and host-serial kernel
output. In normal UML mode those prefixes now use a boot-relative host
monotonic nanosecond clock rather than 100 Hz jiffies; time-travel modes retain
their simulated clock and the tick/event policy is unchanged.

A full no-systemd gadgets requalification on the common architecture scored
96/565 single/multi, versus the final pre-change 96/567. Focused Clang/Object
Detection profiles found no material fixed-carrier spin/IPI cost and kept
activation+synchronization below 1.2% of accumulated guest+syscall time. Two
individual SDOT stream-load reductions stopped at 0.00% and 0.39% probe gains
and were removed; no macOS-only recovery path is retained.

The corresponding clean full 4 KiB compatibility run scored 92/417. That is
-4.17%/-26.19% against the current 16 KiB 96/565 and +16.46%/+0.24% against
the historical G35 4 KiB 79/416; the latter is not a strict A/B. A separate
unscored live mapping check placed the executable kernel at 96 TiB and its
8 GiB physical backing in the same high native window, not directly above
64 GiB. The scored process was not profiled or suspended and shut down cleanly.
The pre-dual-runtime 4 KiB acceptance passed 53/53 tooling checks and 5/5 fast
guest cases; the focused loader sanitizer gate and the 16 KiB 5/5 guest set
also passed.

A four-vCPU, multi-core-only follow-up completed all 16 Geekbench workloads
without executing the single-core suite. The current 16 KiB product scored
309 (263 integer / 415 floating point), versus 269 (226 / 373) at 4 KiB:
-12.94% aggregate, -14.07% integer, and -10.12% floating point. The workload
delta ranged from -1.23% to -35.36%; 4 KiB took 17.53% longer with only +0.35%
peak-resident-size movement. Both guests shut down cleanly. G148 records the
exact hashes and the ordered, active-host, single-observation limitation.

Five consecutive standard-signed Release launches of `phosh.erofs` on the
physical iPad mini 6 passed at 1.5 GiB and six vCPUs under the former
`96 * nativeScale` density rule. Each emitted timestamped printk records,
brought every CPU online, completed DHCP and the EROFS/ext4 pivot, started
stock greetd, reached graphical target, opened its session, and reported direct
virgl/ANGLE/Metal presentation at 2266x1390 and 192 DPI. The captured consoles
have no panic, POSIX-signal failure, allocator-corruption, or OOM marker, and
the device produced no new crash report. The host stopped the runs after stable
presentation, so orderly guest shutdown is not claimed; RAM stayed at 1.5 GiB
because no OOM occurred. A focused Debug check then proved the now-optional
`160 * nativeScale` Retina EDID value at 2266x1390/320 DPI on the iPad and
1320x2580/480 DPI on the iPhone 17 Pro Max Simulator. Both initialized
Virtio-GPU/fbcon and reached their direct Metal desktop paths. The latest
iPhone 17 Pro Max Simulator check additionally booted the new default point
mode at 440x860/160 DPI with a 55x53 VGA8x16 console and Retina mode at
1320x2580/480 DPI with an 82x80 TER16x32 console; both reached stock Phrog.
Its early records included non-tick-aligned timestamps such as
`[    0.004448]`. The current 2 GiB iPhone 17 Pro Max Simulator boot remains
additional EROFS/ext4,
VT/fbcon, stock-greetd, Phrog, and native Metal evidence. Current macOS
qualification proves the full
Phosh service set and its sparse rendered lock screen after the virgl VA-probe
fix. Physical Aquarium remains intentionally outside acceptance; the
removed disk-swap workaround is not part of the product. The earlier 2 GiB
Simulator WebGL 2 Aquarium result remains dated graphics compatibility
evidence.

Both rebuilt macOS page profiles then passed a focused gadgets shell boot on
`phosh.erofs`. The 16 KiB and 4 KiB guests completed DHCP and their disposable
overlays, emitted sub-10-ms timestamp digits (`[    0.004062]` and
`[    0.005343]` respectively), returned their command marker, and powered off
cleanly.

An opt-in physical no-systemd Geekbench launch also reached and completed 13
single-core workloads before the user stopped it during Photo Filter; that
unfinished memory-pressure phase is excluded and no aggregate or multi-core
score is claimed. Eight retained Time Profiler windows put non-carrier work at
no more than 0.040%, with more than 99.6% of active samples on A15 performance
cores and Nominal thermal state in the first and last retained traces. The
dominant costs are existing gadget assembly plus HTML5/Clang cache-link churn;
Object Detection separately exposes cold EROFS zstd decompression in the
native guest kernel. Candidate hot-path changes remain unimplemented pending
focused probes and matched representative A/Bs.

Targeted clean physical reruns narrowed G147's greater-than-10% workload gaps.
Asset Compression no longer reproduced the threshold at 84/83 versus macOS
89. Background Blur remained 96/93 versus 112 and HDR 79/75 versus 106.
Horizon Detection was bimodal at 51/143/51 versus 120, consistent with its
historical adaptive-duration behavior. Separate focused traces were thermally
Nominal, scheduled at least 99.398% of carrier samples on performance cores,
and kept non-carrier work below 0.03%. The strongest next measured candidate
is a profile-keyed MTE-disabled vector-memory path; HDR boundary/link counters
and Horizon byte-store locality counters precede any further code. No
hot-path optimization was accepted from the intrusive traces.

## Post-baseline JIT checkpoint (2026-08-12)

This checkpoint completes the foundational JIT-cache boundary: one authoritative
parsed `ncpus=`/`mem=` budget configuration, a complete 64 MiB private ceiling
per executor, a shared tier that alone receives additional guest-memory budget,
lazy shard backing, complete retained accounting, one compact arena-adjacent
activation anchor per direct target PC, and immutable shared validation words
with neutral relocation cells. Each installer regenerates its mutable
activation state; the canonical cache contains no executor address. Exact-match
and successful-materialization counters remain separate. A managed run
now owns at most its configured guest CPU count of session/profile-bound
executors and drains them before the runtime memory domain retires. The pool's
same-sequence migration regression is fixed by preparing each distinct
task-carrier `StaticGuestMemory` while keeping executor-TLB preparation
conditional on mapping changes.

The focused G138 diagnosis first observed Clang/Object Detection/summary
542/1139/786 at the compact 64 MiB geometry versus 460/856/628 before the
footer change. Deduplicating mutable anchors by target then produced
613/1238/871, +33.26%/+44.63%/+38.69% over the initial result. Profiling of the
compact precursor recorded 3,869 arena resets and 183,428,416 shared
materializations, 77.16%/62.54% below the first oversized-footer attempt.
Complete shared installation averaged 2,424.81 ns versus 16,322.84 ns for the
local compile path (6.73x faster). The final pre-shutdown sample peaked at
6.932 GiB RSS and no longer placed activation/deactivation in its collapsed
top-stack list, while precursor counters showed 3,285,847,540 accounted shared
bytes: the workload populated the lazy tier rather than merely reserving its
limit. A follow-up two-observation floor sweep selected 62 MiB: median
Clang/Object Detection/summary scores were 620.5/1255.5/883 versus
613/1242.5/872.5 at 64 MiB. A 60 MiB arm failed the Object Detection and
combined-score bands. The user retained the 64 MiB production ceiling because
62 MiB saves only 2 MiB per executor and leaves the whole-process budget
unchanged. These scores are focused observations, not broad acceptance.

Final JIT profiles are change-detected at the post-carrier explicit boundary,
which emits compile/overlap, pool, then shared before pool drain, and at the
idempotent app repeat. Compile/overlap and shared also retain normal-exit
callbacks; pool reporting is intentionally explicit-only because it requires
the quiescent pre-drain snapshot. Records go directly to the stderr descriptor
with a same-write leading LF. The writer resumes short or `EINTR` writes and
waits with `poll(POLLOUT)` after `EAGAIN`/`EWOULDBLOCK`; task carriers do not
take the shared stderr `FILE` lock.
`cmake --workflow --preset check-jitprof` passed 2/2 after the leading-LF
framing fix, including a full nonblocking-pipe fixture whose unterminated
foreign prefix is followed byte-exactly by framed records from both writer
APIs, and the 56-target / 50-layered-target / 57-registration architecture
audit.

The first current-source attempt is retained, unmodified and unscored, at
`.build/results/jit-cache-handoff/jit-profile-pool-shared-final-current-source-20260812/README.md`.
It was not retried: every status, workload, final record, and terminal-order
gate passed, but exactly 39 shell continuation prompts prefixed the first
otherwise complete compile record on physical line 188, so strict record
integrity failed.

The post-framing current-source diagnostic is accepted at
`.build/results/jit-cache-handoff/jit-profile-line-framing-current-source-20260812/README.md`.
All statuses were zero, the 39 prompts remained intact on their own line, and
all 1,450 profile records began exactly with `Darwin JIT` and ended in LF:
1,445 periodic compile, two periodic shared, and exactly one compile, pool, and
shared final, with nothing after shared. Direct occupancy reached
262,144/262,144; shared translation accounting reached
268,434,608/268,435,456 with zero insertion failures and zero retained failure
bytes. Clang 758 and Object Detection 1257 are diagnostic/unscored because this
was one run without a matched control. The earlier backpressure artifact remains
valid predecessor-source evidence but is not the current counter authority.
The saved-word
`A B B A A B B A` campaign likewise supports ownership/accounting rather than
a speed or RSS claim (-0.34% Clang, -2.16% Object Detection, +0.62% maximum
RSS, within the 2.8% score band). The global-mask executor-pool A/B is complete
and rejected. Its corrected exact-session-token repeat is also complete and
rejected: primary invocation-median P wall/host CPU are 22.7025/167.400 seconds
versus 22.2960/164.130 (+1.82%/+1.99%), and E wall/host CPU are
17.4315/32.650 versus 14.9345/25.675 (+16.72%/+27.17%). P halves are
+4.25%/+0.32% wall, while E halves reverse at -7.92%/+16.93%; both class
ranges overlap and recorded host load limits causal precision, so no structural
cause is claimed. The pool remains required for the guest-CPU executor cap and
budget correctness.

The unscored current-source mechanism trace is recorded at
`.build/results/jit-cache-handoff/executor-pool-mechanism-trace-final-setup-corrected-20260812/README.md`.
P/E recorded 18,002,647/6,002,142 borrows, 99.9971%/99.9998% remembered-slot
return, 0.0029%/0.0004% fallback, 1.000081/1.000005 cell probes per borrow,
1,461/35 failed CASes, no waiters, and only the initial 10/4 constructions.
This excludes frequent acquisition scanning, CAS retry, waiter traffic, and
reconstruction as explanations for the rejected A/B. The trace found exactly
10/4 nonnull sequence identities with only 0/2 wrapper sequence changes, yet
every identity used multiple slots and maximum fanout reached 9/4. Those
fanout values are lifetime ORed associations rather than borrow-weighted churn.
Linux's pointer identifies a vCPU interrupt source rather than a task,
address-space, or executor-cache key, and the wrapper-local preference missed
on only 0.002883%/0.000183% of P/E borrows. Sequence affinity is closed without
code. Reopening requires a zero-overflow borrow-weighted cross-wrapper trace
correlated with executor-cache consequences sufficient to explain at least a
2% effect, or a symbolized scheduling profile placing at least 2% of carrier
CPU in the corresponding post-handoff cold paths; lifetime fanout alone does
not qualify. Trace-build timings are not score evidence.

The fixed x8/x9 cross-link carry prototype also completed its missing 4 KiB ON
focused/architecture and 16 KiB ON ASan/UBSan focused gates, then a matched
matched P10/E4 2x2 campaign with symmetric x0/x1 control. P raw/normalized wall
improved 4.443%/5.091% and normalized CPU improved 5.614%. E raw improved
2.503%, but its control improved 3.100%, normalized wall regressed 0.616%, and
normalized CPU improved 2.025%. The original strict disposition remains P
PASS/E FAIL/overall FAIL. The user separately accepts the prototype under an
explicitly post-hoc policy allowing E regression below twice the measured P
gain. The option stays OFF: the E control confound, wall/CPU disagreement,
QoS-not-affinity topology, waived host load, and lack of a representative
workload remain visible. Results are recorded at
`.build/results/jit-cache-handoff/carry-x8-x9-matched-perf-final-20260812/README.md`.

G107's bounded 32-entry add/sub-immediate successor-hoist optimization is now
enabled by default. Fresh 16 KiB and 4 KiB Release OFF/ON trees passed its focused
structure/terminal checks plus architecture, and a fresh 16 KiB ON ASan/UBSan
tree passed the same focused tests. The P10/E4 campaign
retained all 96 scored samples. P polymorphic/control wall improved
13.723512%/0.516300% with 13.207211 points difference-in-differences; E improved
7.258795%/5.481481% with 1.777314 points. Polymorphic host CPU improved
13.888889%/9.683605%, all OFF/ON wall ranges are disjoint, and all halves are
beneficial. The prospective dynamic cross-core policy and the independently
preserved strict screen pass, while the stronger preference for at least two
difference-in-differences points fails on E overall and by half. A current
representative check improved full Geekbench Single-Core 94 to 97 and
Multi-Core 564 to 567; targeted Clang/Object Detection improved 438 to 441,
and rerun File Compression/HTML5 outliers stayed within 1.3% of OFF. Results
are under `.build/results/jit-cache-handoff/g107-successor-hoist-matched-perf-setup-20260812/`
and `.build/results/gadgets-optimization-20260813/`.

G140 closes the bounded G130 gadget-residency route. Four-or-longer nonfaulting
integer MLA runs with one v0-v7 destination and nonaliasing sources keep only
that destination in q24 and flush it before the translated boundary. The
sensitive probe improved 69.6% P and 43.5% E; its out-of-bank control moved
-1.1%/-5.7%. Targeted SIMD Geekbench changed 91 to 92. A clean full run changed
the G107 predecessor's Single-Core/Multi-Core 97/567 to 96/575. One earlier
full attempt was stopped after an RCU stall in Multi-Core Text Processing;
isolated and clean-full reruns completed that workload at 62/63. The bounded
selector is now ON by default; no cross-block or whole-bank state is implied.

The residual gadget bullets do not block this handoff. The historical
G8/G17/G36 E-core rerun is closed without a new measurement because none clears
the prospective `P > 2%` eligibility gate and G106 later measured the same
noncritical instruction-count class flat on P and E.
The broader whole-bank/cross-block vector-residency idea remains closed: G140
implements only the evidence-backed bounded destination-only family.

G141 now keeps decoded gadget programs across a same-generation code-change
notification only after exact page-bounded comparison of all consumed guest
words, with every direct and indirect activation reset before reuse. Its P/E
invalidation probe improved 62.4%/50.6%; targeted multi-core Clang/Object
Detection changed 394/461 to 383/464, and full Geekbench changed from G140's
96/575 to 96/567 (original baseline 94/564). Lazy grow-on-demand saved-word
storage replaced the first fixed-array prototype, and the selector is ON by default.
A live follow-up put decode at 2.36% of configured ten-vCPU capacity, so global
decoded templates are closed without code: their private stream copy and executor-link
relocation would consume the available gain.

The shortened sharing ON/OFF/OFF/ON full-Geekbench sequence found median peak
tree RSS 2.87% higher with sharing and noisy two-run score samples; it rejects
the requested memory exception without establishing a causal score delta. The
current 16 KiB host workflow passes 575/575 units and 47/47 tooling tests. The
4 KiB unit run passed 575/576 tests; its sole failure was the unrelated,
timing-sensitive `arm64-jit-executor-pool-multislot`, which passed immediately
when rerun alone, and the 4 KiB tooling suite passes 47/47. Every gadget test
passed in both page profiles. The Apple package workflow passes all three
slices and its installed, raw, and Xcode consumers. Full sanitizer and
complete integration matrices remain uncredited. The accepted
2026-08-10 baseline below is not relabeled as current-tree evidence.

## Current campaign result (2026-08-10)

The refactor campaign is accepted at implementation baseline
`783bfcf7e2447f8f981830a158bec3f402b08d97`. The final Linux source is the
14-topic replay at tree `5b7b50cd573b7a1077586955cad9cf13770df35d`,
bounded at 155 changed files and 6 outside-port files. The host/kernel bridge is
ABI v6 (`0x6524b65f20239ea1`); the installed public API remains v4
(`0xd9a69804d3303044`). G99, G100, G121, and the G123 forward repair remain in
the accepted implementation. The later unified-EROFS migration supersedes the
baseline's old partitioned-image workflow; it does not relabel the broader
baseline evidence as newly rerun.

Final local acceptance passed 572/572 ordinary 16 KiB units, 573/573 ordinary
4 KiB units, 51/51 tooling tests in each profile, 572/572 ASan+UBSan, 8/8 TSan,
1/1 malformed-loader ASan, and both normal and sanitized GPU gates at 5/5 units
plus 1/1 VNC tooling. Guest `integration-fast` passed 5/5 in each page profile;
the 10-vCPU SMP workload reached 8.19 and 8.03 effective host cores. All three
Apple slices passed package/consumer acceptance and formed the XCFramework; a
real Xcode consumer build succeeded.

A post-baseline macOS display addition introduces `native` and `vnc` provider
modes. `scripts/boot_kde_vnc.py` now selects a native Metal window unless
`--display vnc` is requested. The native Virgl path shares two IOSurfaces
between CGL and Metal, drops superseded ready frames instead of adding a FIFO,
and retains CPU readback only as fallback. This does not reattribute the dated
interactive KDE/VNC campaign as native-window acceptance. Both normal and
ASan+UBSan GPU workflows passed 5/5 units plus the 1/1 VNC smoke, and the
production CLI rebuilt successfully. A focused 800x500,
four-vCPU native run reached `graphical.target`, passed its overlay guard, and
reported direct CGL/IOSurface/Metal presentation before Ctrl-C. Native input
interaction and timed latency remain unmeasured.

For the EROFS migration, the final host workflows passed 572/572 units plus
49/49 tooling at 16 KiB and 573/573 plus 49/49 at 4 KiB. Both rebuilt
production page profiles then passed the regenerated five-test
`integration-fast` paths with raw `rootfs.erofs` at `/dev/ubda`: 5/5 in 65.42
seconds at 16 KiB and 5/5 in 69.33 seconds at 4 KiB. The corresponding
complete workflows passed 52/52 tooling checks in each profile. Their
overlay/systemd tests passed in 34.06 and 38.19 seconds with separate Docker
and containerd tmpfs mounts and an offline source-built hello-world container.
Provisioning, launcher, runner, emulator-option, and API contracts also pass.
EROFS itself supplies the read-only lower-filesystem contract, so mode,
`ubd0r`, and `ro` enforcement are gone. The 16 KiB profile records the upstream
experimental subpage-compression warning because the EROFS image uses 4 KiB
filesystem blocks; the 4 KiB profile does not. That artifact identity remains
historical evidence. Current guest runs use EROFS directly without per-run
hashing or a sidecar lock; only explicit provisioning reads
`tools/rootfs/manifest.json`.

The image now provides `/opt/ltp`; the manifests contain 2,428 verified and 135
disabled cases with no overlap. Exactly 88 cases were disabled because they
require deliberately absent HugeTLBFS, transparent-huge-page, or CAN policy.
Full LTP was not rerun for this migration. The credited final-image evidence is
44 focused gadgets-engine rows, all accepted, including `statx05` at 4/0 in
both profiles and `zram01` at 12/1 with rc 0 in 167.6 seconds. Geekbench uses
its local `/opt` payload with `--no-upload`, but the final scoring run remains
intentionally skipped and no new score is claimed.

This is local-only evidence: no hosted CI workflow exists. Input
materialization remained off for the full refactor acceptance and stays
opt-in; instruction evidence remains **0/983 variant-complete**. Source-size
guidance remains advisory.

[`REFACTOR_HANDOFF.md`](REFACTOR_HANDOFF.md) is the authoritative acceptance
record. [`docs/refactor-evidence.md`](refactor-evidence.md) is the detailed
evidence index; [`docs/testing.md`](testing.md) contains current commands
and acceptance boundaries.

## Seventh-audit progress snapshot (historical; superseded)

Status as of 2026-08-04: the seventh independent audit reopened repository-
owned work. Its highest-priority findings were an instruction-evidence oracle
that is reproducible but not independent, a ledger that proves one selected
case per instruction entry rather than all variants, erased instruction-fetch
fault direction, incomplete gadget exception cleanup, unbounded/fallible
memory-domain retirement, GPU shutdown and descriptor-ownership hazards,
incomplete suite build aggregates, and stale current-product evidence. Rootfs
file management remains explicitly deferred
to a later testing redesign. Work needing a special guest image, credentials,
physical devices, public services, interactive graphics, or controlled
performance hardware remains external acceptance rather than an unfinished
source change.
At the time, [`REFACTOR_HANDOFF.md`](REFACTOR_HANDOFF.md) was the authoritative
acceptance-level checklist.

[`docs/refactor-evidence.md`](refactor-evidence.md) is the detailed
evidence index. [`REFACTORING_PLAN.md`](REFACTORING_PLAN.md) remains the original
specification and records the clarified policy that generated build metadata
does not need to be byte-for-byte deterministic.

## Seventh-audit open work (historical; superseded)

- Replace the binding-derived allocation/feature generator with an independent
  authoritative form-to-allocation and allocation-to-feature oracle. Model
  dependent and reserved fields, and add whole-pipeline negatives that remain
  invalid after regenerating every test-owned catalog.
- Give every required valid/invalid instruction variant an independent identity
  and expected state, event, fault, and memory result. The current 983 unique
  records each credit only one selected case, so their checkmarks do not prove
  the entry's width/form/alias/state/feature partitions are exhaustive.
- Preserve instruction-fetch fault direction through interpreter, translated
  engines, TLB metadata, the C/kernel ABI, and Linux retry handling. Repair
  gadget exception cleanup so direct engine callers always receive materialized
  state with the host environment restored, and make interrupt-token
  registration failure deterministic rather than silently discarded.
- Replace permanent retired-domain tombstones with bounded, allocation-free
  identity/lifetime protection. Make GPU framed I/O stop-aware, define distinct
  service/guest endpoint ownership, reset device state across owners, and make
  command mutations exception-atomic with direct host-only tests.
- Turn the gadget and native-code implementation fragments into ordinary
  responsibility-owned translation units. Resolve included headers to engine
  ownership in the architecture audit, and run common conformance through the
  exact production engine compositions rather than test-local recompilations.
- Split mixed Linux topic 0002 into reviewable final-state concerns; make fresh
  integration presets accept the required case-sensitive kernel checkout; make
  every suite aggregate build every registered cross-directory fixture; and
  rebuild both kernels, the current CLI, all three package slices, and package
  consumers only after source work is complete.
- Make the symmetric engine-edge fixture and source-tree audit comfortably fit
  their registered timeouts. Remove the unreachable fatal gadget, production
  runtime/network test instrumentation, unconditional disabled-profiling work,
  unbounded profiler records, implicit registry destruction ordering, and the
  raw-CTest label-verification bypass. Clarify whether “Python-free unit” means
  test implementation/execution or also the configure/build toolchain.

Fresh seventh-audit host verification passed **562/562** 16 KiB units,
**563/563** 4 KiB units, and full ASan+UBSan **562/562** from current-source
build directories. Architecture, TSan **4/4**, loader ASan+UBSan **1/1**, and
normal/sanitized GPU ownership gates passed. The ordinary tooling run passed
**22/23**: the symmetric engine-edge
test timed out at 60.01 seconds but its exact command passed directly in 59.71
seconds, confirming a timeout-margin defect rather than a semantic failure.
Regenerating the 983-row allocation file was byte-identical at SHA-256
`0e383651df7e3d743ab7d7d4ffe2007b6ce41bd6bb09f1ffc8bdb9f0ca77d574`; that
only proves deterministic reproduction of the dependent method. No guest,
benchmark, current kernel/product, or rootfs campaign was used for this audit.

## Sixth-audit closure claim (historical; superseded)

The seventh-audit findings above supersede this completeness claim. The
following records what the sixth pass reported and remains useful dated
evidence.

- The ARM64 allocation catalog is generated independently with LLVM MC 22.1.8:
  983 rows, 79 distinct masks, and only 21 exact-word masks. The validator joins
  architectural form plus opcode provenance and seven negatives reject the
  formerly accepted self-consistent wrong-form case. Both page-size reports
  contain all 983 IDs and are byte-identical at SHA-256
  `d4bf8d90db910807e6adbf2bf19e06f1d32ccd98fd60cfa7b2d26a583d3ed139`.
- Native 4 KiB shadow state is domain-owned and retired on normal and abnormal
  run exit. Sequential, concurrent, and abnormal public-handle tests plus map,
  protect, unmap, and refresh fault injection prove exception containment,
  rollback, isolation, and retry. Native/non-JIT storage is data-only.
- GPU queue descriptors and VNC initialization use RAII/transactional
  ownership. Production replacement/failure/retry and exact-port-reuse cases
  run in canonical rootfs-free normal and ASan+UBSan GPU workflows.
- Gadget and JIT responsibilities are split into logical owners while
  preserving decoder order and the production JIT object's bytes. Cross-engine
  link/object/aggregate/include/source-include edges are rejected symmetrically
  by a positive fixture and 24 negative cases.
- The virtual-transport DMA-mask fix is independent Linux topic 11. Kernel and
  dependency policy reject extra unlisted patches. Queued-transmit restart is
  deterministic, and the NEON inventory now lives under `tests/unit/arm64`.
- Source sizing remains advisory: 1,060 files are reported, 60 exceed a
  guideline, 42 lack optional review context, and zero changed since review.

Fresh closure verification passed 562/562 16 KiB units, 23/23 tooling checks,
563/563 4 KiB units, 562/562 full ASan+UBSan, 2/2 focused native 4 KiB
ASan+UBSan, 4/4 TSan, 1/1 loader ASan+UBSan, 1/1 normal GPU, 1/1 sanitizer GPU,
and 1/1 rootfs-free network coverage. iOS device and simulator 21-target graphs
passed; archive scans found no executable publication outside the explicit JIT
arena. A fresh authenticated eleven-topic Linux replay passes exact topic-path
overlap policy, verifies the 162/13 delta, and reaches canonical tree
`834e5c5faa8fc4638c9fdcbd40a1f29f5c7c2d5d`.

No guest boot, Node.js, Go, browser, benchmark, or controlled performance
workload was run for this closure. Physical sandboxed/entitled iOS and other
external campaigns remain explicit acceptance work, not repository refactor
items.

## Fifth-audit closure claim (historical)

This section records what the fifth pass reported. The sixth audit above
supersedes its completeness claim; statements below use their original wording
so the implementation history remains understandable.

- Exact ARM64 evidence now joins every encoding to an independent 983-row
  allocation catalog and records exact required/observed host features. Six
  negatives cover encoding-only and unrelated-feature mutations. Both page
  sizes produce 983 stable IDs with 710 native executions and 273 exact skips.
- Native-code targets no longer link, embed, include, or call gadget-owned code;
  shared transfer/TLB/environment support is neutral, and the architecture
  audit rejects any future native-code-to-gadget edge. Interpreter and gadget
  paths contain no executable-memory publication primitive, W+X mapping, or
  RW/RX transition; explicit JIT ownership remains available.
- Context/provider publication is a rollback-safe joint transaction. MMU
  mappings, history, invalidation, stale batch identity, and native 4 KiB shadow
  state are exception-atomic under deterministic Nth-failure coverage. Queued
  embedded stop admission has deterministic wait-boundary coverage.
- GPU descriptors acquire RAII ownership immediately after `recvmsg`; repeated
  malformed, short, oversized, dispatch-exception, and allocation-failure cases
  do not leak. Startup failure injection is present only in a test variant.
- Evidence reset/production/validation has standalone presets, raw tooling is
  non-destructive, the loader has a canonical ASan+UBSan gate, and next-version
  policy emits tested carry/drop/retest/upstream actions for applied, contained,
  and conflicting patches. ABI, provider ownership, TSan, and replay docs
  matched behavior at that time.
- The default dependency prefix was rebuilt from pinned release inputs and
  passes its full verifier. macOS, iOS device, and iOS simulator archives pass
  architecture and raw/install-tree C/C++ consumers; iOS symbol audits confine
  executable publication to the explicit JIT arena.

Dated fifth-audit verification:

- deterministic 16 KiB evidence pipeline: **563/563** in 286.37 seconds;
- deterministic 4 KiB evidence pipeline: **564/564** in 279.60 seconds, with
  architecture at **553 targets / 585 registrations**;
- non-destructive raw tooling: **22/22**;
- default-prefix GPU ownership fixture: **1/1** after 64 repetitions per error
  class; GPU architecture at **562 targets / 586 registrations**;
- canonical malformed-loader ASan+UBSan: **1/1**; TSan passed exactly **4/4**
  in 4.44 seconds;
- authenticated Linux replay matches canonical final tree
  `834e5c5faa8fc4638c9fdcbd40a1f29f5c7c2d5d`; replay commit identity and build
  metadata remain intentionally dynamic;
- independent public and kernel bridge ABI v4 identities, 162/13 Linux delta
  policy, next-version/topology negatives, rebuilt dependency verification,
  exact 16/4 KiB active-import checks, pristine LibVNCServer 0.9.15, the then-current
  16 KiB production kernel/app final link, all three package graphs, and C/C++
  consumers pass; and
- source size remains advisory: 1,034 files, 53 above guideline, 42 without
  optional review context, and six changed since review.

No guest boot, Node.js, Go, browser, or benchmark workload was run during this
closure. Rootfs redesign and external/device campaigns remain outside this
repository-owned completion claim.

## Fourth-audit closure (historical)

The following was the reported end state of the fourth audit. It remains useful
dated evidence but is superseded by the later historical closure records and
the current seventh-audit checklist above.

- Exact instruction evidence uses a checked-in 983-row catalog, exact encoding
  and architectural-form joins, per-engine executed/retired results, declared
  aliases only, and four negative fixtures. Fresh 16 KiB and 4 KiB reports each
  validate all 983 IDs: 713 JIT executions and 270 exact feature skips.
- Every engine maps a valid implementation gap to the guest illegal-instruction
  event without retirement or fallback and remains reusable. Same-handle runs
  fail fast; different embedded handles remain process-serialized with
  stop-aware queued admission.
- Software-MMU state is domain/run isolated, C allocation failures are
  contained and retryable, and the ELF loader uses non-destructive bounded
  mappings with complete cleanup and malformed-image coverage.
- The installed emulator archive is provider-neutral. The macOS app owns and
  injects SLiRP, while networking-enabled raw and installed archives link from
  genuine C and C++ consumers. Public API identity v4 is independent from the
  kernel bridge v4 identity.
- SLiRP readiness, runtime target composition, test/fixture ownership, API
  documentation, GPU descriptor RAII, and final-link provenance now have
  focused acceptance tests. The final-link audit is an allowlist, including an
  arbitrary ambient-archive rejection fixture.
- LibVNCServer is the pristine pinned 0.9.15 release tree
  `3b0f5deb99f726cc0cfb963b783fcc396017fde8e646240a9ed3b01c18077a35`.
  A fresh verified prefix, loopback VNC smoke, and all package consumers pass.
- Linux replay enforces ten exact nonempty patch IDs, subjects/order, and no
  merges. The authenticated release replays to exact tree
  `834e5c5faa8fc4638c9fdcbd40a1f29f5c7c2d5d` over 162 paths, including removal
  of the obsolete `DARWIN_LIBELF_INCLUDE` hook.
- Expired weak registrations are pruned under 3,000-cycle churn, network
  implementation includes are private, and root `.DS_Store` debris is removed
  and rejected by the source audit.

Rootfs file management is deferred to a later testing redesign. Image hashes,
sidecar locking, maintenance serialization, and rootfs cleanup are not current
refactor exit criteria.

## Fourth-audit verification (historical)

- The then-current 4 KiB host units passed **562/562**; that audit's 16 KiB unit
  selection passed **561/561**, with focused MMU, GPU, VNC, lifecycle, gadget,
  and package checks repeated after its final edits.
- The embedded-loader fixture passes at the exact production link window and
  in a targeted ASan+UBSan build covering occupied-window refusal, malformed
  dynamic/relocation metadata, cleanup, MMU isolation, and run admission.
- Architecture audits pass for the 559-target unit/tooling/GPU graph, the
  tooling-disabled external-integration graph (39 targets / 10 registrations),
  and all three package product graphs.
- Public ABI v4 is fingerprint `0xd9a69804d3303044`, SHA-256
  `d9a69804d33030448b780d58c3f553c0e12e4f1e19ab37f89d2719b8b93115f1`.
  Kernel bridge ABI v4 is independently fingerprinted as
  `0xfe2d71a33e3d446e`.
- The fresh private dependency prefix passes source-tree, receipt, payload,
  archive-member, architecture, platform, and deployment-target verification.
  Build timestamps, identities, compiler notes, and binary hashes remain
  dynamic observations.
- The three arm64 package slices pass raw/install-tree C and C++ consumer
  links. iOS device and simulator symbol audits find no executable-memory
  publication primitive outside `jit_arena.cpp`; non-JIT paths perform no
  RW/RX transition and never map W+X.
- The source-size report covers 1,023 files: 54 above an advisory guideline, 43
  without optional review context, and five changed since review. These are
  informational, not line-count failures.
- The previously recorded rootfs mode, size, and digest remain historical
  observations only; rootfs management is no longer a refactor acceptance
  gate.
- Interpreter/gadget state and MMU storage remain data-only; executable
  publication is reachable only through the explicitly selected JIT arena.

## Preserved earlier regression matrices

These dated results remain useful broad evidence and were not deleted: normal
unit/tooling **580/580 at 16 KiB** and **581/581 at 4 KiB**; **562/562
ASan+UBSan**; **5/5 targeted TSan**; and fast guest integration **24/24** at
each page size. They are not substituted for the targeted current-source
acceptance tests in the handoff.

The preserved 16 KiB gadget Geekbench result is **80 single / 506 multi**
(result), +3.90%/-0.39%
against the immediately preceding 77/508 run. No heavy benchmark was rerun as
part of the fourth audit.

## External acceptance (historical snapshot)

An LTP-enabled guest image, physical sandboxed or
entitled iOS devices, opt-in public-network/interactive-GUI/rendered-browser
campaigns, and repeated controlled gadget/JIT performance runs remain external
prerequisites. They are not silent pass claims and are separate from the open
repository-owned seventh-audit checklist.
Rootfs file management is deferred rather than treated as external acceptance.
