# Shared Apple GL/GLES baseline

This file freezes the pre-implementation inputs and performance policy for
[`MAX_GL_GLES_SUPPORT_PLAN.md`](MAX_GL_GLES_SUPPORT_PLAN.md). It is evidence,
not a capability or conformance claim. The captured outer revision was clean
at `5974c6f687a34900d2d969f2ee2b01afa32b77b1` before this evidence was added.

## Shared-path implementation checkpoint (2026-08-21)

The Phase 1 source transition is now implemented: one `apple-gpu` manifest
product and offline builder produce verified macOS, Simulator, and device
prefixes from the same UTM ANGLE/libepoxy/virglrenderer commits and ordered
topics. All Apple GPU targets use the fixed ANGLE/Metal provider and native
Metal-texture contract; CGL, OpenGL.framework, the desktop graphics family,
and the layout-changing provider macro are absent from the current graph.
macOS stages the same flat EGL/GLES framework shape that Kernarium packages.

This checkpoint does not promote a guest profile. A focused 16 KiB macOS
diagnostic reached the immutable guest's two DRM nodes through real Xwayland
and virgl, naturally measured desktop GL 2.1 and GLES 3.0, passed one exact
frozen GLES 3 case, exported a complete QPA, and powered off cleanly. Host
compilation, prefix identity, packaging, and that single case are development
evidence. The same exact case now also passes through the signed 16 KiB
Kernarium app on the iOS 26.5 Simulator and connected iPad mini 6. Both report
desktop GL 2.1 and GLES 3.0 through real DRM/Xwayland/virgl, and both return
guest/runtime status zero. The first iPad shutdown exposed an intermittent
dirty DRM manager teardown; maintained kernel topic 0020 now shuts atomic
Virtio-GPU state down before unplug. Both LLVM 22 page-profile kernels rebuilt,
and three consecutive Simulator plus three connected-iPad exact-case runs then
powered off without the warning. The app adapters reject that signature and
missing clean-stop markers. The fixed identity sentinels now pass their five
GLES 2 and twelve GLES 3 cases under both 16 KiB and 4 KiB in strict
macOS-to-Simulator-to-iPad order. The current 20-config 16 KiB macOS GLES 2
and GLES 3 matrices now pass completely. Complete Simulator/iPad matrices,
the seven-pair performance campaign, connected-iPad thermal acceptance, and
sustained lifecycle campaigns remain outstanding.

The fixed Aquarium canary is now runnable on both permitted products and both
page profiles. An initial 4 KiB macOS run reached measurement but suffered two
RCU stalls while nested `mmu_gather` promoted a precise Chromium `mprotect`
range to a full address-space flush. Kernel topic 0021 preserves precise
Darwin nested ranges; the default generic behavior remains conservative. The
resulting Linux commit `28bfc7baf`, replay tree
`436f25b3c4cad9cbbead1d37bad19e219ce4075d`, fresh LLVM 22 16 KiB/4 KiB builds,
and accepted artifacts `mainline-6.18-{16k,4k}-20260821-28bfc7ba-1` verify
without a bridge-ABI change. Clean accepted-artifact runs at revision
`9f30073a` passed at 2.533/2.867 FPS on macOS and 5.767/5.933 FPS on Simulator
for 16 KiB/4 KiB, respectively; all used 30 samples, WebGL 2/GLES 3.0, clean
shutdown, and zero dropped or CPU-fallback frames. These single observations
are implementation canaries, not the seven interleaved pairs or a performance
qualification. Aquarium remains excluded from physical-iPad acceptance.

## Current macOS GLES qualification checkpoint (2026-08-22)

The GLES 3 campaign also exposed four common-path defects before reaching the
depth-inventory issue below. Managed Virgl topics `42b8c00` and `a32f188`
implement ES 3.0 multisample renderbuffers and the representable separate
transform-feedback layout. Managed WebKit topics `a25e51c` and `45a2a7b`
count transform-feedback queries per draw and compile only shaders that require
non-finite semantics without Metal fast math. Focused emulator reruns passed
the implicated official cases, and the complete current matrix below closes
their campaign-wide validation.

