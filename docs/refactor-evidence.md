# Refactor evidence

## Common Apple ANGLE/Metal transition (2026-08-21)

| Boundary | Current evidence and limitation |
| --- | --- |
| Product authority | One `apple-gpu` manifest membership owns UTM WebKit/ANGLE, libepoxy, and virglrenderer for macOS, Simulator, and device. One offline builder publishes three format-3 prefixes with the same `gl_provider=angle-metal` and component/feature contract. The ordinary macOS product no longer owns a graphics source or recipe. |
| Provider and presentation | Common `HostGlProvider` requests the hardware ANGLE Metal display and ES 3 contexts behind one opaque renderer pointer. Common `MetalPresentation` consumes same-device full-frame `MTLTexture` scanouts; AppKit/UIKit are shells, and macOS VNC is platform-selected. The focused provider test pins exact virgl context-parameter versioning, sharing, make-current, rejection, and teardown; the device fixture pins direct, dropped, unsupported, and counted CPU-fallback outcomes. |
| Negative closure | Prefix verification rejects wrong architecture/platform/deployment target, unexpected payloads, install names, `LC_SUB_CLIENT`, symlinks, provenance drift, and CGL/EAGL/Vulkan provider symbols in frameworks and static libepoxy/virglrenderer archives. Runtime verification accepts exactly flat authenticated EGL/GLES companions and rejects legacy desktop attribution or closure. Invalid existing prefixes are not replaced. |
| Dependency gates | Central manifest validation; real read-only `apple-gpu` and `ios-network` prepared-repository verification; all three installed Apple-GPU prefix checks; and the preparation, patch-regeneration, build-manifest, ordinary-prefix, Apple-GPU-prefix, and runtime-closure tooling scripts passed. The unchanged local desktop prepared-source root contains a pre-Git legacy zlib tree, so its `prepare.sh --verify` gate was not claimed; the installed desktop prefix was revalidated before linking. |
| Host gates | `check-gpu` and `check-gpu-asan-ubsan` each passed the architecture audit, seven GPU unit tests, and `virtio-gpu-vnc-smoke` 1/1. The final canonical GPU workflow's audited host graph reported 606 targets, 55 layered targets, and a 641-entry test manifest. Test children that exercise process death now `exec` before constructing Metal, avoiding unsupported post-`fork` Metal compiler initialization while preserving descriptor, signal, watchdog, and reset coverage. |
| Apple products | The final provider source rebuilt `uml-darwin` and passed its post-link Apple-GPU closure check. Both page profiles rebuilt for iOS Simulator and device. `Kernarium/scripts/package-ios-frameworks.sh` reverified the two mobile GPU prefixes and published the 4 KiB and 16 KiB runtime XCFrameworks plus EGL and GLESv2. Sandbox-denied CoreSimulator logging produced warnings during packaging but did not affect `xcodebuild -create-xcframework`, which exited successfully four times. |
| macOS guest diagnostic | `.build/vk-gl-cts/20260821-011205-82005-16k-gles3` records the 16 KiB JIT guest reaching `/dev/dri/card0`, `/dev/dri/renderD128`, the `uml` user's real Xwayland display, and virgl/Mesa 25.0.7. Natural discovery reported desktop GL 2.1, GLES 3.0, and no software renderer. Exact frozen case `KHR-GLES3.shaders.declarations.declarations.missing_variable_name_vertex` passed; the complete QPA SHA-256 is `ad22f96fe58ef0ee1b9ab5d65ff6e0676236a74daf7bfdff0f656141edf66a3c`, all framed artifacts verified, and the guest returned `result=0 exitcode=0`. This is a focused developer diagnostic, not a complete sentinel or must-pass matrix. |
| Simulator provider bring-up | A clean Debug app on the booted iOS 26.5 iPhone 17 Pro Max Simulator selected `KernariumRuntime_6_18_41_16K`, 1,536 MiB, four reported processors, 440x860/160 DPI, the exact hardware ANGLE/Metal marker, and direct Metal presentation. Linux exposed capset 1 version 1/308 bytes and capset 2 version 2/1,408 bytes, mounted the 3,802,316,800-byte EROFS image, started SDDM, logged direct virgl/ANGLE/Metal presentation, and visibly rendered the KDE desktop. `simctl terminate` and its console attachment exited zero after capture; that normal-desktop capture itself did not request orderly guest shutdown or CTS. |
| Signed-app CTS adapters | `vk_gl_cts_apple_campaigns.json` allow-lists only `gles3-declaration-sentinel-v1`. Configure-time generation embeds the existing bounded guest launcher with the exact build/leaf identity and compiled guest page size into both Device and Simulator runtimes. The Simulator wrapper forces gadgets and the selected runtime, captures console artifacts, and requires one `Pass`; the device wrapper accepts only a matching passing Simulator `run.json`/QPA before install. Echoed launcher source exposed an artifact-marker ambiguity on the first booted run; `cb95a004` restricts prefixed markers to service-style prefixes and adds the regression case while retaining fail-closed malformed-frame handling. Shutdown validation now requires all three clean-stop markers and rejects the dirty DRM manager signature. Twenty-two tooling tests, all four runtime builds and 38-target/49-layer audits, four-XCFramework packaging, seven focused app tests, and the 606-target/55-layer/641-test canonical GPU workflow passed. |
| Simulator CTS diagnostic | `.build/vk-gl-cts-ios-simulator/20260821-020501-10804-16k-gles3-declaration-sentinel-v1` records revision `cb95a004`, iOS 26.5 iPhone 17 Pro Max Simulator, 16 KiB pages, gadgets, the exact hardware ANGLE/Metal marker, direct presentation, both DRM nodes, real Xwayland, virgl/Mesa 25.0.7, desktop GL 2.1, and GLES 3.0. Exact case `KHR-GLES3.shaders.declarations.declarations.missing_variable_name_vertex` passed 1/1; selected-caselist SHA-256 is `e5085f4769ec3fff43c422398352d3a4d84b3a6a09681f9c43b7c1618516695e`, QPA SHA-256 is `3d41442aecdaa2864606fdafc6bcca288efd176164f48dc37594b25356a85f5e`, and renderer SHA-256 is `fa76d0271341f14bb9d6b2ec0769df1bfa270913951ecd4c0def142db4e0aa90`. Guest and runtime returned zero after orderly poweroff. |
| Connected-iPad CTS diagnostic | The device wrapper consumed that exact passing Simulator `run.json`/QPA before installing the standard-signed Release app on the wired iPad mini 6 (`iPad14,1`, iOS 18.5 build 22F76). `.build/vk-gl-cts-ios-device/20260821-020554-10897-16k-gles3-declaration-sentinel-v1` records the same rootfs, campaign, 16 KiB pages, gadgets, hardware ANGLE/Metal marker, direct presentation, DRM/Xwayland/virgl renderer identity, desktop GL 2.1, and GLES 3.0. The case passed 1/1; selected-caselist SHA-256 is the same `e5085f4769ec3fff43c422398352d3a4d84b3a6a09681f9c43b7c1618516695e`, QPA SHA-256 is `2b8f6a3b7ff8f64daeb9a5deee90834c8da8e14b097675947a6610872257dbaa`, and renderer SHA-256 is the same `fa76d0271341f14bb9d6b2ec0769df1bfa270913951ecd4c0def142db4e0aa90`. Guest/runtime returned zero; after poweroff the kernel warned `Memory manager not clean during takedown` in `drm_managed_release`, and stopping the completed console attachment left `devicectl` status 1/app signal 9. This is a passing CTS diagnostic, not clean lifecycle acceptance. |
| Kernel lifecycle repair | The configured maintained Linux checkout owns commit `cfeb4220a` (`drm/virtio: shut down atomic state before unplug`). Regeneration added topic 0020 with stable patch ID `b1b185072c88c6a649d3507e48b20653e3dc0997` and produced reviewed final tree `b7ebf6944c6ed87956d27992bbef32c84c3692f7`; the audited delta is 161 changed paths with 8 outside the port. The topic changes only `drivers/gpu/drm/virtio/virtgpu_drv.c`, is classified `drop-locally-if-contained`, and retires atomic scanout before device unplug. Configured-source verification, the exact 161/8 delta audit, the focused ordering test, and a fresh pinned-commit replay with full LLVM 22 builds of both canonical page profiles passed. |
| Post-fix Simulator lifecycle diagnostic | The rebuilt 16 KiB app passed the exact case and clean-shutdown validator in three consecutive iPhone 17 Pro Max Simulator runs: `.build/vk-gl-cts-ios-simulator/20260821-022946-37241-16k-gles3-declaration-sentinel-v1`, `.../20260821-023030-37331-16k-gles3-declaration-sentinel-v1`, and `.../20260821-023106-37406-16k-gles3-declaration-sentinel-v1`. Every run returned guest/runtime zero, selected-caselist SHA-256 `e5085f4769ec3fff43c422398352d3a4d84b3a6a09681f9c43b7c1618516695e`, renderer SHA-256 `fa76d0271341f14bb9d6b2ec0769df1bfa270913951ecd4c0def142db4e0aa90`, and no kernel warning; the final QPA SHA-256 is `1d31174d5173c6d3f6fa1b4931923db4c3932739115bec0144e9ae27e2d55a51`. |
| Post-fix iPad lifecycle diagnostic | The standard-signed rebuilt Release app passed three gated repetitions on the wired iPad mini 6: `.build/vk-gl-cts-ios-device/20260821-023151-37504-16k-gles3-declaration-sentinel-v1`, `.../20260821-023443-37729-16k-gles3-declaration-sentinel-v1`, and `.../20260821-023640-37843-16k-gles3-declaration-sentinel-v1`. All three passed 1/1, returned guest/runtime zero, met the clean-shutdown validator, and omitted both the dirty manager text and its DRM warning; the final QPA SHA-256 is `446f5dd5dffe646032c65b03cf49ee02a637b0277b65ea13fc32ad5b138f9682`. Stopping the completed `devicectl --console` attachment still records tool status 1/app signal 9 after the verified runtime result; it is not a guest failure. This bounded 3/3 repair check is not the plan's sustained lifecycle/thermal campaign. |
| Qualification boundary | One standard-kernel lifecycle topic changed, but no bridge ABI, rootfs, Mesa, libdrm, or CTS payload changed. No guest API, conformance, or performance claim is promoted. The complete capset/fixed-sentinel set, display-backed caselist, complete GLES 2/GLES 3 matrices, performance/thermal campaign, and sustained connected-iPad lifecycle remain outstanding; the frozen missing CTS source/build receipt keeps qualification fail-closed. |

