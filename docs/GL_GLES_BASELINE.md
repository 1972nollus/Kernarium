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
macOS-to-Simulator-to-iPad order. Complete caselist identity and matrices,
performance, thermal, and sustained lifecycle campaigns remain outstanding.
The authoritative CTS build
receipt is still missing, so qualification remains fail-closed and no rootfs
or guest-userspace change is authorized.

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

Sentinel selection also exposed three diagnostic non-passes that remain open,
not waived. The macOS run at
`.build/vk-gl-cts/20260821-045416-44189-16k-gles3-identity-sentinel-v1`
reported `KHR-GLES3.shaders.fragdepth.write.const` as `NotSupported` because a
depth buffer was required and failed
`KHR-GLES3.transform_feedback2.transform_feedback2_states` with query result
`0 != 2`. The Simulator run at
`.build/vk-gl-cts-ios-simulator/20260821-050029-52429-16k-gles2-identity-sentinel-v1`
failed `KHR-GLES2.core.internalformat.texture2d.rgba_unsigned_byte_rgba` with
an image-comparison difference after that case had passed on macOS. These cases
are outside the small cross-product identity sentinel, but remain required
classification work for the complete matrices.

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
No rootfs byte or provisioning metadata was changed.

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
- any proposal to change the image or CTS inputs requires explicit user
  direction first.

A complete text-caselist identity also remains unavailable, now with a
reproducible display-backed blocker. The repository runner's `--inventory`
mode uses the installed binary's package-aware export pattern, concatenates
the transient package files in C-locale order, and records only a strict
digest/count/module report after a zero exit. On the rebuilt 16 KiB common
ANGLE/Metal emulator, the exact binary exported eight nonempty module
hierarchies and began `dEQP-GL45-ES3`, then aborted with
`EGL_BAD_MATCH` from `eglCreateContext()`. The partial diagnostic contains
148,476 `TEST:` rows across `CTS-Configs`, `dEQP-EGL`, `KHR-GLES2`,
`dEQP-GLES2`, `KHR-GLES3`, `dEQP-GLES3`, `dEQP-GLES31`, and
`dEQP-GL45-ES31`; its raw concatenated digest is
`dbad92115f7c3a80891f6842faa191e6cb02c577955ae31fc38ba5e2e1bea5dc`.
It is explicitly `complete=0`, exited 137, and is not a frozen identity. The
guest nevertheless powered off cleanly, with evidence at
`.build/vk-gl-cts/20260821-032519-85656-16k-inventory`. Because the fixed
GL 2.1/GLES 3.0 renderer cannot create every context demanded during this
multi-package enumeration, the complete digest, count, and module fields in
the JSON remain `null` and promotion remains disabled.

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
  synchronous feedback, new thermal escalation/throttling, app termination,
  or lifecycle failure.

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