Qualification found that the earlier 25-config inventory was not truthful.
UTM Virgl's GLES format table advertised `VIRGL_FORMAT_Z32_UNORM` by mapping it
to `GL_DEPTH_COMPONENT24`. Guest Mesa therefore exposed normalized 32-bit
depth configurations, while the CTS correctly treated a 32-bit default depth
buffer as floating point and rejected depth blits before they reached the
host. Direct ANGLE and instrumented Virgl probes independently showed genuine
32-bit floating-point MSAA depth resolve working, complete FBOs, and a
successful host blit. Managed Virgl topic `3a287ef` and outer checkpoint
`e741cbad` remove only the false GLES normalized-depth advertisement; genuine
`Z32_FLOAT` and the native desktop `Z32_UNORM` path remain.

The Release inventory now contains exactly 20 non-MSAA configurations: IDs
1-4, 13-16, 25-28, 37-40, and 49-52. All support window, pixmap, and pbuffer
surfaces. Its canonical SHA-256 is
`843544a7aef987426f74851a8802447a2428bde9ff9a5d893e1c2073ffbb2607`,
captured at
`.build/vk-gl-cts/qualification/gles3-z32-release-config-inventory/20260822-060342-60650-16k-config-inventory-gles3`.
The former 25-config inventory and matrix are retained as diagnostic history,
not current qualification evidence.

Current GLES 2 campaign
`.build/vk-gl-cts/matrix/gles2-16k-e741cbad-z32-truthful` ran the exact 473-case
official leaf once against every current configuration. Its 20 immutable
shards contain 9,460 executions: 8,760 `Pass`, 700 Khronos-permitted
`NotSupported`, and no disallowed status. Independent verify-only
reconstruction reported
`Verified 9460 gles2 case executions in 20 immutable shards`.

The clean detached campaign
`.build/vk-gl-cts/matrix/gles3-16k-e741cbad-z32-truthful` ran the exact 6,498
case Khronos GLES 3 leaf once against every current configuration through VNC,
JIT, ten vCPUs, 8 GiB, a sealed read-only ext4 lower, disposable OverlayFS
state, and acknowledged Virtio serial transfer. All 20 immutable shards passed
their artifact and clean-shutdown checks. The exact 129,960 executions contain
125,270 `Pass`, 4,690 Khronos-permitted `NotSupported`, and no disallowed
status. Independent verify-only reconstruction reported
`Verified 129960 gles3 case executions in 20 immutable shards`. The derivative
remained mode 0444 at SHA-256
`f3ebadb579988892f4e33524b3101e82437199f876a7463aae092251fccec57a`.
Together these campaigns complete the current 16 KiB macOS GLES 2 and GLES 3
matrices. The signed iOS 26.5 Simulator subsequently ran fixed generated GLES
2 and GLES 3 config-inventory campaigns at
`.build/vk-gl-cts-ios-simulator/20260822-093938-4195-16k-gles2-config-inventory-v1`
and
`.build/vk-gl-cts-ios-simulator/20260822-093844-4028-16k-gles3-config-inventory-v1`.
Both matched the same 20 config tuples and canonical digest exactly, used
Gadgets with direct ANGLE/Metal presentation and the sealed lower, returned
zero, and recorded no dropped or CPU-fallback frames. This establishes the
mobile matrix geometry; it is not a case-matrix result. The complete
Simulator/iPad matrices remain required.

## Authenticated derivative-rootfs checkpoint (2026-08-21)

The user explicitly authorized a separate 8 GiB ext4 clone for CTS
reconstruction. The original `rootfs.erofs` remains unchanged at SHA-256
`73e36c063e8374b68bdaca992f789d41dda59e159fd4b426dbbb5515dcf84c91`.
The derivative `.build/rootfs/rootfs-gl-gles.ext4` has filesystem UUID
`688cb152-ce4b-4ac4-9b5d-3def792791f3`, exact size 8,589,934,592 bytes, and
final SHA-256
`f3ebadb579988892f4e33524b3101e82437199f876a7463aae092251fccec57a`.
Its checked authority is
[`../tests/integration/gpu/vk_gl_cts_derivative_rootfs.json`](../tests/integration/gpu/vk_gl_cts_derivative_rootfs.json).
That receipt freezes the exact sorted exclusion list and its SHA-256
`5e8fd90745be07768de57e8826d5cdc25c9c64d54c38c29ca15e0cabe6f0f1bf`.
The clone omits unrelated benchmark suites, Node, Go, installed Vulkan CTS,
and Vulkan/Vulkan-SC source payloads. It retains only the 12,895,859-byte
shared Vulkan framework required by upstream `tcutil-platform` through static
`vkutil`; its semantic archive SHA-256 is
`a7c6bc03a0678e164cf6112f88ef8422f97d84eda4f0242b42b81b8ba7c8427a`.