## Shared Apple GL/GLES baseline (2026-08-20)

| Boundary | Current evidence and limitation |
| --- | --- |
| Frozen inputs | [`GL_GLES_BASELINE.md`](GL_GLES_BASELINE.md) records the clean pre-change revision, host toolchain, dependency split, kernel identities and both page-profile configuration digests, immutable rootfs/Mesa/libdrm identity, and predeclared performance corpus/noise/promotion budgets. |
| CTS identity | `tests/integration/gpu/vk_gl_cts_identity.json` records the installed CTS binary sizes, SHA-256 digests and build IDs plus the Khronos XML and leaf counts/digests. `vk-gl-cts-campaign-safety` rejects unsafe selectors, software renderers, missing DRM/Xwayland, retired CGL, truncated or incomplete QPA unions, and corrupt or unsafe artifact frames. |
| Qualification blocker | Read-only inspection found no authoritative build receipt tying the co-located 1.4.5.5-labeled source to the installed binaries. The display-backed complete caselist is also intentionally deferred until the common ANGLE/Metal path exists. Diagnostic development may continue, but official qualification is fail-closed and no guest image change is authorized. |
| Archived CGL result | The previously measured macOS guest GL 4.1/GLES 3.0 result is archival only. It is not a runnable lane, performance comparison, or qualification input. |

## Required kernel source-location evidence (2026-08-20)

| Boundary | Evidence |
|---|---|
| Local authority | `kernel/mainline-6.18/source-location.conf` is Git-ignored; its tracked example contains the only supported assignment schema. The former ignored `kernel/source-location.conf` remains developer-owned migration input. |
| Case sensitivity | `kernel/mainline-6.18/scripts/source-location.sh` creates a lowercase probe at the configured location and rejects an uppercase path alias. |
| Fail closed | `kernel-source-location-config` proves standalone Kbuild rejects a missing config and checked presets cannot bypass it with `UML_KERNEL_SOURCE_DIR`. |

## Git-only source replay evidence (2026-08-20)

| Area | Current evidence and boundary |
| --- | --- |
| External libraries | Central Git-only manifest validation, offline exact-commit preparation with ordered `git am` replay, concurrent publication, manual-patch-edit rejection, Git rebase/format-patch regeneration, build-recipe coverage, and private-prefix negative tests passed. The real macOS/iOS dependency products were not rebuilt in this provenance-only pass. |
| Linux | The 19 maintained commits were rebased with Git from the former imported release root onto stable-repository commit `2fe596715f840d053aed5cee5455f701bdcd2b50`, then regenerated by `git format-patch`. Stable patch IDs remained unchanged; final tree is `f78217b082f6becb8af1656bcc69521307eb729b`. Git materializer tests, series policy/topology, canonical checkout verification, 160/7 delta audit, and a separate fresh case-sensitive replay with `--skip-build` passed. Kernel compilation was not repeated. |
| Historical boundary | Archive/signature and prepared-tree evidence in dated sections below describes the superseded source model. Current preparation has no archive, detached-signature, extracted-root, packaged-tree, or manually edited patch input. |

## macOS WebGL instancing evidence (2026-08-17)

| Area | Current evidence and boundary |
| --- | --- |
| Negative control | `.build/repro-instancing-local-before-single-v2/20260817-163715/console.log` records the prior binary compiling a WebGL2 vertex shader that consumes `gl_InstanceID`. It reports `GL_ARB_draw_instanced is not supported` and then four Virtio-GPU `0x1204` replies. The page still produced its early JavaScript frame marker before the bounded runner stopped; that marker does not negate the host context error. |
| Fixed workload | `.build/repro-instancing-local-after-single-v2/20260817-163842/console.log` records the identical 512-instance workload on the rebuilt binary. It emitted `UML_WEBGL_SAMPLE_OK` at 30 frames with renderer `ANGLE (Mesa, virgl (Apple M1 Ultra), OpenGL 4.1)` and contains no illegal-shader, `ARB_draw_instanced`, or `0x1204` entry. The accepted nonblank capture is `captures/01-webgl2-gl-instanceid-reproducer.png`. |
| Reported page | `.build/repro-threejs-after-single/20260817-164538/console.log` records the exact public `webgl_instancing_performance.html` page building its 1,000-object `INSTANCED` scene, then emitting `UML_WEBGL_SAMPLE_OK` after 38 frames with the same virgl renderer. Its accepted capture is `captures/01-three-js-instancing-performance.png`; the log has no illegal shader or invalid-context reply. A matched prior-binary page run also passed and did not produce the negative signature, so this current page revision is positive coverage, not the negative control. |
| Browser boundary | The browser A/Bs used diagnostic `--no-sandbox --single-process --in-process-gpu` flags after ordinary Chromium child processes timed out before navigation in this checkout. The workload still used guest Mesa, the production Virtio-GPU transport, and host virglrenderer. No Chromium flag or launcher change is proposed. |
| Dependency provenance | The updated patch applied to the authenticated virglrenderer 1.3.0 archive and produced prepared-tree SHA-256 `af34da083f3461560b523097c66bb1f096a391215786f88532582a6c4d91a67c`. A full private static prefix rebuilt and passed its verifier. `dependency-source-archive-verifier`, `dependency-source-preparer`, `dependency-build-manifest`, `dependency-prefix-verifier`, and `dependency-runtime-closure` passed 5/5. |
| Host gates | The normal and ASan/UBSan GPU graphs each passed the architecture audit, all five GPU ownership tests, and `virtio-gpu-vnc-smoke` 1/1. The tests used the newly built private prefix rather than the now-stale default-prefix receipt. |

## Kernarium iOS application evidence (2026-08-15)

