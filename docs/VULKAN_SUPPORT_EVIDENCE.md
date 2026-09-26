# Vulkan support evidence

This file records the measured acceptance result for
[`MAX_VULKAN_SUPPORT_PLAN.md`](MAX_VULKAN_SUPPORT_PLAN.md). The result applies
only to this project's guest Mesa Venus, virglrenderer, Mesa KosmicKrisp, and
Metal stack on the tested macOS host. It is not a Khronos conformance
submission, certification, or Vulkan trademark claim.

## Qualified stack

The final campaign ran on 2026-09-04 on an Apple M1 Ultra host running macOS
26. It used the canonical `macos-integration-venus` emulator and the verified
`apple-gpu-vulkan-experiment/macos-arm64` dependency prefix. The active route
was:

```text
Linux application -> Vulkan Loader -> Mesa Venus -> Virtio-GPU
    -> virglrenderer Venus -> Mesa KosmicKrisp -> Metal
```

The native provider probe created the Apple M1 Ultra device successfully and
reported Vulkan 1.4.354. The renderer probe reported Venus capset 4 version 0,
a 160-byte capset, context flag 4, and successful context creation. The
framework exposes `vkGetInstanceProcAddr` directly; neither probe nor the
runtime closure uses an ambient Vulkan loader or another Vulkan provider.

The implementation intentionally reports unsupported behavior as
unsupported. In particular, shader float16, float16 FMA, float16
signed-zero/Inf/NaN preservation, and float16 round-to-nearest-even controls
are disabled because concurrent NaN-heavy native-half graphics workloads can
time out the tested Apple GPU. Integer-8 support is independent and remains
enabled. MoltenVK is not built, linked, loaded, or packaged, and the iOS
device and Simulator products contain no Vulkan path.

## Reproducible identities

The campaign used these exact inputs:

| Input | Identity |
| --- | --- |
| Host Mesa source | 26.2.1 upstream commit `da14d65e4499e66468094be52bff9ea0915a695e`; prepared tree `c517450ce3b1bdcf077f010c34f4464e8da230af` |
| Host Mesa topic | tip `ceb2b54`; ordered series SHA-256 `ae38570d54ff886b919a9a173ea80e7327113d038e4c02bd5dda85d2a6156c9c` |
| virglrenderer source | upstream commit `65cc14eb896f121ffc5130ce04815a923a03c41d`; prepared tree `c960e47662ffd48dac59464ba695d866b4ac809e` |
| virglrenderer topic | tip `8f2964c`; ordered series SHA-256 `633afd209e82a9eecfe5539eab88b6c53adec6134a631b96696b60a48b12681f` |
| Guest Mesa topic | tip `136bf2f`; patched tree `58ebb0b33a5c2bd269f09b2ba4eb95a110723345`; ordered series SHA-256 `108ab47fe77174c109e2231335f9b5971e5229879ab0be8356c314d1173673cd` |
| KosmicKrisp framework binary | SHA-256 `632c8ae21579f893f24c24673567c5e386db5b44d10feccd33b985bb0929c7e7` |
| Guest rootfs | 8,589,934,592-byte `rootfs-vulkan-venus.ext4`; SHA-256 `ac992bd6e6af757d6af0f703be3a78a5e795b9f7fd94532fd9290f3fb122798e` |
| Rootfs receipt | SHA-256 `18d81ed6e1d6c5c0585f9c47fac87edeeea4b82be1e0f879f3d77e8562eb4782` |
| Vulkan CTS | release 1.4.6.2; `deqp-vk` SHA-256 `2d037a6ac00754cc92a8136c30cd3470ad8bf8791603307fe38f546464e0e5b3` |
| Guest Vulkan loader | SHA-256 `dfe2055524d52d5b095370720f7afb34d0b388708653173e492a37ba7b48d2a0` |
| Guest Virtio ICD | SHA-256 `a33216fc3e60cb3c4ca588b0fab0f25dfea70406f3e544cb655a8a5774c3a8e9` |