The selected 1.4.5.5 source closure contains 12,964 entries and 356,332,856
bytes with POSIX archive SHA-256
`3eaf1fe6937de7db36966ba63a162ddffd980746386daf1c46ae617cc1ad0d25`.
Inside the emulator, a clean Release build used the JIT engine, ten vCPUs,
and `cmake --build --parallel 10`; the derivative root device was already
block-read-only and build output lived on a separate scratch ext4 disk. Only
`deqp-gles2`, `deqp-gles3`, and `glcts` were selected. Their SHA-256/build-ID
pairs are
`6f377730dd919c66970d7c8e06aaca76f1487eeb1ec3ba3c8d05e6d604ad297a`/
`308925e0d2d85b72ae581894f912bccfa96c2587`,
`456eb99a3ba1c8198fa73fe70a6fdcb1b8f0f51d2bb319d44ea7a63058a8d635`/
`f4e13c9880ce3594e4fa9fb666dd746336a3afa5`, and
`cfb17f07d4e7e78ed71a2c065670465c720d9109bb557f4316cbabd1f1c9834e`/
`c3980d5ebb6f4e1588bba36fe5b91e01bcc8e4d5`. The link audit found no
selected Vulkan module, Vulkan SC, Shaderc, or runtime Vulkan dependency.
Installation replaced only those executables and preserved all six
authenticated base resource trees verbatim. The resulting in-guest receipt
at `/usr/share/uml-rootfs/gl-gles-cts-build.env` has SHA-256
`97b4decef91fea71b4e221cf21a4e8d2ca20fb88514498c2310b02b026142933`.

The maintenance epoch ended only after the root remounted read-only, the
emulator shut down cleanly, and a block-read-only filesystem check passed.
The host file is mode 0444. Every later boot attached it with `ubd0cr`, mounted
it as a read-only ext4 lower, and supplied writable state through OverlayFS.
Exact JIT/ten-vCPU validation passed under both 16 KiB and 4 KiB at
`.build/rootfs/overlay-validation-16k/20260821-193546-39978` and
`.build/rootfs/overlay-validation-4k/20260821-193934-40379`. The derivative
then passed the declaration sentinel 1/1 and the GLES 2/GLES 3 identity
sentinels 5/5 and 12/12 on native macOS under both page profiles, with clean
shutdown and zero CPU fallback. The signed Simulator app also passed the
declaration sentinel 1/1 with the derivative under both page profiles at
`.build/vk-gl-cts-ios-simulator-derivative/20260821-195324-52203-16k-gles3-declaration-sentinel-v1`
and
`.build/vk-gl-cts-ios-simulator-derivative/20260821-195421-52319-4k-gles3-declaration-sentinel-v1`.

This closes build provenance only for the exact derivative. The original
EROFS CTS build receipt is still missing. The initial all-package diagnostic
at
`.build/vk-gl-cts-derivative/20260821-194351-41416-16k-inventory` reproduced
the original failure at `dEQP-GL45-ES3`: status 137, `complete=0`, eight
registered modules, 148,476 rows, and the same partial SHA-256
`dbad92115f7c3a80891f6842faa191e6cb02c577955ae31fc38ba5e2e1bea5dc`.
No partial all-package identity was promoted.