| Area | Current evidence and boundary |
| --- | --- |
| Display composition | The guest uses the production Virtio-GPU/vhost-user device and virglrenderer. Pinned UTM libepoxy creates an ANGLE EGL display using the Metal backend; virglrenderer exports the native Metal resource and `ContentView` presents it through `MTKView`. No fake GPU, codec backend, or iOS VNC backend exists, and the macOS AppKit/CGL/IOSurface/Metal and VNC providers remain separately selected. Virtio-GPU now advertises a checked base EDID containing its exact preferred pixel mode and effective density. A full macOS Phosh boot completed the GStreamer VA plugin scan without `CREATE_VIDEO_BUFFER`/`DESTROY_VIDEO_BUFFER` context errors, passed the rendered-frame VNC verifier with a captured lock screen, and reported direct CGL/IOSurface/Metal presentation in native mode. A matched 1320x2580/288-DPI native-Metal control isolated the post-login quarter-screen issue: the image's session Phoc config produced a 720x1440 framebuffer, plane, and CRTC at origin, while empty disposable greeter and session configs produced 1320x2580 for all three. |
| Guest shape and startup | The app now defaults to `mem=1536M`, all processors reported by iOS, and Retina mode off, with bounded settings applied on the next launch. Point mode maps safe-area points directly to guest/Metal pixels and advertises 160 DPI; optional Retina mode multiplies by `nativeScale` and advertises `160 * nativeScale`. A focused 16 KiB iPhone 17 Pro Max Simulator check booted point mode at 440x860/160 DPI and Retina mode at 1320x2580/480 DPI; both completed DHCP and the EROFS/ext4 pivot, reached graphical target, and rendered stock Phrog at full geometry. An earlier focused Retina check booted 2266x1390/320 DPI on the physical iPad mini 6. The earlier five-run standard-signed Release reliability campaign used all six iPad CPUs, rendered at 2266x1390 and 192 DPI under the former `96 * nativeScale` rule, completed DHCP and the read-only EROFS/disposable-ext4 pivot, reached graphical target, started stock greetd and its session, and reported direct virgl/ANGLE/Metal presentation. Each reported 1,520,944 KiB available from 1,572,864 KiB during kernel startup. The earlier 2 GiB iPhone 17 Pro Max Simulator Phosh boot remains separate dated evidence. |
| WebGL 2 | The earlier 2 GiB Simulator Aquarium extension created a live WebGL 2 context and emitted `UML_AQUARIUM_OK` for 500 fish at a 1024x1024 canvas with 5.000 average FPS (4.000 minimum, 6.000 maximum), renderer `ANGLE (Mesa, virgl, OpenGL ES 3.0)`, and version `WebGL 2.0 (OpenGL ES 3.0 Chromium)`. The host uses an exact upstream ANGLE capability backport and a focused virglrenderer ES 3 renderbuffer-MSAA patch, with no Chromium preload or guest-version spoof. This is dated Simulator graphics evidence, not a current physical-device acceptance workload. |
| Input, network, settings, and lifecycle | Independent in-process Virtio-input devices advertise an absolute touchscreen and evdev keyboard. UIKit scales safe-area touch points into the selected point- or Retina-pixel guest range and sends software-keyboard text; it requests first-responder status only while that keyboard is shown. The settings sheet persists vCPU count, RAM, overlay size, a validated Documents EROFS filename, either the 4 KiB or 16 KiB kernel profile, and Retina mode for the next launch; 16 KiB remains the kernel default and Retina remains off. The emulator's normal callback boundary uses the private static iOS GLib/libslirp prefix. The UIKit idle timer is disabled only during the managed guest run. |
| Console, VT, and first greetd start | Both page profiles build standard VT/fbcon with Virtio-GPU fbdev emulation and selectable VGA8x16/TER16x32 fonts, disable the DRM log client and conflicting UML stdio console, retain Unix98 PTYs without the legacy BSD bank, and limit UML serial lines to four. Point mode produced a 55x53 VGA8x16 console at 440x860; Retina produced an 82x80 TER16x32 console at 1320x2580 instead of the unreadable 165x161 8x16 fallback. Normal-mode printk timestamps now use boot-relative host-monotonic nanoseconds rather than 100 Hz jiffies; records such as `[    0.004448]` demonstrate microsecond-format digits below the old 10 ms quantum. With no Virtio-console host backend, `ttyS0` carries host stdout while `tty0` remains on fbcon. Two earlier-image stock-Phrog cold boots at 1290x2796 and 288 DPI kept `NRestarts=0`, reported 64 VTs / 4 serial lines / 0 legacy PTYs, and traced the former first-start `SIGHUP` to tty0 autologin `vhangup`. The updated image owns a ttyS0 serial getty instead. Kernarium preserves all image-owned getties and credentials, removes only the mutually exclusive development `phosh.service` enablement, and leaves stock display-manager/greetd/Phrog intact without the former wait/order workaround. A same-build iOS Simulator A/B showed Plymouth blocking before greetd and `plymouth.enable=0` reaching the full-geometry Phrog keypad, so Kernarium systemd boots now skip that splash path while macOS and direct-shell launches remain unchanged. The checked-allocation disabled arm accepted the image-owned password through the Virtio keyboard and displayed the logged-in Phosh app screen across the full 1320x2580 guest surface about 45 seconds later. The unchanged LTP `pty07` VT-resize race passed 1/1 and restored the active console. |
| Rootfs and app metadata | The earlier Simulator run copied the 3,802,316,800-byte `rootfs.erofs` into Documents. The app now enumerates regular `.erofs` files there and recreates one sparse temporary disk at the configured 1--32 GiB size (8 GiB by default); guest ext4 directories provide OverlayFS, Docker, and containerd state from one shared free-space pool. No disk-backed swap is created or activated. The built app plist contains `UIFileSharingEnabled` and `LSSupportsOpeningDocumentsInPlace`. The target and entitlement source record Extended Virtual Addressing; the shared scheme owns the unit and UI test actions. |
| postmarketOS Sxmo | `pmos-sxmo.erofs` was selected through the same production desktop bootstrap in a 16 KiB, 2 GiB, four-vCPU macOS qualification at 440x860 and 160 DPI. DHCP completed; the image's OpenRC udev, coldplug, settle, nftables firewall, and tinydm services all reached their successful states; and the VNC verifier received a complete Sxmo/Sway frame. The disposable overlay preserved the UID-10000 account and image-selected session while supplying only UML deviceinfo, udev classification, and post-settle group access. The host stopped the guest after capture, so an orderly in-guest shutdown and an iOS runtime launch are not claimed. |
| postmarketOS build gates | Both page-profile kernels rebuilt with the new built-in network policy. All four iOS runtime slices rebuilt and repackaged, generic Simulator and physical-iOS Debug app builds passed, and the arm64 Simulator app/test/UI-test bundle compiled. The current canonical workflows passed 575/575 units plus 49/49 tooling at 16 KiB and 576/576 plus 49/49 tooling at 4 KiB. The first 4 KiB pass encountered the known randomized native-shadow reservation refusal; its exact retry and the complete owning-workflow retry both passed. |
| Build and regression gates | All four current runtime builds passed: 4 KiB and 16 KiB for Device and Simulator. Packaging produced `KernariumRuntime_6_18_41_4K` and `KernariumRuntime_6_18_41_16K` XCFrameworks plus EGL and GLESv2, and the app has no launch-time dependency on either runtime. The iOS 26.5 iPhone 17 Pro Max Simulator Xcode run passed five app tests and six UI tests. Separate real launches selected each saved page size, logged the matching framework/API identity, brought all four vCPUs online, completed DHCP and the EROFS/ext4 pivot, registered GPU/touch/keyboard devices, and reached `graphical.target` plus greetd; the later point/Retina A/B reached Phrog in both modes. A generic physical-iOS Release build succeeded with standard Xcode signing and embedded both versioned frameworks. Current host workflows passed 575/575 units plus 49/49 tooling at 16 KiB and 576/576 plus 49/49 at 4 KiB; the first 4 KiB pass saw one transient native-bridge reservation refusal, whose exact rerun and complete owning-workflow rerun both passed. Fresh macOS gadgets shell boots then passed on both page profiles, including DHCP, overlay setup, command markers, precise timestamps, and clean poweroff. Both earlier full macOS integration builds passed 53/53 tooling and the interpreter, gadgets, JIT, and SLiRP guests; their only failure was the same external `rootfs.erofs` Docker smoke timing out after `containerd.service` started and `docker.service` remained in `Starting`. That matched post-boot rootfs limitation is not attributed to the dual-runtime code or silently counted as a green 5/5 campaign. Generated `.DS_Store`, `__pycache__`, `.pyc`, and `.pyo` paths are excluded from structural decisions. |
| Signed iOS kernel and carriers | Device and Simulator carry one versioned runtime for each canonical page profile. Within a profile they embed the same scalable-layout ET_DYN bytes used by macOS in a 16 KiB-aligned `__TEXT,__uml_elf` section covered by the normal CodeDirectory. Both kernel inputs pass the carrier verifier with three executable segments; the Darwin link aligns early executable boundaries to 16 KiB even for the 4 KiB guest so signed text cannot share an iOS host page with following loader data. The always-linked app loader owns the pre-main timer mask, loads exactly one saved profile for the process lifetime, and validates its API version, table size, page size, Linux release, and framework identity. The ELF loader byte-verifies the carrier, scales the top-packed native window with configured RAM (60.3125--62 GiB for the 1.5 GiB default and 192 MiB auxiliary allowance), aliases immutable executable pages with public `vm_remap`, copies data/BSS, and rejects executable relocations. Exactly one named host carrier owns each vCPU; 128 KiB task stacks, cooperative host-stop wakeup, and IRQ-enabled IPI polling keep task count and cross-call progress independent of the iOS host-thread limit. Kernarium leaves its launcher on the existing Swift worker, masks process-directed `SIGALRM` on app and launcher workers, and lets only vCPU0's carrier accept the early `ITIMER_REAL` calibration tick. The earlier settled 1 GiB desktop's vmalloc measurement remains historical. |
| Common-architecture performance | A complete Release 16 KiB gadgets run used the direct OverlayFS PID-1 shell with no systemd and scored 96 single-core / 565 multi-core, versus the final pre-change 96/567. Separate unscored single/multi Clang and Object Detection samples put carrier activation+synchronization below 1.2% of accumulated guest+syscall time, found no material spin/IPI leaf, and reproduced the existing Clang link-churn and Object Detection SDOT/vector-load shapes. Pair-loading dot operands left a focused SDOT probe at 2.298 seconds; compacting three operands into one word moved 2.298 to 2.289 seconds (+0.39%). Both arms were removed before Geekbench, and no macOS-only shortcut is retained. |
| Four-vCPU page-profile performance | Current Release gadgets products ran Geekbench 6.7.1 with `--multi-core --no-upload`, four vCPUs, 8 GiB, and no systemd. Neither log contains a single-core phase. All 16 workloads and both shutdowns passed: 16 KiB scored 309 multi-core (263 integer / 415 floating point), while 4 KiB scored 269 (226 / 373), or -12.94% / -14.07% / -10.12%. Workloads ranged from -1.23% Background Blur to -35.36% PDF Renderer; 4 KiB elapsed time was +17.53% with peak RSS +0.35%. G148 records product/log hashes, exact commands, the sequential order, and active-host background-load limitation; one observation per profile is directional evidence, not a causal A/B. |
| Physical-iPad completed-workload profile | The standard-signed Release app ran Geekbench 6.7.1 from its no-systemd ext4 OverlayFS shell with 1 GiB and six vCPUs. Thirteen single-core workloads completed before the user stopped the memory-pressure-heavy Photo Filter phase; no aggregate or multi-core score is claimed, and traces 009--011 are excluded. Eight completed-workload Time Profiler windows put non-carrier CPU at 0.010--0.040%, more than 99.6% of active samples on A15 performance cores, and the first/last retained thermal tracks at Nominal. Gadget assembly accounted for 73.93--98.89% except for HTML5/Clang's existing cache-link churn and Object Detection's 23.03% native-kernel share, of which about 19.3% resolved to EROFS zstd decompression. G147 ranks bounded active-link reset, folded scalar register-offset ADD, and a profile-keyed MTE-disabled vector-pair path for focused probes; no optimization is accepted from the diagnostic capture. |
| Physical-iPad targeted confirmation | Clean no-profiler reruns selected G147's four workloads more than 10% below current macOS references. Asset Compression's 84/83 versus 89 no longer crossed the threshold; Background Blur confirmed at 96/93 versus 112 and HDR at 79/75 versus 106. Horizon Detection returned 51/143/51 versus 120, repeating a slow mode but also its historically documented Geekbench adaptive-duration split. Separate 15-second Blur/Horizon/HDR profiles remained thermally Nominal, placed 99.398--99.987% of carrier samples on performance cores, and kept non-carrier CPU at 0.018--0.027%. G149 ranks profile-keyed MTE-disabled vector memory, HDR boundary/link instrumentation, and Horizon byte-store locality; its 8.775% compaction/migration kernel share is excluded as 1 GiB guest-memory pressure. No hot-path change is accepted from the intrusive traces. |
| 4 KiB high-address performance | The current clean 4 KiB gadgets product completed Geekbench 6.7.1 from the same no-systemd direct shell at 92/417 single/multi, with all guest and emulator statuses zero. That is -4.17%/-26.19% against the current 16 KiB 96/565 and +16.46%/+0.24% against the historical G35 4 KiB 79/416; the historical arm crosses source and harness changes. A separate unscored `vmmap` pass observed executable mappings beginning at `0x600000000000` and the 8 GiB physical backing at `0x600001e08000-0x600200000000`, proving that the 4 KiB run used the current 96 TiB native window rather than placing guest memory directly above 64 GiB. The scored process was not inspected. One observation per current profile limits causal performance claims. The pre-dual-runtime validation passed 53/53 tooling plus 5/5 fast guests in the canonical 4 KiB workflow, the 1/1 loader sanitizer gate, and the 16 KiB 5/5 fast guest set; the current-source rootfs boundary is recorded separately above. |
| Boundary | Non-debugger Phosh boot and repeated stable native presentation are proven on the physical iPad at 1.5 GiB and six vCPUs. The host stopped the five runs after presentation, so orderly guest shutdown is not claimed. No RAM reduction was needed because no OOM occurred. Physical Aquarium remains intentionally excluded: no swap workaround, physical Aquarium success, complete Geekbench run, aggregate score, or matched optimization A/B is claimed. The earlier 2 GiB Simulator Phosh and Aquarium results remain dated evidence only. |

## Post-baseline JIT evidence (2026-08-12)

The focused evidence below belongs to this repository checkpoint. It does not replace
or reattribute the broad accepted 2026-08-10 baseline that follows.

