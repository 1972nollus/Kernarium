# Virtio-GPU/Virgl OpenGL on Apple hosts

This document owns the implemented Virtio-GPU/Virgl architecture. The
capability and performance ladder is in
[`MAX_GL_GLES_SUPPORT_PLAN.md`](MAX_GL_GLES_SUPPORT_PLAN.md); test commands and
current evidence are in [`testing.md`](testing.md) and
[`refactor-evidence.md`](refactor-evidence.md).

## Current architecture

All three Apple products use one host GL path:

```text
guest OpenGL/GLES application or compositor
        -> released Mesa Virgl Gallium + GBM/EGL
        -> Linux virtio_gpu DRM/KMS
        -> UML Virtio/vhost-user transport
        -> VirtioGpuDevice
        -> virglrenderer + UTM libepoxy
        -> product-fixed ANGLE EGL/GLES Metal backend
        -> native MTLTexture
        -> common newest-frame-wins Metal presentation
           -> AppKit MTKView | UIKit MTKView

unsupported/diagnostic scanout -> bounded CPU readback
                                -> native diagnostic frame | macOS VNC
```

`HostGlProvider` owns the external ANGLE display, ES 3 configuration, context
sharing, make-current behavior, and EGL teardown. It requests only
`EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE` with the hardware device type. The
provider is opaque inside a fixed-layout `VirglRenderer`; macOS, Simulator,
and device cannot select another GL provider. Construction, submissions,
reset, and teardown remain on the GPU transport thread because EGL current
context is thread-local.

Virglrenderer exports a checked native Metal resource. A common device-owned
presentation state accepts only a same-device, full-frame texture, retains the
newest generation, and permits at most one Metal command buffer in flight.
AppKit owns its main-thread window and keyboard/pointer/wheel translation;
UIKit owns the application-provided `MTKView`, safe-area sizing, and touch/text
lifecycle. VNC is a macOS-only CPU-readback consumer and cannot create a GL
context, select a provider, or affect capsets.

CGL, EAGL, OpenGL.framework, the OpenGL-texture/IOSurface bridge, ANGLE's
Vulkan backend, MoltenVK, Zink, and the former desktop libepoxy/virglrenderer
family are absent from the GL product graph. The old macOS CGL result remains
archival evidence only.

## Dependency and runtime contract

One central `apple-gpu` manifest product pins UTM WebKit/ANGLE, UTM libepoxy,
and UTM virglrenderer plus their ordered generated mail-patch series.
`dependencies/build-apple-gpu.sh` builds exactly these arm64 prefixes:

```text
.build/dependencies/apple-gpu/macos-arm64
.build/dependencies/apple-gpu/ios-simulator-arm64
.build/dependencies/apple-gpu/ios-device-arm64
```

Each format-3 platform contract records the platform, deployment target,
static libraries, flat EGL/GLES frameworks, fixed ANGLE/Metal provider, native
texture export, and common recipe. Verification compares installed provenance
byte-for-byte and rejects an unexpected payload or symlink, wrong architecture
or Mach-O platform, wrong minimum OS, invalid install name/rpath/client rule,
unexpected dynamic closure, missing platform-display export, or CGL/EAGL/
Vulkan provider symbol in either framework or static archive.

The macOS executable stages exactly `EGL.framework` and `GLESv2.framework`
under `@executable_path/Frameworks`; the runtime verifier requires one exact
copy and compares both bundles to the authenticated prefix. Kernarium's
packager independently revalidates the Simulator and device prefixes before
creating the corresponding XCFrameworks. The ordinary macOS prefix contains
only non-graphics dependencies.

## Host and guest ownership

The device implements its existing one-control/one-cursor queue contract,
Virgl capsets 1 and 2, 2D/3D resource lifecycle, transfers, scanout, context
commands, command submission, and ordered fences. A checked native-resource
result decides direct presentation. Accepted or superseded direct frames skip
CPU readback; unsupported resources take the counted compatibility path.
Native performance evidence requires zero sustained fallback.

The maintained mainline kernel already builds standard Virtio-GPU DRM/KMS,
UML DMA/IOMEM, Virtio UML, and Virtio input support for both page profiles.
The host-provider transition changes no Linux topic, Kconfig value, bridge ABI,
or kernel image. Host-visible blobs, a shared-memory aperture, and mapping/
coherency work remain conditional optimization or Vulkan scope and are not
advertised by this implementation.

The authenticated `rootfs.erofs` and its Mesa/libdrm/CTS userspace remain
immutable. Guest evidence must use `/dev/dri/card0` and
`/dev/dri/renderD128`, natural capset discovery, the real Virgl renderer, and
the image's graphical session. Version overrides, software renderers, guest
Zink, Xvfb, library preloads, or overlay-substituted graphics components are
not valid evidence.

## Verification order and current boundary

Run the common macOS host/unit loop first, then compile and validate the same
source and recipe for Simulator, then device:

```sh
dependencies/prepare.sh --product apple-gpu --verify
dependencies/build-apple-gpu.sh --platform macos --verify-only
cmake --workflow --preset check-gpu
cmake --workflow --preset check-gpu-asan-ubsan

dependencies/build-apple-gpu.sh --platform ios-simulator --verify-only
cmake --preset kernarium-ios-simulator
cmake --build --preset kernarium-ios-simulator
cmake --preset kernarium-ios-simulator-4k
cmake --build --preset kernarium-ios-simulator-4k

dependencies/build-apple-gpu.sh --platform ios --verify-only
cmake --preset kernarium-ios-device
cmake --build --preset kernarium-ios-device
cmake --preset kernarium-ios-device-4k
cmake --build --preset kernarium-ios-device-4k
Kernarium/scripts/package-ios-frameworks.sh
```

The source transition and all three platform builds are implementation
evidence, not guest capability qualification. The expected shared candidate
remains desktop GL 2.1 and GLES 3.0 until macOS guest identity/function tests,
the identical Simulator campaign, and the connected-iPad correctness,
performance, thermal, direct-presentation, and lifecycle campaign complete.
Official CTS promotion is additionally blocked by the missing authoritative
receipt for the original installed CTS binaries and the pending complete
cross-product matrices. The checked derivative owns an authenticated receipt
and a complete scoped GLES 2/GLES 3 hierarchy identity. No further rootfs
change is authorized to remove the remaining block.

## Archived predecessor evidence

Before the common transition, macOS used CGL/OpenGL 4.1 and a two-slot
CGL-to-IOSurface-to-Metal bridge. Dated KDE/VNC and focused native-window runs
reported guest GL 4.1/GLES 3.0, a nonblank accelerated desktop, and clean
Virgl protocol operation. Those results explain the earlier implementation but
are no longer a runnable lane, A/B baseline, current provider claim, or
qualification input.