A native arm64 Debian 13 VM control at
`.build/vk-gl-cts-vm-control/20260821-native-arm64` used the exact installed
CTS payload, including `glcts` SHA-256
`cfb17f07d4e7e78ed71a2c065670465c720d9109bb557f4316cbabd1f1c9834e`
and build ID `c3980d5ebb6f4e1588bba36fe5b91e01bcc8e4d5`. The VM's direct Mesa
25.0.7 renderer was
`virgl (ANGLE (Apple, Apple M1 Ultra, OpenGL 4.1 Metal - 90.5))`, with the
same natural desktop GL 2.1 and GLES 3.0 ceiling. Private, test-local
`libEGL.so`, `libGL.so`, and `libGLESv2.so` symlinks supplied the unversioned
names that the CTS opens dynamically; no VM system file was changed. The
multi-package export then failed at the same `dEQP-GL45-ES3` context request
with status 137, the same eight nonempty modules and 148,476 rows, and the
same partial digest above. By contrast, the exact standalone `deqp-gles2` and
`deqp-gles3` exports returned zero with 19,825 and 48,300 tests and SHA-256
`d727f1fd0f051a7dde6fbb67a0cf734ee01d111fbc36c274a6945a3ca4a046fd`
and `bb24f19fdab379c38530c62a8a354ba102d676b75777ab3b654da063e2153860`.
Filtered `glcts` exports of `KHR-GLES2.*` and `KHR-GLES3.*` also returned zero
in the VM, producing the same 473-case and 6,499-case package hierarchies later
captured in the emulator.

The VM observation is diagnostic only and is not a conformance oracle. The
released source, rather than agreement with the VM, accounts for why the
global traversal reaches this out-of-scope context request.
`framework/common/tcuTestHierarchyUtil.cpp` inflates every registered package
during a text-caselist export, and
`external/openglcts/modules/glcTestPackageRegistry.cpp` registers
`dEQP-GL45-ES31` immediately before `dEQP-GL45-ES3`.
`modules/gles31/tes31Context.cpp` uses `glu::EmptyRenderContext` outside
execute mode, so the former package lists successfully. The GLES 3 context in
`modules/gles3/tes3Context.cpp` has no corresponding run-mode branch, while
`modules/gles3/tgl45es3TestPackage.cpp` requests `glu::ApiType::core(4, 5)`,
so the latter package asks a GL 2.1 renderer for a real GL 4.5 context and
receives `EGL_BAD_MATCH`. Rebuilding the same unmodified source would preserve
that request. This source diagnosis scopes the inventory; it does not prove
the emulator conformant or excuse any official case failure. Patching CTS listing
semantics would change the qualification source, so no second rootfs rebuild
was performed. The all-package traversal is not a truthful inventory boundary
for GLES qualification: `dEQP-GL45-ES3` is an ES test package deliberately run
on a desktop GL 4.5 context, not a requirement of the GLES 2 or GLES 3 profile.

The corrected inventory invokes the unmodified installed `glcts` separately
for `KHR-GLES2.*` and `KHR-GLES3.*`, requires exactly those two nonempty
package files, rejects any cross-module row, and concatenates them in fixed
GLES 2 then GLES 3 order. The real 16 KiB ANGLE/Metal emulator run at
`.build/vk-gl-cts/20260821-205148-73388-16k-inventory` completed with JIT, ten
vCPUs, the checked derivative mounted as a read-only ext4 lower, status zero,
clean shutdown, 6,972 hierarchy cases, and SHA-256
`3ec390ed2d36f0fd8e32a02718e19c5750cf9381eab002aac9b71f848ebc6162`.
The frozen modules are exactly `KHR-GLES2` and `KHR-GLES3`. Their installed
official must-pass leaves contain 473 and 6,498 cases respectively; the one
additional GLES 3 hierarchy case is
`KHR-GLES3.texture_filter_anisotropic.drawing`, which the released Khronos
must-pass leaf does not select. The current 20-config 16 KiB macOS GLES 3
cross-product now passes as recorded above, as does the matching 20-config
GLES 2 cross-product. Qualification remains fail-closed pending complete
Simulator/iPad matrices and the other Phase 2 gates, and the original EROFS
image still lacks build provenance; the scoped caselist itself is no longer a
blocker.