| Area | Current evidence and boundary |
| --- | --- |
| Budget and stable storage | The host publishes total budget, executor count, and private ceiling as one first-valid configuration. Complete private geometry is capped at 64 MiB per executor; only the shared remainder grows with guest memory. The private charge covers CPU/TLB state, block cache, fallback words, fixed indirect links, direct reverse-index heads, relocation scratch, and rounded arenas. Each target PC has at most one compact arena-adjacent activation anchor, reused by every later source and retired with the arena; bounded refusal takes the semantic unlinked path. |
| Shared ownership and semantics | The process-byte-bounded cache keys PC, exact consumed guest words, and the complete code-generation profile; shard backing is created on first insertion. Canonical words have zero executor-pointer cells and neutral relocation keys. An installer copies into its private arena, reuses or creates the compact direct target anchors, resolves indirect state, and only then increments `reused=`. Mutable activation state is absent from shared ownership; noncanonical blocks retain the accounted fallback slab. |
| Fixed-private-cache diagnosis | G138's focused Clang/Object Detection/summary changed from 460/856/628 to 542/1139/786 with a compact footer, then to 613/1238/871 after all sources for one target reused one anchor at the same 64 MiB ceiling. A follow-up two-observation sweep found 62 MiB neutral at 620.5/1255.5/883 versus 613/1242.5/872.5 for 64 MiB; 60 MiB failed the Object Detection and combined bands. The user retained 64 MiB because 62 MiB saves only 2 MiB per executor and leaves the total budget unchanged. The representative native regression fell 688 -> 171 -> 6.66 seconds across scan removal and anchor deduplication. Precursor profiling recorded 3,869 arena resets and 183,428,416 shared materializations; shared install averaged 2,424.81 ns versus 16,322.84 ns local compile (6.73x faster). Final pre-shutdown sampling peaked at 6.932 GiB RSS, placed no activation/deactivation routine in the collapsed top-stack list, and earlier accounting reached 3,285,847,540/3,624,031,696 shared bytes, showing real lazy-tier population rather than eager reservation. |
| Executor pool regression/fix | One run owns no more than its configured guest CPU count of session/domain/profile-bound executors and drains the pool before domain retirement. A pooled executor could move to a distinct task-carrier `StaticGuestMemory` with the same generation/change tuple and incorrectly skip `memory.prepare()`. The fix tracks and resets prepared-memory identity at each begin/end boundary, preparing every distinct memory object while leaving executor-TLB preparation conditional on mapping changes. Focused pool coverage passed at 16 KiB, 4 KiB, ASan/UBSan, and TSan. |
| Executor-pool performance | The prior global-mask P/E A/B is rejected. The corrected exact-session-token result is recorded at `.build/results/jit-cache-handoff/executor-pool-exact-token-measurement-perf-20260812/README.md`; all 48 scored samples and 20 sequential guest invocations validate. Primary invocation-median P wall/host CPU are 22.7025/167.400 seconds versus 22.2960/164.130 (+1.82%/+1.99%); E wall/host CPU are 17.4315/32.650 versus 14.9345/25.675 (+16.72%/+27.17%). P halves are +4.25%/+0.32% wall and E halves reverse at -7.92%/+16.93%; both ranges overlap. Recorded background load and the split E halves limit causal precision, so no structural cause is claimed. The exact-token pool is retained for the guest-CPU cap and budget proof, not as a demonstrated speedup. |
| Executor-pool mechanism trace | `.build/results/jit-cache-handoff/executor-pool-mechanism-trace-final-setup-corrected-20260812/README.md` records accepted unscored counters from two complete 35-field finals. P/E totals are 21/15 wrappers, 18,002,647/6,002,142 borrows, 99.9971%/99.9998% remembered-slot return, 530/22 fallbacks, 18,004,098/6,002,173 probes (1.000081/1.000005 per borrow), 1,461/35 failed CASes, zero waiters, 10/4 one-time constructions, peak live 10/4, peak borrowed observed 9/3, and zero overflow or standalone construction. Frequent acquisition scanning, CAS retry, waiter traffic, and reconstruction are therefore excluded as explanations for the rejected A/B. Sequence affinity is closed because lifetime fanout is not borrow-weighted cache churn and wrapper preference missed only 0.002883%/0.000183% of P/E borrows. |
| Final-profile delivery | Bounded records go directly to the stderr descriptor without taking or flushing its shared `FILE` lock, and begin with an LF in the same write so unrelated partial PTY text cannot prefix the schema. The exact unwritten suffix resumes after partial writes or `EINTR`; `EAGAIN`/`EWOULDBLOCK` waits through `poll(POLLOUT)`. The explicit post-carrier/pre-drain facade emits compile/overlap, pool, then shared and may be repeated idempotently by the app. Compile/overlap and shared retain normal-exit callbacks; pool reporting is intentionally explicit-only because it requires that quiescent snapshot. `cmake --workflow --preset check-jitprof` passed 2/2. Its fixture held the stderr `FILE` lock, filled a nonblocking pipe to `EAGAIN`, proved exact leading-LF framing and drain-and-resume record integrity through both writer APIs, exercised concurrent long records and changed/unchanged finals, and passed an architecture audit at 56 targets, 50 layered targets, and 57 registrations. |
| Predecessor one-shot artifact | `.build/results/jit-cache-handoff/link-capacity-profile-backpressure-fix-20260812/README.md` remains accepted predecessor-source evidence: Clang 807 and Object Detection 1242, all statuses zero, 1,240 complete periodic compile records, two periodic shared records, exactly one compile and shared final, and no malformed record. Direct occupancy is 262,144/262,144; shared final reports 4,354,180 matches, 4,352,573 installed reuses, 1,597 direct-capacity plus ten arena failures, and 268,434,848/268,435,456 accounted bytes. It predates the current canonical-first admission and insertion-failure fields and is not the current counter authority. |
| Rejected current-source framing artifact | `.build/results/jit-cache-handoff/jit-profile-pool-shared-final-current-source-20260812/README.md` preserves one unscored attempt and no retry. All statuses, workloads, three finals, and terminal order passed, but exactly 39 unterminated shell continuation prompts prefixed the first otherwise complete compile record on physical line 188. The emulator streams already shared one PTY, so suffix parsing or outer-stream separation was rejected as a false repair. |
| Accepted current-source artifact | `.build/results/jit-cache-handoff/jit-profile-line-framing-current-source-20260812/README.md` records the accepted post-framing run. All statuses and the strict parser passed. The 39 prompts stay on their own line, and all 1,450 physical profile records begin exactly with `Darwin JIT` and end in LF: 1,445 periodic compile, two periodic shared, and exactly one compile, pool, and shared final in post-stop order with nothing after shared. Direct/indirect occupancy is 262,144/262,144 and 21,469/262,144. Shared final reports 5,131,797 matches, 5,127,800 reuses, 5,672,507 admission rejections, zero insertion failures/retained failure bytes, 3,986 direct-capacity plus 11 arena failures, and 268,434,608/268,435,456 accounted bytes. Clang 758 and Object Detection 1257 are diagnostic/unscored. |
| Fixed x8/x9 carry | `.build/results/jit-cache-handoff/carry-x8-x9-matched-perf-final-20260812/README.md` records focused 4 KiB ON and 16 KiB ON ASan/UBSan gates, eight preflights, and 32 valid scored invocations. P raw/normalized wall/normalized CPU are -4.443%/+5.091%/+5.614%. E raw/control/normalized wall/normalized CPU are -2.503%/-3.100%/-0.616%/+2.025%. Strict preregistration is P PASS/E FAIL/overall FAIL; the user policy accepts E regression below twice P's measured gain. Default remains OFF. |
| G107 successor hoist | `.build/results/jit-cache-handoff/g107-successor-hoist-matched-perf-setup-20260812/` records focused Release 16 KiB and 4 KiB OFF/ON plus 16 KiB ON ASan/UBSan gates and 96 scored samples: P/E polymorphic wall improved 13.723512%/7.258795% and CPU 13.888889%/9.683605%. `.build/results/gadgets-optimization-20260813/` supplies the representative boundary: full Geekbench Single-Core 94→97, Multi-Core 564→567, targeted Clang/Object Detection 438→441, and rerun File Compression/HTML5 within 1.3% of OFF. The bounded selector is now ON by default. |
| G140 SIMD destination residency | `.build/results/gadgets-optimization-20260813/` records the bounded destination-only MLA probe and Geekbench checks. The sensitive probe improved 69.6% P and 43.5% E; the out-of-bank control moved -1.1%/-5.7%. Targeted SIMD Geekbench changed 91→92. Against the G107 predecessor, the clean full result changed Single-Core 97→96 and Multi-Core 567→575. An earlier full attempt stopped after an RCU stall in Multi-Core Text Processing; isolated and clean-full reruns completed it at 62/63. The bounded selector is ON by default. |
| G141 exact gadget-cache validation | `.build/results/gadgets-optimization-20260813/` records P/E invalidation-probe gains of 62.4%/50.6%, targeted multi-core Clang/Object Detection 394/461→383/464, and full Geekbench 96/575→96/567 against G140 (original baseline 94/564). Lazy grow-on-demand validation buffers replaced the first fixed-array prototype; exact stale reuse is ON by default. A live sample put decode at 2.36% of configured ten-vCPU capacity, so global decoded templates are closed without adding shared lookup/copy/relocation state. |
| Residual gadget closure | G8 (-0.06%), G17 (-0.36%), and G36 (noise) do not clear the prospective `P > 2%` gate, while G106 later found the same noncritical instruction-count class flat on both P and E. That remeasurement item is closed. G140 closes the evidence-backed bounded G130 destination-only route; no whole-bank or cross-block vector state is accepted. |
| Measurement boundary | Both current-source profile attempts are diagnostics without matched controls: the malformed one is retained as failure evidence, and the accepted post-framing scores are explicitly unscored. The current `check-jitprof` workflow passes 2/2. Both executor-pool A/Bs are rejected. The shortened sharing ON/OFF/OFF/ON full-Geekbench sequence reports median peak tree RSS 11,385,044,992 bytes ON versus 11,067,219,968 OFF (+2.87%); its two observations per arm are too noisy for a causal speed claim, so the significant-memory exception does not apply. The current 16 KiB workflow passes 575/575 units and 47/47 tooling tests. The 4 KiB unit run passed 575/576; its only failure was the unrelated timing-sensitive executor-pool multislot test, which passed immediately in an exact isolated rerun, and 4 KiB tooling passes 47/47. Every gadget test passed in both page profiles. `scripts/build-xcframework.sh` passes all three Apple slices and its installed, raw, and Xcode consumer builds. Full sanitizer and complete integration workflows are not credited to this checkpoint. |

## Current acceptance evidence (2026-08-10)

The accepted implementation baseline is
`783bfcf7e2447f8f981830a158bec3f402b08d97`. It names the code under test rather
than the later documentation-only reconciliation commit. The unified-EROFS
migration is recorded separately because it did not rerun every host, package,
sanitizer, or performance campaign from that baseline. This table supersedes
all older audit status and open-work claims below; the remainder of this file
is a dated evidence archive.