The checked receipt binds the source trees, generated patch identities,
installed files, loader and CTS builds, filesystem identity, image size, and
image digest. The base image was attached read-only below a disposable
overlay for every fraction.

## Complete Vulkan CTS campaign

The receipt-selected `current-must-pass` list was split into all 16 released
fractions. Four rolling workers were used; each guest received exactly 4
vCPUs and 4 GiB RAM. A worker immediately took the next pending fraction when
it became free. Every final attempt exited successfully, transferred a
complete QPA and renderer record, selected only the Virtio ICD, shut down,
and was reaped.

| Fraction | Total | Pass | NotSupported | QualityWarning | CompatibilityWarning |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 0/16 | 200,198 | 32,891 | 167,307 | 0 | 0 |
| 1/16 | 203,743 | 35,959 | 167,784 | 0 | 0 |
| 2/16 | 208,933 | 35,012 | 173,921 | 0 | 0 |
| 3/16 | 217,331 | 37,961 | 179,370 | 0 | 0 |
| 4/16 | 217,078 | 35,168 | 181,906 | 4 | 0 |
| 5/16 | 206,779 | 31,593 | 175,186 | 0 | 0 |
| 6/16 | 207,494 | 30,684 | 176,810 | 0 | 0 |
| 7/16 | 193,150 | 32,189 | 160,961 | 0 | 0 |
| 8/16 | 202,871 | 32,669 | 170,202 | 0 | 0 |
| 9/16 | 207,376 | 32,201 | 175,175 | 0 | 0 |
| 10/16 | 199,025 | 32,435 | 166,588 | 2 | 0 |
| 11/16 | 192,614 | 32,863 | 159,751 | 0 | 0 |
| 12/16 | 203,376 | 31,598 | 171,778 | 0 | 0 |
| 13/16 | 202,237 | 31,927 | 170,309 | 1 | 0 |
| 14/16 | 192,721 | 31,149 | 161,571 | 1 | 0 |
| 15/16 | 192,941 | 31,725 | 161,216 | 0 | 0 |
| **Aggregate** | **3,247,867** | **528,024** | **2,719,835** | **8** | **0** |

The aggregate contains zero `Fail`, `InternalError`, `Crash`, `Timeout`, or
other disallowed statuses. `NotSupported` is an accepted CTS status for tests
whose optional prerequisite is not advertised; it does not mean that a
mandatory advertised feature failed. Four quality warnings are in fraction
4, two are in fraction 10, and one each is in fractions 13 and 14.
The status counts sum exactly to the selected-case count in every fraction
and in the aggregate.

The focused
`dEQP-VK.api.driver_properties.conformance_version` regression passes. The
guest now preserves the renderer's `1.4.3.2` value instead of replacing it
with the unrecognized Venus constant `1.4.0.0`; the complete fraction 3 also
passes with 37,961 `Pass` results and no disallowed status.

## Lifecycle result

The four-worker pool never exceeded four live emulator processes. The final
process audit found no `uml-darwin` process after all 16 fractions completed.
The runner's 600-second no-console-progress guard also reaped non-progressing
startup attempts before their fractions were retried, so a stalled guest did
not hold a slot or survive the campaign.

## Validation boundary

The dependency prefix verifier, runtime-closure verifier, native capability
probe, Venus backend probe, rootfs input verifier, receipt verifier, focused
conformance-version case, and complete 16-fraction campaign all pass for the
identities above. The final post-campaign host workflows pass as follows:

- `check-unit`: 576/576 unit and 64/64 tooling tests;
- `check-unit-4k`: 577/577 unit and 64/64 tooling tests;
- `check-gpu-venus` and `check-gpu-venus-4k`: 8/8 unit tests and the 1/1
  guest VNC canary at each page profile;
