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
evidence; the complete identity/capset sentinel, caselist export, frozen
macOS/Simulator measurements, and connected-iPad campaign remain outstanding.
The authoritative CTS build receipt is still missing, so qualification remains
fail-closed and no rootfs or guest-userspace change is authorized.

A normal 16 KiB iPhone 17 Pro Max Simulator boot separately selected
`KernariumRuntime_6_18_41_16K`, emitted the hardware ANGLE/Metal marker and
`Native Metal display: 440x860`, exposed Virgl capsets 1 and 2, mounted the
3,802,316,800-byte immutable image, and rendered KDE at 440x860/160 DPI. The
host then terminated the app successfully; orderly guest shutdown and a
Simulator CTS result are not claimed. Both page-profile runtimes now embed a
launcher generated from the exact allow-listed GLES 3 sentinel, but the
matching Simulator campaign still has to run.

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

A complete text-caselist export also remains pending. `glcts` initializes its
X11 platform before it can finish that export; the pre-migration CGL product
is not rerun or preserved merely to obtain it. Although the common
ANGLE/Metal path now builds, its display-backed guest export has not yet been
run. Until the registered-module list, digest, and count are frozen, the
corresponding JSON fields stay `null` and promotion remains disabled.

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
desktop GL 2.1 and GLES 3.0 through ANGLE/Metal, but the shared candidate
remains unqualified until macOS, the iPhone 17 Pro Max Simulator, and the
connected iPad each produce the complete required evidence.