| Area | Final local evidence and boundary |
| --- | --- |
| Linux replay | The authenticated Linux 6.18.41 package plus 14 canonical topics replayed to tree `5b7b50cd573b7a1077586955cad9cf13770df35d`. Topic ordering, overlap, ABI, and delta policy passed at 155 changed files and 6 outside the Darwin port, and both supported page-profile kernel builds completed. |
| ABI | Host/kernel bridge ABI v6 is `0x6524b65f20239ea1`; installed public API v4 is `0xd9a69804d3303044`. Declaration identity, mirrored-header, compile-time layout, and consumer checks passed. |
| Retained mechanisms | G99 keeps ThinLTO on the gadget translation library. G100 keeps exactly five audited decode-family `.ipp` fragments behind one composer. G121 keeps the published per-thread lifecycle-slot protocol. G123 keeps the forward `init_mm` serialization and whole-Darwin-slot 4 KiB repair; the diagnostic pre-refactor rollback is absent. |
| Ordinary host | Unit passed 572/572 at 16 KiB and 573/573 at 4 KiB; tooling passed 51/51 in both profiles. Architecture/source graphs covered 575 targets and 623 registrations at 16 KiB and 576/624 at 4 KiB. Input materialization was off, zero sidecars were emitted, and final quiescence required no cleanup action. |
| Source-size advisory | The current report covered 1,244 files: 105 above a guideline, 87 without optional review context, and 12 changed in size since review. `source-size-advisory-policy` passed. These counts are navigation/ownership signals only; crossing a guideline or changing size is not an acceptance failure. |
| Sanitizers and devices | Full ASan+UBSan passed 572/572 in 710.45 seconds; TSan passed 8/8 in 5.14 seconds; malformed-loader ASan passed 1/1. Normal and ASan GPU graphs each passed 5/5 units plus the 1/1 VNC tooling test; the GPU graph audit covered 595 targets and 628 registrations. |
| Refactor-baseline guests | `integration-fast` passed 5/5 at 16 KiB in 153.40 seconds and 5/5 at 4 KiB in 84.69 seconds. Standalone 10-vCPU SMP passed at 8.19 and 8.03 effective host cores. The old partitioned-image observation belongs only to that dated baseline. |
| Unified EROFS migration | Final host workflows passed 572/572 units plus 49/49 tooling at 16 KiB and 573/573 plus 49/49 at 4 KiB. The latest complete integration workflows passed 52/52 tooling checks and all five regenerated `integration-fast` paths with the raw EROFS image at `/dev/ubda`: 16 KiB 5/5 in 65.42 seconds and 4 KiB 5/5 in 69.33 seconds. The overlay/systemd test passed in 34.06 and 38.19 seconds, respectively; each proved separate Docker and containerd tmpfs mounts and imported and ran the source-built hello-world container with `--pull=never --network none`. Provisioning, overlay launcher, GPU/network/LTP/performance runner, emulator-option, and API contracts pass. EROFS replaces host mode, per-run hashes, sidecar locks, `ubd0r`, and `ro` enforcement. The 16 KiB guest emits the upstream experimental EROFS subpage-compression warning for the image's 4 KiB filesystem blocks; the 4 KiB guest does not. |
| Unified-image payloads | `/opt/ltp`, `/opt/node/bin/node`, and local benchmark payloads are present. The LTP manifests are 2,428 verified plus 135 disabled, disjoint and totaling 2,563; 88 were moved for intentionally absent `CONFIG_HUGETLBFS` (79), `CONFIG_TRANSPARENT_HUGEPAGE` (5), and `CONFIG_CAN` (4). The final-image focused boundary is 44 gadgets-engine rows, all accepted; `statx05` passed 4/0 in both profiles and the exact live `zram01` row passed 12/1 with rc 0 in 167.6 seconds. Full LTP was not rerun. Geekbench 6 defaults to its `/opt` executable with `--no-upload`; the final scoring run remains skipped. |
| Apple package | The macOS, iOS-device, and iOS-simulator slices formed the XCFramework and passed platform, API, staging, package-shape, and consumer checks. Raw and installed-tree consumers passed; the real Xcode consumer reported `BUILD SUCCEEDED`, with only a non-fatal locked-iPhone discovery warning. |
| Post-baseline native display | The host display boundary now selects VNC or a native AppKit/MetalKit window. The supported native Virgl path blits the renderer's GL texture into a two-slot IOSurface bridge and samples that storage from Metal; direct-path unit coverage proves accepted frames skip CPU readback and framebuffer update. The bridge has one Metal frame in flight and latest-ready replacement rather than a FIFO. Normal and ASan+UBSan GPU workflows each passed 5/5 units plus 1/1 VNC tooling, and the production integration CLI rebuilt successfully. A focused 800x500, four-vCPU run reached `graphical.target`, emitted `UML_GPU_OVERLAY_TEST: PASS`, and logged direct CGL/IOSurface/Metal presentation before Ctrl-C. This is native rendering/startup evidence rather than a timed or input-interaction campaign. |
| Explicit limitations | Verification was local-only; this repository has no hosted CI workflow. Full LTP was not run for the migration even though its payload is now available. Input materialization remains opt-in (about 6.6 GiB for a complete profile), and semantic evidence remains **0/983 variant-complete**. Source-size thresholds are advisory. The user explicitly directed that the final Geekbench rerun be skipped, so historical and focused controls are preserved without a new score. External KDE/VNC, rendered WebGL, full SMP, and full performance campaigns were not requalified merely by migrating the rootfs. |

Current guest execution maps EROFS directly without per-run hashing or sidecar
locking. Only the explicit provisioning path reads `tools/rootfs/manifest.json`
to authenticate an input image.

The authoritative handoff is in
[`REFACTOR_HANDOFF.md`](REFACTOR_HANDOFF.md); commands and external
prerequisites are in [`testing.md`](testing.md).

## Seventh-audit evidence (historical; superseded)

The following records the open seventh independent audit on 2026-08-04. It is
preserved to show what drove the subsequent work and is not the current status.

| Area | Fresh evidence and consequence |
| --- | --- |
| Allocation/feature oracle | The generator reads each form label and selected word from test-owned catalogs, asks the external decoder about that selected word, and infers a mask with independent one-bit flips. Regenerating all 983 rows is byte-identical at SHA-256 `0e383651df7e3d743ab7d7d4ffe2007b6ce41bd6bb09f1ffc8bdb9f0ca77d574`, but a consistently changed form or unrelated feature still regenerates and validates. A two-bit mask scan found 127 accepted words across 70 IDs that the decoder rejects or reclassifies; `0b221427`, for example, satisfies one current `ffe00000/0b200000` mask but is rejected by the decoder. Determinism therefore does not establish an independent form/allocation/feature oracle. |
| Variant cardinality | Inventory, bindings, and evidence each contain 983 unique stable IDs. The validator rejects a second evidence record for an ID, the recorder emits only the bound selected case/word, and the harness credits only that bound entry. The generic variant-description field and broad executable sweeps do not independently enumerate every required valid/invalid variant, so one checked row cannot close a multi-variant instruction entry. |
| Exact production engines | The full conformance sweep uses a test-local gadget adapter and a native-code support target that recompiles the implementation with test macros. Focused production-adapter contracts pass, but the 983-row report does not prove the exact production engine objects. The native executor is still textually assembled from roughly 10,095 implementation-header lines, and gadget translation retains a roughly 4,274-line central builder plus large selection fragments. |
| Runtime/ABI fault correctness | `MemoryAccess` distinguishes execute, but every engine/runtime fault conversion, the translated TLB, the C ABI, and the shared harness collapse it to read/write; Linux guesses execute when fault address equals PC. A data read at PC and a true fetch fault can therefore take the wrong retry path. Separately, a gadget second-block throw can bypass architectural-state materialization/host-environment restoration, and interrupt-token registration catches allocation failure without latching or reporting it. None has the required injected cross-engine/ABI test. |
| Engine boundary audit | Link/object/aggregate rules pass, but the real File API manifest does not serialize include-directory ownership. Synthetic negatives inject include edges not produced by the real graph, and text scanning can be bypassed by adding another engine's include directory then including only a basename. The registered 25-pass edge fixture timed out at 60.01 seconds; its direct command passed in 59.71 seconds. |
| Memory-domain lifetime | Every run allocates a monotonic domain and permanently records it in the software retirement set; initialized 4 KiB runs also retain it in the native set. Even domains that never acquired an address space are retained. If retirement insertion fails, the exception is swallowed and a delayed resolver can recreate retired state. No 100,000-run boundedness/interleaving gate exists. |
| GPU lifecycle and ownership | A live peer can leave a partial frame in blocking `MSG_WAITALL` reads while service stop only sets a flag and joins. The service also retains RAII ownership of peer descriptors whose exact raw numbers are handed to and closed by guest transport code, creating double-close/reuse risk. Reconnect/reset retains device resources and scanout state, and direct command-level exception-atomic tests are absent. Existing normal and ASan+UBSan ownership fixtures pass but do not exercise these cases. |
| Linux/workflow shape | Topic 0002 still combines MMU/large-page, SMP/timer/IRQ, execution/capability, and build-portability changes across 19 files (+1,723/-117), contrary to the planned decomposition. Fresh checked-in integration presets cannot provide the required case-sensitive kernel checkout. The source audit traverses large ignored trees before filtering, and raw CTest registrations can escape mechanical suite-label verification. |
| Suite build completeness | In a fresh GPU graph, `unit-test-binaries` omits the registered GPU ownership binary. `tooling-test-binaries` is empty because it scans only the tooling subdirectory, while the registered VNC smoke uses two parent-owned targets that are not built. There is no graph audit joining registered target commands to suite aggregate dependencies. Unit source/execution is Python-free, but ADR 0001's stronger no-Python build claim conflicts with the unconditional Python ABI verifier at configure. |
| Current product provenance | Current gadget/native-code split sources and topic 0011 postdate the existing macOS CLI, kernels, and package consumers. The macOS XCFramework archive contains obsolete `program.cpp.o` while newer build archives contain the split objects. Those dated artifacts cannot close the current tree. |
| Bounded cleanup and zero-cost diagnostics | One fatal gadget wrapper/declaration/body has no source or relocation reachability. Network restart and runtime lifecycle/failure controls remain in production objects. Disabled dispatch/MMU profiling still performs clock work; enabled gate profiling permanently retains one heap record per participating thread and scans the growing vector. Legacy-context teardown can reach the software registry through implicit static destruction order. Remove or isolate these paths and add symbol/object, no-clock-call, thread-churn, and subprocess exit-order gates. |