- `check-gpu-venus-asan-ubsan`: 8/8 unit tests and the 1/1 sanitized guest
  VNC canary; and
- `check-integration-venus` and `check-integration-venus-4k`: 67/67 tooling
  and 5/5 guest integration tests at each page profile.

The architecture and test-manifest audits also pass.

This result qualifies the project's experimental macOS guest Vulkan route. It
does not qualify iOS, replace formal Khronos process requirements, or imply
support for capabilities that KosmicKrisp truthfully leaves unadvertised.

## Focused dynamic raster qualification

The released Vulkan CTS 1.4.6.2 dynamic raster selection completed all 1,397
cases under the 16 KiB guest profile: 596 `Pass`, 801 `NotSupported`, and no
failures or warnings. Polygon modes, depth clipping, provoking vertices, line
coverage and stipple, and depth-bias regressions used one provider across three
4-vCPU/4-GiB guests. Every selected leaf appeared exactly once, all artifacts
verified, and every guest shut down and was reaped. The native planner,
materialization, and raster regressions supplement this focused qualification;
it does not replace the final complete Vulkan and Zink/OpenGL campaigns.

## Mesa 26.2.3 guest epoch

On 2026-09-26 the host and guest Mesa moved from 26.2.1 to the `mesa-26.2.3`
tag (commit `31e9a6b2e95e30d84bf3177d1f497d063e59b6b2`, tree
`7f4340843f646b83e354e94efb4f2205af97a7fe`) as guest epoch `source-30`. Every
host and guest topic replayed without adaptation. The complete campaign above
remains the 26.2.1 result; source-30 is qualified by the targeted checks
below, each run beside a same-day source-29 control on the same host.

| Input | Identity |
| --- | --- |
| Host Mesa series | final tree `959bd74e296bcb6f4d3c1060f25bb22cc13cb88d` |
| Guest Mesa series | final tree `7059336f8075f678b4556dfbed555a3254cd9ea0` |
| KosmicKrisp framework binary | SHA-256 `90f7d65eb2226ed7678d54591ed8feab06625d08dc620aa1f608c523912b3a46` |
| Venus rootfs | SHA-256 `1bed9e0d01f903eb16da1a4aa826043849ced40747ece894b8ea9afdac926535`; receipt `f06a571e24959856ed108411256328edee2dbecbdecb17af97b913caa8398122` |
| Zink/OpenGL rootfs | SHA-256 `e919fd40b5386f0b84067ef5aedec547e3ed5eca7f6751933b1c4e5559a4144d`; receipt `b5821fecbe2a004bff82127d73539c88cae79265cc8568f4ce759d9fe9ee5913` |

The KosmicKrisp Meson suites pass (67 kosmickrisp tests and the Vulkan
utility test), as do the guest Mesa unit tests built by the sealer. The
guest reports driver version 26.2.3 through Venus and through Zink.

Targeted CTS 1.4.6.2 results, each identical between source-29 and
source-30:

| Selection | Cases run | Pass | NotSupported | Other |
| --- | ---: | ---: | ---: | --- |
| Vulkan smoke | 21 | 19 | 2 | none |
| `synchronization`, `synchronization2`, `device_group` | 91 | 58 | 33 | none |
| `wsi.wayland` | 76 of 88 | 66 | 10 | 2 aborted, 10 not run |
| `KHR-GL46` through Zink | 87 | 84 | 2 | 1 `ResourceError` |

Two `wsi.wayland` cases that create 10-bit swapchains end the CTS process
with `SIGABRT`: `swapchain.create.image_array_layers` and
`swapchain.simulate_oom.image_extent`. In both, the host device rejects a
guest `RESOURCE_CREATE_BLOB` with `INVALID_PARAMETER`. The other 10
`simulate_oom` cases were not run. `texture_buffer.texture_buffer_max_size`
fails to map its buffer in a 4 GiB guest, as described in
[`graphics.md`](graphics.md).