The current ordered 16 KiB capability diagnostic is preserved at
`.build/vk-gl-cts/20260821-042014-33014-16k-gles3`,
`.build/vk-gl-cts-ios-simulator/20260821-042130-33165-16k-gles3-declaration-sentinel-v1`,
and
`.build/vk-gl-cts-ios-device/20260821-042215-33330-16k-gles3-declaration-sentinel-v1`.
The Simulator adapter accepted the first run as its mandatory macOS
predecessor, and the device adapter accepted the Simulator run before
installing the signed app. Each product explicitly requested host ES 3.0, 3.1,
and 3.2 contexts. ES 3.0 succeeded; ES 3.1 and 3.2 both failed naturally with
`EGL_BAD_MATCH` (`0x3004`). The ES 3.0 extension counts are 117 on the M1 Ultra,
98 on the Simulator GPU, and 109 on the iPad's A15 GPU.

The same run captured and decoded every field and raw byte in Virgl capset 1
(version 1, 308 bytes) and capset 2 (version 2, 1,408 bytes). Their respective
SHA-256 pairs are `37ddba6b...`/`e074ac11...` on macOS,
`996cec8c...`/`185907f8...` on Simulator, and
`5bd1ec0d...`/`f205336a...` on iPad. All report GLSL level 130. The exact
macOS-to-Simulator diff removes `start_instance`, one render format, eleven
sampler formats, the 3D-ASTC capability, three multisample formats, three
readback formats, and changes `max_vertex_outputs` from 30 to 31 plus the
renderer name. The Simulator-to-iPad diff adds `start_instance`, one render
format, three sampler formats, 3D ASTC, one multisample format, three readback
formats, and changes only the renderer name among scalar limits. The complete
format-number sets, bit values, arrays, video slots, raw payloads, and semantic
diffs remain in the three `run.json` files; equal version strings are not used
as a substitute for that comparison.

All three exact CTS cases passed with the same selected-list and renderer
artifact identities. Final native-display counters were macOS 124 guest/299
presented/1 dropped, Simulator 100/201/1, and iPad 618/942/0; every product
reported zero CPU-fallback frames and clean guest/runtime shutdown. These
short-run counters prove direct presentation and fallback absence only. They
are not the frozen sustained frame-pacing, CPU/GPU, memory, energy, or thermal
qualification campaign.

The schema-2 campaign authority additionally freezes a five-case GLES 2
identity sentinel across negative and optimized shaders, internal formats,
copy/renderbuffer behavior, and 3D texture filtering, plus a twelve-case GLES
3 identity sentinel across extension/API coverage, framebuffer blit and
completeness, packed depth/pixels, shaders, driver errors, parallel compile,
compressed texture storage, and synchronization-sensitive updates. The final
ordered GLES 2 chain is
`.build/vk-gl-cts/20260821-050712-53069-16k-gles2-identity-sentinel-v1`,
`.build/vk-gl-cts-ios-simulator/20260821-051121-61347-16k-gles2-identity-sentinel-v1`,
and
`.build/vk-gl-cts-ios-device/20260821-051207-61486-16k-gles2-identity-sentinel-v1`;
the GLES 3 chain is
`.build/vk-gl-cts/20260821-045622-44382-16k-gles3-identity-sentinel-v1`,
`.build/vk-gl-cts-ios-simulator/20260821-050221-52605-16k-gles3-identity-sentinel-v1`,
and
`.build/vk-gl-cts-ios-device/20260821-050341-52752-16k-gles3-identity-sentinel-v1`.
Every run passed its exact ordered set, powered off cleanly, and reported zero
CPU-fallback frames. The selected-list digests are
`1248dc6887e7ba9bb0316da73fc48961f7051c27c6f3bfc420d7361410a4d7eb`
for GLES 2 and
`495136cf6676bdfdaa0ba3838b1c97de83fed3296f43de06d67f93cd0a5759bd`
for GLES 3.