### Seventh-audit fresh verification

| Check | Result |
| --- | --- |
| 16 KiB host graph | clean configure/build succeeded; strict unit selection passed 562/562 in 468.78 seconds |
| 4 KiB host graph | clean configure/build succeeded; strict unit selection passed 563/563 in 367.62 seconds |
| Architecture/source | current 4 KiB graph passed at 553 targets / 586 registrations; normal and sanitized GPU graphs passed at 562 targets / 587 registrations; source report covers 1,060 files, 60 above advisory guidance, 42 without optional review context, and zero changed since review |
| Tooling | 22/23 passed; only `symmetric-engine-edge-audit` reached its 60-second timeout, while the same command passed directly in 59.71 seconds |
| Safety gates | full ASan+UBSan 562/562; TSan 4/4; malformed-loader ASan+UBSan 1/1; GPU normal 1/1 and GPU ASan+UBSan 1/1 after loopback-port permission |
| Policy/dependencies | pinned release-source manifest, pristine LibVNCServer 0.9.15, verified private prefix, ABI identities, Linux manifest/topology/next-version fixtures, and isolated topic 0011 checks pass |
| Not claimed | no guest boot, rootfs change, LTP, browser, benchmark, current Linux replay/build, current CLI final link, or current package rebuild was performed |

Reproduction notes for a fresh task:

```sh
cmake --build .build/audit-round7-unit16 \
  --target tooling-test-binaries unit-test-binaries architecture-audit
ctest --test-dir .build/audit-round7-unit16 -L '^unit$' --output-on-failure

cmake --build .build/audit-round7-unit4k \
  --target tooling-test-binaries unit-test-binaries architecture-audit
ctest --test-dir .build/audit-round7-unit4k -L '^unit$' --output-on-failure
ctest --test-dir .build/audit-round7-unit4k \
  -R '^symmetric-engine-edge-audit$' --output-on-failure

python3 tests/tooling/arm64/generate_a64_instruction_allocations.py \
  --llvm-mc /opt/homebrew/opt/llvm/bin/llvm-mc \
  --output /private/tmp/uml-audit-round7-allocations.tsv
cmp tests/unit/arm64/a64_instruction_allocations.tsv \
  /private/tmp/uml-audit-round7-allocations.tsv
printf '0x27 0x14 0x22 0x0b\n' | \
  /opt/homebrew/opt/llvm/bin/llvm-mc --disassemble --show-inst \
    --triple=aarch64 --mattr=+all
```

The final command rejects a word admitted by one checked-in mask. The 127-word
two-bit scan was an audit probe, not a checked-in test; converting that scan
and the whole-catalog form/feature mutations into durable negative fixtures is
part of the open acceptance work. Completed logs are under the named build
directories; the full sanitizer log contains 562 `Test Passed.` records and no
`Test Failed.` record.

## Sixth-audit closure evidence (historical; superseded)

The table below records what the sixth pass reported. Seventh-audit findings
above invalidate its completeness claim without erasing the value of its dated
test results.

| Area | Dated reported evidence |
| --- | --- |
| Independent instruction oracle | `generate_a64_instruction_allocations.py` records LLVM MC 22.1.8 provenance and generates 983 allocations with 79 distinct masks and 21 exact-word masks. The validator keys allocation ownership by section plus architectural form and checks opcode provenance, mask, and value. Seven negative fixtures reject encoding, form, feature, alias, and the formerly accepted self-consistent wrong-form mutations. |
| Exact engine evidence | Both page-size reports contain all 983 IDs. JIT has 670 retired plus 40 executed/non-retired cases and 273 exact feature skips; interpreter and gadgets have 893 retired plus 90 executed/non-retired cases. Reports are byte-identical at SHA-256 `d4bf8d90db910807e6adbf2bf19e06f1d32ccd98fd60cfa7b2d26a583d3ed139`. |
| Engine architecture and modularity | Gadget catalog/selection/encoding, build, and execution have separate owners. JIT cache, linking, publication, diagnostics, semantic memory, emission, compilation, verification, lifecycle, and testing are separated; test-only ownership is excluded from production. The production JIT object is byte-identical to its pre-split object. The File API audit rejects cross-engine link, object, aggregate, include, and source-include edges in both directions, with a 24-case negative matrix. |
| 4 KiB run isolation and failure safety | Native shadow state is owned by the active domain and retired after normal or abnormal execution. Because domains ultimately materialize in one process address space, native callbacks serialize with the embedded runner in 4 KiB builds. A same-VA two-handle test proves queued cancellation, flush-before-handoff, byte/domain isolation, and retry; 16 KiB native callbacks retain separate concurrency coverage. Map, protect, unmap, and refresh allocation failures are contained at the C boundary, roll back host slots/protections/bookkeeping without harming siblings, and permit retry. Native shadow mappings are data-only. |
| GPU/VNC ownership and gates | Queue notification descriptors have RAII ownership; memory-table replacement is transactional; initialized VNC state remains guarded across event-thread construction. Production dispatch covers repeated replacement, each reset-stage failure, retry, descriptor balance, post-listen thread failure, and exact port reuse. Canonical rootfs-free normal and ASan+UBSan GPU workflows require this test whenever its target exists. |
| Linux/dependency policy | The 64-bit vhost-user DMA-mask correction is independent topic 11. Complete-directory inventory checks reject unlisted Linux or dependency patches. `topic-path-overlaps.tsv` declares the sole exact multi-topic path as `virtio_uml.c` owned by 0007/0011; undeclared, stale, and wrong-owner fixtures fail. A fresh authenticated eleven-topic replay verifies final tree `834e5c5faa8fc4638c9fdcbd40a1f29f5c7c2d5d` and the 162/13 delta policy. |
| Bounded taxonomy/coverage | A deterministic queued-transmit restart proves that no previous-session frame executes after reopen. The NEON inventory input is owned by `tests/unit/arm64`. The source report covers 1,060 files, 60 above advisory guidance, 42 without optional review context, and zero changed since review. |
| iOS executable memory | Device and simulator archives contain no executable-publication reference outside `jit_arena.cpp`. Interpreter, gadgets, native shadow, and software-MMU storage are data-only: no W+X mapping and no RW/RX transition. The explicit JIT path remains available for entitled debugger/jailbreak use. |

### Independent-oracle baseline reproduction (historical)

Before the closure implementation, the following baseline reproduction exposed
the duplicated catalog. It describes the old file contents and is retained as
the negative symptom; running it against the current generated catalog no
longer produces this result.

```sh
awk -F '\t' '
  NR==FNR { if (FNR>1) binding[$1]=$6; next }
  FNR>1 { rows++; if ($4=="ffffffff") exact++; if (binding[$1]==$5) same++ }
  END { printf "rows=%d exact_masks=%d same_encoding_as_bindings=%d\n", rows, exact, same }
' tests/unit/arm64/a64_instruction_evidence_bindings.tsv \
  tests/unit/arm64/a64_instruction_allocations.tsv
```

Historical output:

```text
rows=983 exact_masks=983 same_encoding_as_bindings=983
```

At that baseline, calling the real binding validator with the same nonmatching
architectural word in both test-owned fixtures printed:

```text
accepted self-consistent nonmatching architectural encoding
```

The checked-in negative fixture now makes that mutation fail for an allocation-
form mismatch.

Fresh closure verification:

| Check | Result |
| --- | --- |
| 16 KiB host graph | build 1130/1130; unit 562/562; tooling 23/23 |
| 4 KiB host graph | build 1070/1070; unit 563/563; exact-evidence validator 1/1 |
| Sanitizers | full ASan+UBSan 562/562; focused native 4 KiB ASan+UBSan 2/2; TSan 4/4; malformed-loader ASan+UBSan 1/1 |
| GPU-enabled graphs | normal and ASan+UBSan each audited as 562 targets / 587 registrations; ownership test 1/1 in each |
| Network | rootfs-free deterministic SLiRP ARP/restart test 1/1 |
| Structural audits | current source/architecture graphs passed; report covers 1,060 files, 60 above guideline, 42 without optional review context, and zero changed since review |
| Apple products | iOS device and simulator builds/audits passed at 21 targets each; non-JIT executable-publication symbol scans were empty |
| Linux/dependencies | fresh authenticated eleven-topic replay, exact final tree, 162/13 delta, topology/overlap policy, exact patch inventories, both ABI identities, pinned official inputs, pristine LibVNCServer 0.9.15, and dependency negative tests passed |
| External campaigns | no guest boot, LTP, Node.js, Go, browser, benchmark, or controlled performance run; rootfs redesign remains deferred |

The fresh cached-archive replay reverified the pinned OpenPGP signer, packaged
tree, all eleven patch identities/order, canonical final tree, and delta policy;
its temporary checkout was removed afterward. Dynamic generated metadata is
intentionally not a byte-equality gate.

## Fifth-audit evidence (historical closure claim)

The following table records what the fifth pass reported before the sixth-audit
baseline found gaps that the sixth pass then reported closed. The seventh audit
reopened some of those guarantees. This remains useful dated evidence, not the
current acceptance checklist.

| Area | Dated fifth-audit result |
| --- | --- |
| Deterministic 16 KiB host graph | `.build/audit-round5-main-unit` passed reset, 561 evidence producers, and validation 563/563 in 286.37 seconds; its generated report contains all 983 stable IDs |
| Deterministic 4 KiB host graph | `.build/audit-round5-main-4k` passed the 553-target / 585-registration architecture audit and reset, native-shadow coverage, all producers, and validation 564/564 in 279.60 seconds |
| Exact evidence semantics | independent `a64_instruction_allocations.tsv` mask/value metadata and exact required/observed feature vectors join every binding. Each page-size report records 670 retired native cases, 40 executed/non-retired cases, and 273 exact native-feature skips while interpreter and gadgets continue. Six negative fixtures include encoding-only and unrelated-feature mutations |
| Evidence workflow | reset is a Python-free unit fixture, ordinary producers depend on it, validation depends on all producers, and standalone evidence presets select the complete graph. Raw tooling excludes reset, preserves producer files, and passed 22/22 after generation |
| Engine architecture and iOS memory | transfer, TLB, FP, and environment helpers are owned by neutral `uml_arm64_execution_helpers`; native-code targets have no gadget link/object/header/symbol edge and the audit rejects one. Interpreter/gadget source and iOS archives contain no W+X or RW/RX publication primitive; only `jit_arena.cpp` owns executable publication |
| Cross-registry rollback and admission | deferred context/provider publication commits jointly without allocation. Provider-B failure leaves A's CPU/network fallback unchanged, permits B retry, and does not weaken committed-overlap fail-closed behavior. Stop mutation shares the admission mutex, and deterministic post-admission/wait-boundary hooks prove a queued stop completes before active-run release |
| Software and native MMU | preallocation/rollback covers Nth-page, Nth-operation, and history-publication failures with coherent mapping/code sequences and retry. Batches retain exact generation-state identity and reject retirement/replacement with `-EAGAIN`. Native 4 KiB C entry points contain exceptions and roll back post-`mmap`, container-growth, and multi-page failures without harming sibling pages |
| GPU received descriptors | `ReceivedDescriptors` owns ancillary FDs immediately after `recvmsg`; release occurs only after successful transfer. Sixty-four repetitions per malformed-version, oversized, truncated, dispatch-exception, and allocation-failure class plus valid transfer passed 1/1 with unchanged FD counts. Startup failure seams compile only in a test variant |
| Canonical gates and policy | malformed loader ASan+UBSan passed 1/1 from the checked-in loader preset; canonical TSan passed exactly 4/4 in 4.44 seconds. Next-version fixtures distinguish applied, contained, and conflicting patches and verify carry/drop/retest/upstream output; topology, manifest, policy, ABI, and documentation checks pass |
| Linux, ABI, and dependencies | authenticated no-fetch replay matches canonical final tree `834e5c5faa8fc4638c9fdcbd40a1f29f5c7c2d5d`; commit/build metadata is intentionally dynamic. The 162/13 delta policy, independent public/kernel ABI v4 identities, exact 16/4 KiB active-import checks, pristine LibVNCServer 0.9.15, and the freshly rebuilt default private prefix all verify. The then-current 16 KiB kernel and production app passed final static-link closure |
| Apple packages | macOS, iOS device, and iOS Simulator graphs passed architecture audits and raw/install-tree genuine C/C++ consumer links. `.build/xcframework/UMLEmulator.xcframework` was produced from the then-current fifth-audit tree and default prefix |
| Source organization | the advisory audit covers 1,034 files: 53 above guideline, 42 without optional review context, and six changed since review; cohesive logical units may exceed a nominal line target |
| Guest/performance work | no guest boot, LTP, Node.js, Go, browser, or benchmark campaign was run for closure; rootfs redesign remains deferred and physical-device/external-service campaigns remain external acceptance |

### Fifth-audit baseline reproductions (historical)

Before the closure changes, the raw tooling failure was reproduced with:

```sh
ctest --test-dir .build/audit-round5-main-unit \
  --output-on-failure -L '^tooling$' -j 6
```

It passed 21 of 22 tests and failed `arm64-instruction-evidence` because no
evidence remained after the selected reset. CTest `DEPENDS` orders only tests
already selected; it does not select the unit producers. The corresponding
post-unit, reset-excluding 4 KiB run passed:

```sh
ctest --test-dir .build/audit-round5-main-unit4k \
  --output-on-failure -L '^tooling$' \
  -E '^arm64-instruction-evidence-reset$' -j 6
```

The validator's encoding/feature negative used its real `validate_bindings()`
entry point with a one-row inventory and printed:

```text
accepted mismatched encoding and unchecked feature requirement
```

At that baseline the checked-in catalog's previously identified false rows were
repaired, but the reproduction showed that the guarantee was not mechanically
enforced. The fifth pass claimed its allocation join closed that gap; the
historical sixth-audit self-consistent reproduction above proved it did not.

The baseline GPU descriptor reproduction sent a valid-version unsupported request over
the service socket with one `SCM_RIGHTS` descriptor, closed the sender's copy,
and repeated that sequence 12 times. The service logged 12 unsupported-request
errors and retained descriptors 9 through 20, all referring to `/dev/null`.

## Fourth-audit closure evidence (historical)

This table was the reported end state of the fourth audit. Later audit records
supersede it, and the seventh-audit checklist is authoritative wherever an
underlying guarantee was reopened.

| Area | Dated result or evidence |
| --- | --- |
| Host units | the then-current 4 KiB selection passed 562/562 and the 16 KiB selection passed 561/561, with focused MMU, lifecycle, gadget, GPU, VNC, and packaging checks repeated after that audit's final edits |
| Exact ARM64 evidence | both page-size reports validate 983/983 checked-in stable-ID bindings, with exact encoding/form and executed/retired joins, 713 JIT executions, 270 exact host-feature skips, and all four negative fixtures passing |
| Runtime safety | all engines agree on guest-illegal implementation gaps and remain reusable; same-handle fail-fast, stop-aware embedded admission, domain/run-isolated MMU state, registry rollback, SLiRP readiness, and allocation-fault retry tests pass; the bounded ELF-loader fixture passes both at the production link address and under targeted ASan+UBSan |
| Architecture and ownership | the 559-target / 585-registration unit/tooling/GPU graph passes; tooling-disabled external integration configures and audits as 39 targets / 10 registrations; all three package product graphs pass |
| Public and kernel ABI | public API v4 is `0xd9a69804d3303044` (`d9a69804d33030448b780d58c3f553c0e12e4f1e19ab37f89d2719b8b93115f1`); independent kernel bridge v4 is `0xfe2d71a33e3d446e` (`fe2d71a33e3d446ec929bad4ef91d8f7b59fa2c1718bbd3d137cb24627ba175b`) |
| Dependencies and link provenance | pristine LibVNCServer 0.9.15 tree `3b0f5deb99f726cc0cfb963b783fcc396017fde8e646240a9ed3b01c18077a35`, fresh private-prefix verification, loopback VNC, canonical final-link allowlist, and ambient-archive rejection pass |
| Linux replay | ten exact nonempty stable patch IDs, subjects/order, no merges, topology negatives, policy checks, and independent no-fetch verification pass; exact final tree is `834e5c5faa8fc4638c9fdcbd40a1f29f5c7c2d5d` over the bounded 162-path delta |
| Apple package and executable memory | macOS, iOS device, and iOS Simulator slices pass raw and installed-tree genuine C/C++ consumers; non-JIT paths contain no W+X or RW/RX transition, and device/simulator archives expose executable publication only through the explicitly selected JIT arena |
| Guest/performance work | no new guest boot, LTP, Node.js, Go, browser, or benchmark workload was run for this closure; preserved earlier lightweight and performance evidence remains below |

## Initial fourth-audit reproductions (historical)

The following failures were the inputs to the now-closed work. They remain here
to show the original symptoms and are not claims about the current tree.

The original failing invocation did not require a guest:

```sh
cmake -S tests/consumer/apple \
  -B .build/audit-round4-macos-network-consumer -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/Apple.cmake" \
  -DUML_APPLE_PLATFORM=macos \
  -DUML_EMULATOR_LIBRARY="$PWD/.build/presets/macos-integration/darwin/emulator/libuml_emulator.a" \
  -DUML_EMULATOR_HEADERS="$PWD/darwin/api/include" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build .build/audit-round4-macos-network-consumer \
  --target uml-emulator-external-consumer --parallel 10
```

The then-regenerated
`../.build/audit-round3-finish-unit/instruction-evidence/stable-id-bindings.tsv`
contains 983 output rows, but only 183 use an explicit `stable-id` binding; 800
use `decoded-label`. They resolve to 902 unique evidence selections. Concrete
false-form bindings include:

- `BASE-0009` (extended register) and `BASE-0011` (shifted register) both select
  `0x2b220027`, disassembled as an extended-register form.
- `FPSIMD-0234` (immediate) and `FPSIMD-0235` (literal) both select
  `0x3c403c5d`, disassembled as an immediate writeback load.

The cause was structural. `form_matches()` fell through from exact compound
qualifiers to broad substring tests, while `section_matches()` inferred section
ownership from names and paths. `EngineHarness::check_program()` recorded every
stored program word with one aggregate engine result rather than proving the
word retired. `arm64_instruction_evidence.cpp` then ORed engine flags across
variants. The validator therefore proved only that it could assign every
inventory row to some passing evidence record, not that the correct instruction
form and every variant ran on every applicable engine.

Other findings were source-level reproductions rather than new passing tests.
Most notably, `Emulator::run()` acquired the global embedded-run mutex before
same-handle admission; the MMU was process-global and generation-only; engines
disagreed on implementation gaps; the ELF loader used destructive fixed
mappings without complete range validation or unmapping; and an undersized
SLiRP read left stale readiness. Each now has the focused acceptance coverage
summarized above and specified in the handoff.

Three later user decisions changed prioritization without erasing the audit
evidence: guest-illegal became the required nonfatal outcome for an
unimplemented instruction; the LibVNCServer post-release patch layer was
removed in favor of the pristine 0.9.15 release archive; and rootfs
hash/lock/maintenance work—including the observed launcher lock mismatch—is
deferred until the testing-image model is redesigned.

## Third-audit closure (historical)

The third campaign's focused checks reported closure of the run-retirement
race, real embedded-kernel multi-handle contract, fresh guest-helper generation,
both optional JIT profiles, reverse/interface/product architecture enforcement,
build-time dependency revalidation, network queue/provider reuse, ABI/device
extraction, and the final-state Linux topic series. These were the claims
accepted by that campaign; the seventh audit supersedes conflicting closure
statements. The
earlier broad matrices remain
parseable passing evidence for their dated configurations and are retained
below rather than relabeled as new runs.

## Verification host and tree (historical)

- macOS 26.3 (25D125), Darwin 25.3.0, arm64
- CMake 4.1.1 and Ninja 1.11.1
- Apple clang 21.0.0 (`clang-2100.1.1.101`)
- Python 3.14.6
- Xcode 26.6 (17F113)
- historical outer repository base: `618b68658013b83394efef367570fa2c36113eb0`
- current outer HEAD: `b931e9b682e7b4bde4a4824c2e2142676052e1e1`
  (`Major refactor`)

The fourth-audit evidence was originally collected while the refactor was a
dirty tree based on the historical commit above. At the fifth-audit baseline,
the outer HEAD at that time had 109 modified/deleted tracked paths and 15
untracked status entries containing implementation, tests, replay policy, ABI,
and documentation work. Nothing was staged, and all pre-existing changes were
preserved.

## Preserved host and guest matrices