The ordered 4 KiB GLES 2 chain is
`.build/vk-gl-cts/20260821-053254-80436-4k-gles2-identity-sentinel-v1`,
`.build/vk-gl-cts-ios-simulator/20260821-053355-80513-4k-gles2-identity-sentinel-v1`,
and
`.build/vk-gl-cts-ios-device/20260821-053448-80794-4k-gles2-identity-sentinel-v1`;
all passed 5/5. Their QPA SHA-256 values are
`a7806d4e004aea920136f6deceab7e7d127c3adc858704c60e53b0797d958c46`,
`c970b6bd184b6f650cc0652a4e2175f774e910ca62fb014b0ab3717bda5457d3`,
and `9d438964b4a92796f06bea27bedfbb3fc0bb0b0bd0f9e6b922a30db0e91c8dd1`;
final guest/presented/dropped/CPU-fallback counters are 193/379/1/0,
135/268/0/0, and 211/466/0/0. The 4 KiB GLES 3 chain is
`.build/vk-gl-cts/20260821-053622-80929-4k-gles3-identity-sentinel-v1`,
`.build/vk-gl-cts-ios-simulator/20260821-053835-81090-4k-gles3-identity-sentinel-v1`,
and
`.build/vk-gl-cts-ios-device/20260821-053953-81340-4k-gles3-identity-sentinel-v1`;
all passed 12/12. Their QPA SHA-256 values are
`fa9907e76ad25b7f623e7a48cc35e009200518b3851b9bb29111e1bae2bb6897`,
`c81dcde869dc010d86580402cfe0a05b6446aecdd4365736bbc6ed43c3a9727d`,
and `93b857107bab3c93be3e49fdc3a4439edff297d2af3bbd9ba67784b40edaf96e`;
counters are 520/931/1/0, 491/699/0/0, and 539/726/0/0. Both chains retain
the same selected-list digests as their 16 KiB counterparts, shut down
cleanly, and prove page-profile parity only for this bounded corpus.

Sentinel selection exposed three diagnostic non-passes; none was waived. The
macOS run at
`.build/vk-gl-cts/20260821-045416-44189-16k-gles3-identity-sentinel-v1`
reported `KHR-GLES3.shaders.fragdepth.write.const` as `NotSupported` because a
depth buffer was required and failed
`KHR-GLES3.transform_feedback2.transform_feedback2_states` with query result
`0 != 2`. The current complete macOS matrix classifies the depth case as 15
`Pass` plus five Khronos-permitted `NotSupported` results on configurations
without a depth buffer. The transform-feedback query case passes on all 20
configurations after the Virgl separate-buffer and ANGLE per-draw query
repairs. The Simulator run at
`.build/vk-gl-cts-ios-simulator/20260821-050029-52429-16k-gles2-identity-sentinel-v1`
failed `KHR-GLES2.core.internalformat.texture2d.rgba_unsigned_byte_rgba` with
an image-comparison difference after that case had passed on macOS. That
finding was repaired in the common ANGLE/Metal path rather than waived. The
final 4,096 MiB Simulator campaign
`.build/vk-gl-cts-ios-simulator-matrix/gles2-16k-ce8cfa0f-apple8-restored-4g`
subsequently passed the full 20 x 473 product: 8,760 `Pass`, 700
Khronos-permitted `NotSupported`, no disallowed result, and an independent
verify-only reconstruction of all 9,460 executions.

A normal 16 KiB iPhone 17 Pro Max Simulator boot separately selected
`KernariumRuntime_6_18_41_16K`, emitted the hardware ANGLE/Metal marker and
`Native Metal display: 440x860`, exposed Virgl capsets 1 and 2, mounted the
3,802,316,800-byte immutable image, and rendered KDE at 440x860/160 DPI. The
host then terminated the app successfully; orderly guest shutdown and a
Simulator CTS result were not claimed by that normal desktop capture. Both
page-profile runtimes embed a launcher generated from the exact allow-listed
GLES 3 sentinel. The first 16 KiB matched pair remains preserved at
`.build/vk-gl-cts-ios-simulator/20260821-020501-10804-16k-gles3-declaration-sentinel-v1`
and
`.build/vk-gl-cts-ios-device/20260821-020554-10897-16k-gles3-declaration-sentinel-v1`.
Both passed 1/1 and returned guest/runtime status zero, but the iPad emitted
`Memory manager not clean during takedown`. After topic 0020, the three
Simulator runs ending in `022946`, `023030`, and `023106` and the three gated
iPad runs ending in `023151`, `023443`, and `023640` all passed 1/1 and met the
new clean-shutdown validator. These repetitions resolve the focused defect but
do not substitute for the plan's sustained lifecycle campaign.