| Third-audit campaign | Result | Local evidence |
| --- | --- | --- |
| Dated unit selection | 562/562 passed | `../.build/audit-round3-finish-unit` |
| Repeated API and context lifecycle | 50/50 each passed | that audit's unit binaries |
| Tooling/provenance after evidence generation | 18/18 passed | reset fixture excluded |
| Targeted TSan | 1/1 passed | `../.build/audit-round3-final-tsan` |
| Optional JIT link profile | 2/2 passed | `../.build/audit-round3-final-profile-link` |
| Optional JIT pressure profile | 1/1 passed | `../.build/audit-round3-final-profile-pressure` |
| Bounded guest acceptance, 16 KiB | 5/5 passed | `../.build/audit-round3-final-integration` |
| Bounded guest acceptance, 4 KiB | 1/1 passed | `../.build/audit-round3-final-integration-4k` |

The 16 KiB set covers immutable-overlay systemd, lightweight interpreter,
gadgets, JIT, and deterministic SLiRP. The 4 KiB set proves a second kernel
build and lightweight interpreter boot. Interpreter tests use shell arithmetic
and CPU/HWCAP inspection only; they exclude Node.js, Go, browsers, benchmarks,
and the pressure registrations. Gadget and JIT smokes are separate engine
tests.

The preserved broader campaign remains: 580/580 normal unit/tooling at 16 KiB,
581/581 at 4 KiB, 562/562 ASan+UBSan, 5/5 targeted TSan, and 24/24 fast guest
integration at each page size. Those JUnits remain under
`../.build/results/refactor/final-20260803`; they are dated regression evidence,
not substituted for the fifth-audit acceptance work.

## Preserved instruction and architecture evidence

The third-audit executable-instruction report assigned all 983 inventory IDs
(516 Base and 467 FP&SIMD) to registered CTests and disassembled encodings, with
interpreter/gadget flags and either a JIT flag or an explicit host-feature skip.
The fourth audit showed that this assignment was not proof of correct per-form
or per-variant execution. The generated `stable-id-bindings.tsv` contained one
header plus all 983 assignments in both page-size build trees. Selecting only
`arm64-instruction-evidence` lists and runs one validator without activating
its reset fixture or erasing producer evidence. The validator is
non-destructive to producer TSVs, but it refreshes `stable-id-bindings.tsv`.
The checked-in exact catalog and later reports are stronger than this heuristic
historical evidence, but the sixth and seventh audits found that they still lack
an independent encoding-to-form and exact feature-skip join.

The preserved third-audit architecture campaign passed with these graphs:

| Configuration | Audited graph |
| --- | --- |
| Unit plus tooling | 551 targets / 580 registered tests |
| 16 KiB integration | 43 targets / 27 registered tests |
| 4 KiB integration | 44 targets / 27 registered tests |
| macOS product only | 21 targets |
| iOS device product only | 21 targets |
| iOS simulator product only | 21 targets |
| JIT link profile | 552 targets / 563 registered tests |
| JIT pressure profile | 551 targets / 562 registered tests |
| TSan unit configuration | 550 targets / 561 registered tests |

The audit covers normalized includes, forward and reverse declaration maps,
direct/object-composition edges, pure-interface and concrete cycles, semantic
test ownership, migration records, conditional registrations, source-size
reviews, documentation paths, and executable-memory primitive ownership. It
consumes the existing File API reply and never reconfigures its own build tree.
Isolated-link tests cover each CPU engine/factory edge. Two-real-kernel-handle
serialization, network-provider isolation, and deterministic overlapping
run-retirement/admission tests cover the process-wide and per-handle contracts.

That historical advisory source-size report covered 1,023 production,
test/support, CMake, and declarative files. Fifty-four exceeded a guideline, 43
lacked optional review context, and five reviewed files had changed in size.
The fresh seventh-audit source-size counts are recorded above; all such counts
are informational and cohesive logical units may remain large.

## Linux, dependencies, ABI, and rootfs (historical)

The exact Linux replay uses the signed official `linux-6.18.41.tar.xz` release
package (SHA-256
`17fc72f0f8d4a8a8633a5d20085f5d9c5a5ec51ee896a0b7ae1ec25da31273ea`) and the
11 final-state topic patches in `../kernel/patches/series/manifest.tsv`. The
packaged tree is
`3098cc6dba43578488a8f7f486797af8046ce8b8`; it omits only upstream's
export-ignored `Documentation/.renames.txt` relative to the recorded tag tree.
Independent authenticated replay reproduces final tree
`834e5c5faa8fc4638c9fdcbd40a1f29f5c7c2d5d`. Generated local import/replay
commit IDs are observations because their authentic committer metadata is
intentionally dynamic. The Darwin host-service topic has stable ID
`14c6b921eac5574938073b9aca3006fc5f9cac14` and SHA-256
`589f1c85669095f43f4a4bf9d6e1d71f6b6923ec0011b65e0711cbf1745b22f0`.
The independent DMA-mask topic has stable ID
`4293e280ee408165ae454f97819e652e00906581` and SHA-256
`b9f1494ad577cbc2bbe62f34daaac27e53f86b9d03a196fbed928be0872aebce`.

The historical closure run performed authenticated release replay plus independent
canonical no-fetch verification, 162 changed-file delta-ceiling checks, and
focused release-package dependency preparation/negative tests. Production
libraries are built from pinned official release packages into a project-private
arm64 static prefix. LibVNCServer uses the pristine pinned 0.9.15 release tree
without a project patch layer.
That historical run enforced archive, patch, prepared-tree, manifest,
build-policy, and full-prefix payload identities and rejected ambient/staging
paths and archive-member metadata mismatches. The current lean workflow keeps
authenticated source inputs, the manifest-bound receipt, required static arm64
payload, symlink exclusion, and the final executable's system-only dynamic
closure, but no longer hashes the installed prefix or attributes individual
archive members. The dated matrices also covered ABI synchronization and the
source-tree audit, and both 16 KiB and 4 KiB kernels built and booted at that
time.
Kernel and external-library timestamps, build identity,
user/host, compiler notes, package layout/hashes, and binary hashes are dynamic
observations rather than reproducible identities; source inputs, patch bytes,
exact source tree, and ABI identity are the enforced invariants. Public API v4
uses fingerprint `0xd9a69804d3303044` and declaration SHA-256
`d9a69804d33030448b780d58c3f553c0e12e4f1e19ab37f89d2719b8b93115f1`.
The independently versioned kernel bridge v4 uses fingerprint
`0xfe2d71a33e3d446e` and declaration SHA-256
`fe2d71a33e3d446ec929bad4ef91d8f7b59fa2c1718bbd3d137cb24627ba175b`.
Separate declaration histories and negative tests reject drift without an
explicit identity advance in the affected boundary.

As a historical observation, the rootfs remained 8,589,934,592 bytes, mode `0444`, SHA-256
`501770dcce8223a3b7b41c475413ccb5f733291e190dcb23964890b50dafc0e5`.
Provisioning, explicit maintenance authorization, recovery artifacts, lock
lifetime, and process-group cleanup are covered by host tests. No writable
rootfs maintenance was performed. These properties are no longer current
refactor acceptance gates.

## Apple package and executable memory (historical)

`scripts/build-xcframework.sh` rebuilt all three arm64 aggregate archives,
linked raw-build and installed-tree genuine C and C++ consumers for every
platform, audited the device/simulator archives, and recreated
`../.build/audit-round4-package/UMLEmulator.xcframework`. Its Info.plist
contains macOS arm64, iOS-device arm64, and iOS-simulator arm64. A local
`UMLBackend.xcframework` is an older artifact, not a current product.

Archive hashes, package layout details, tool versions, timestamps, and other
generated build metadata are intentionally dynamic observations. Acceptance is
anchored to source inputs, slice platforms/architectures, public declarations,
archive composition, executable-memory policy, and successful external links.

Interpreter and gadget state plus software-MMU storage are data-only; gadgets
execute ordinary precompiled signed RX text. They contain no W+X allocation
and make no RW/RX transition. Dynamic executable publication is confined to
`darwin/cpu/arm64/jit/jit_arena.cpp`, retained only for explicitly selected,
entitled debugger/jailbreak JIT use. Device and simulator symbol audits found
no `mprotect`, VM-protect, JIT-write-protect, or instruction-cache publication
reference outside that arena, and no production `jit::testing` support.

## Preserved performance observation

A full unprofiled Geekbench 6.7.1 run of the source-built-dependency
Release candidate completed at **80 single / 506 multi**
(result) with systemd as
PID 1, 10 guest cores, 8 GiB, gadgets, network access, and the immutable lower
image under tmpfs OverlayFS. This is +3.90%/-0.39% versus the immediately
preceding 77/508 run and +1.27%/0.00% versus the earlier G79 79/506 point. The
aggregate evidence does not show a broad gadget regression.

The executable SHA-256 observation is
`949a6ef31838e87b3d894dda96c43592a4878a2ac98f79c63dba8b6b7b700af6`.
The preserved console is
`../.build/results/refactor/audit-2026-08-03-release-deps/geekbench-gadgets-16k-console.log`,
SHA-256
`31df1439d98fe6b4773b9a6c05c00f85078e92e2cea701a44b844fe91bf7b86c`.

The prior single-core PDF anomaly resolved: 63 rose to 86, while multi-core PDF
rose from 535 to 547. The compiler scores moved from 48/392 to 51/385. A few
individual multi-core workloads regressed, led by HTML5 Browser at -4.58%, but
the overall multi-core score is effectively flat.

| Geekbench workload | Single | Multi |
| --- | ---: | ---: |
| File Compression | 104 | 601 |
| Navigation | 172 | 1,196 |
| HTML5 Browser | 73 | 562 |
| PDF Renderer | 86 | 547 |
| Photo Library | 78 | 617 |
| Clang | 51 | 385 |
| Text Processing | 50 | 61 |
| Asset Compression | 86 | 734 |
| Object Detection | 31 | 222 |
| Background Blur | 70 | 577 |
| Horizon Detection | 107 | 815 |
| Object Remover | 123 | 707 |
| HDR | 99 | 791 |
| Photo Filter | 101 | 431 |
| Ray Tracer | 98 | 961 |
| Structure from Motion | 74 | 620 |

## External acceptance (historical)

The following are intentionally not local pass claims because their inputs are
outside the repository:

- a suitable guest image containing the full LTP payload for gadgets and JIT;
- physical sandboxed/entitled iOS devices;
- opt-in public-network, interactive KDE/VNC, rendered WebGL, and browser
  campaigns; and
- repeated controlled gadget/JIT performance runs.

Missing external inputs are separate from the open repository-owned seventh-
audit work. Rootfs provisioning, identity, locking, and maintenance are
intentionally deferred until the testing-image redesign.