## Host, dependency, and kernel input

The capture host is an arm64 Apple M1 Ultra Mac Studio with 64 GiB of memory,
macOS 26.3 (25D125), Xcode 26.6 (17F113), macOS SDK 26.5, and Apple Clang
21.0.0 (`clang-2100.1.1.101`). Mutable host identifiers are intentionally not
recorded.

The checked iOS GPU platform contracts are format 2, arm64, deployment target
16.0, static-plus-framework payloads using ANGLE EGL/GLES and Metal. The
ordinary installed macOS prefix has a schema-2 receipt built with SDK 26.5 and
still contains the removal-input desktop libepoxy/virglrenderer family. No iOS
GPU prefix was installed at capture time. Central dependency-manifest
validation passed; offline `ios-gpu` verification stopped because the pinned
UTM repositories had not yet been prepared. Phase 1 must replace these two
families with one `apple-gpu` product rather than treating either absence as a
skip.

The pinned Linux input is stable 6.18.41 commit
`2fe596715f840d053aed5cee5455f701bdcd2b50`; the reviewed replay tree is
`f78217b082f6becb8af1656bcc69521307eb729b`. The configured checkout was clean
on the 19-topic branch. Existing production configurations had these exact
identities:

| Profile | `PAGE_SHIFT` | `.config` SHA-256 |
| --- | ---: | --- |
| 16 KiB | 14 | `54eedbec0cc68c63b17ad25e8c9cdc48352564d30c9d351c567996b4ccb37893` |
| 4 KiB | 12 | `554f99d5bbe0b2c30e7d9d9b9e55500bd8d1bfa1f00d8de46d4f97e48a31bf68` |

Both configurations build DRM, Virtio, and `DRM_VIRTIO_GPU` in. The
Git-ignored source-location configuration was absent from the outer tree at
capture time, so a future configure or replay must restore that explicit local
configuration before it can be claimed.

## Immutable guest and CTS identity

`rootfs.erofs` passed its schema-3 manifest verifier at 3,802,316,800 bytes and
SHA-256
`73e36c063e8374b68bdaca992f789d41dda59e159fd4b426dbbb5515dcf84c91`.
The image is Debian 13.6 with Mesa 25.0.7-2+deb13u1 and libdrm 2.4.124-2.

Read-only guest inspection confirmed the three CTS executable sizes, SHA-256
digests, and GNU build IDs, plus both Khronos `mustpass.xml` files and every
GLES/desktop leaf count and digest. The machine-readable authority is
[`../tests/integration/gpu/vk_gl_cts_identity.json`](../tests/integration/gpu/vk_gl_cts_identity.json).
No original-rootfs byte or provisioning metadata was changed.

No authoritative receipt exists in the image tying the co-located
`/home/uml/vk-gl-cts-1.4.5.5` source to the installed executables. A read-only
search found no CMake cache, Ninja log, compile database, install manifest,
configuration log, build-info file, or receipt for this CTS build. The source
also has no Git metadata. Therefore:

- the installed CTS is valid for diagnostic development slices;
- official qualification is blocked and the runner refuses
  `--qualification`;
- no commit, source reconstruction, or conformance claim is inferred from the
  1.4.5.5 directory label; and
- the checked derivative above is the only explicitly authorized image/CTS
  exception; any later change requires new user direction.

A global all-package text export remains unavailable and is retained only as
a diagnostic. On the rebuilt 16 KiB common ANGLE/Metal emulator, the exact
binary exported eight nonempty module hierarchies and began
`dEQP-GL45-ES3`, then aborted with `EGL_BAD_MATCH` from
`eglCreateContext()`. The partial diagnostic contains
148,476 `TEST:` rows across `CTS-Configs`, `dEQP-EGL`, `KHR-GLES2`,
`dEQP-GLES2`, `KHR-GLES3`, `dEQP-GLES3`, `dEQP-GLES31`, and
`dEQP-GL45-ES31`; its raw concatenated digest is
`dbad92115f7c3a80891f6842faa191e6cb02c577955ae31fc38ba5e2e1bea5dc`.
It is explicitly `complete=0`, exited 137, and is not a frozen identity. The
guest nevertheless powered off cleanly, with evidence at
`.build/vk-gl-cts/20260821-032519-85656-16k-inventory`. Because the fixed
GL 2.1/GLES 3.0 renderer cannot create every context demanded during this
multi-package enumeration, that result is not a GLES qualification identity.

The native arm64 Debian diagnostic described above reproduces this exact
partial digest and failure using the same CTS bytes over direct virgl, while
the standalone and filtered Khronos GLES 2 and GLES 3 text exports complete.
That is useful corroboration, but it does not isolate or clear UML, Darwin's
Virtio-GPU transport, either guest page profile, or any official CTS result.
The emulator's complete official matrices are the acceptance authority.

The repository runner's `--inventory` mode now owns the exact lower shared
baseline scope instead of every package linked into `glcts`. It exports only
`KHR-GLES2.*` and `KHR-GLES3.*` through the real display, validates the exact
two-module boundary, concatenates in fixed profile order, and checks the
frozen 6,972-case digest
`3ec390ed2d36f0fd8e32a02718e19c5750cf9381eab002aac9b71f848ebc6162`.
The checked identity is complete for that GLES 2/GLES 3 hierarchy scope; the
separately hashed official must-pass leaves remain the execution authority.

## Frozen campaign and performance policy

The first runnable shared-path baseline uses the immutable guest above. Within
each platform, an A/B keeps the page profile, guest CPU count and memory,
display geometry/density, engine, seed, workload, warm-up, measurement window,
and run order fixed. Cross-platform numbers are reported separately and are
not treated as interchangeable samples.

The lower-profile corpus is frozen as:

1. the renderer/DRM/capset identity sentinel plus fixed context, clear/draw,
   shader, buffer, texture, synchronization, readback, and negative-limit
   cases;
2. the complete shipped GLES 2 and GLES 3.0 Khronos matrices once exact
   `mustpass.xml` expansion and build provenance are available;
3. the local instancing reproducer and rendered WebGL functional pages; and
4. WebGL Aquarium with 500 fish, a 15-second warm-up, thirty one-second FPS
   samples, and the ordinary native-display frame counters. Aquarium runs on
   macOS and Simulator only.

New Simulator qualification runs use 4,096 MiB of guest RAM. Their required
native macOS predecessor remains fixed independently at 1,536 MiB; cross-product
numbers are reported separately and are not treated as interchangeable
samples. The earlier single-run Simulator canaries retain their historical
memory setting and remain bring-up evidence only.

Performance comparisons use seven interleaved lower/candidate pairs after one
unscored bring-up run per product. A control series is acceptable only when
its median throughput/frame-time variation is at most 1% and its p95/resource
variation is at most 2%; noisier evidence is inconclusive and is rerun rather
than widening the envelope. The candidate passes only when its confidence
interval also remains inside all applicable limits:

- at most 3% throughput loss or frame-time/resource increase for unaffected
  lower-profile workloads;
- at most 5% median throughput loss or frame-time increase for
  candidate-feature workloads;
- at most 10% increase in p95 frame time, host/guest CPU, GPU time/energy, or
  resident memory; and
- zero sustained native-display CPU readback, per-draw or per-dispatch
  synchronous feedback, app termination, or lifecycle failure. Device runs
  begin only after a nominal thermal probe; later thermal state changes are
  observational unless they cause a measured performance-budget or test
  failure.

Candidate-only operations use a frozen equivalent direct-Metal reference and
record submissions, passes, bytes moved, CPU/GPU time, and command/memory
amplification before a profile claim. A CTS pass cannot override a failed or
noisy performance gate.

## Archived renderer boundary

The older macOS CGL result—guest desktop GL 4.1 and GLES 3.0—is historical
context only. It is not rebuilt, rerun, retained as a preset, used as an A/B,
or accepted by the new CTS driver. The focused macOS diagnostic measured
desktop GL 2.1 and GLES 3.0 through ANGLE/Metal, and the exact signed-app
sentinel measured the same values on Simulator and iPad. The shared candidate
remains unqualified until all three produce the complete required correctness,
performance, thermal, and lifecycle evidence against an authoritative CTS
identity.
