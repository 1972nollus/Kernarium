# VA-API backend over the Virtio video decoder and encoder

`uml_drv_video.so` is a libva backend driver that lets a VA-API client in a
Linux guest decode and encode video through the [Virtio video
device](../../../darwin/device/video/README.md), whose V4L2 stateful mem2mem
nodes the guest kernel publishes as `/dev/videoN` (the Steam image aliases the
decoder to `/dev/video-dec0`). It exists because Valve's x86-64 CEF carries
only the VA-API decode path while the device speaks V4L2, because Debian's
Chromium uses VA-API for hardware video encode as well as decode, and because
no packaged libva backend drives a *stateful* codec.

It is a guest component: it is built natively for the guest it runs in, against
that guest's own libva and libdrm, and is never part of the Apple host build.

## What it offers

| | |
| --- | --- |
| entry point | `__vaDriverInit_1_22` (VA-API 1.22, libva 2.22 - Debian trixie) |
| driver name | `uml`, so `LIBVA_DRIVER_NAME=uml` loads `uml_drv_video.so` |
| decode | `VAProfileVP9Profile0` + `VAEntrypointVLD`, `VA_RT_FORMAT_YUV420`, NV12 |
| encode | H.264 Constrained Baseline, Main and High and HEVC Main, with `VAEntrypointEncSlice` and `VAEntrypointEncSliceLP`, as [Encoding](#encoding) describes |
| video processing | `VAProfileNone` + `VAEntrypointVideoProc`: NV12 or ARGB in, NV12 or ARGB out, cropping and nearest-neighbour scaling |
| surfaces | driver-allocated NV12 and ARGB, or a client buffer imported through `VASurfaceAttribExternalBufferDescriptor` (`DRM_PRIME` and `DRM_PRIME_2`) |
| export | `vaExportSurfaceHandle` with `DRM_PRIME_2`, **one object** holding every plane at its own offset, separate or composed layers |
| images | `vaDeriveImage` (maps the surface itself), `vaCreateImage` + `vaGetImage` for NV12, I420 and ARGB; `vaPutImage` of an image in its surface's own format |

The maximum picture size is whatever the device reports through
`VIDIOC_ENUM_FRAMESIZES`. Everything else in VA-API answers
`VA_STATUS_ERROR_UNIMPLEMENTED`; nothing aborts, because this code runs inside a
browser's GPU process.

H.264 and HEVC are **not** advertised even though the device decodes them. A
stateful decoder needs the original byte stream, and a VA-API client submits
picture parameters plus slice payloads with the parameter sets already consumed;
rebuilding SPS/PPS and VPS from those structures is a separate piece of work.
Advertising them before that exists would make a client pick this driver and
fail. VP9 needs no such rebuild: a VA-API VP9 submission carries the whole
compressed frame.

## How a decode is turned into packets

The driver keeps one V4L2 decode session per VA decode context, and each
`vaBeginPicture` / `vaRenderPicture` / `vaEndPicture` becomes one compressed
packet whose timestamp names the surface it decodes into.

Two VP9 rules need care, because a stateful decoder emits *shown* frames only
while a VA-API client submits every frame:

- **Hidden frames.** A frame with `show_frame == 0` is decoded as a reference
  and never emitted. A client such as Chromium later displays it by submitting
  nothing at all: its VP9 decoder handles `show_existing_frame` itself and hands
  the renderer the surface of the earlier submission. The driver therefore sends
  a hidden frame as a two-frame superframe, the frame followed by the one byte
  that means "show the frame in slot *n*", where *n* is a slot the frame
  refreshes. The decoder then emits the hidden picture immediately and its
  reference state is unchanged. `UML_VA_V4L2_HIDDEN=skip` submits the frame
  alone instead.
- **`show_existing_frame` submissions.** Clients that do submit them are served
  from the surface that last wrote that reference slot, without troubling the
  decoder.

`vaEndPicture` waits for the picture, because the device decodes synchronously
and because Chromium's VA-API wrapper never calls `vaSyncSurface` after a
decode. `vaSyncSurface`, `vaDeriveImage`, `vaGetImage` and a video-processing
blit all wait too.

## Surfaces and what a client can do with them

A surface is one buffer holding every plane, because Chromium's
`ExportVASurfaceAsNativePixmapDmaBuf` asserts that an exported surface has
exactly one object. The backing store is chosen once per process:

| `UML_VA_V4L2_SURFACES` | backing store |
| --- | --- |
| `memory` | a `memfd`: a CPU buffer with a descriptor. Enough for clients that read pixels back; a graphics stack cannot import it. |
| `gbm` | a linear GBM buffer object on a render node, exported as a dma-buf. What a browser needs, and only as good as the guest's GBM. |
| `auto` (default) | `gbm` when a linear NV12 buffer really allocates and exports, otherwise `memory`. |

A descriptor a client hands back is recognised by its identity, so importing a
surface this driver exported shares the same memory instead of copying it.

A planar surface is always **one** object: a browser refuses a descriptor with
more (`CHECK_EQ(num_objects, 1u)`), while the guest's GBM gives each plane of
a planar buffer an object of its own. So an NV12 surface is carried by a
single packed 32-bit buffer whose rows hold the luma and then the chroma, and
the descriptor names both planes inside it at their offsets.

That limit is on what the driver exports. A descriptor a client imports may
name several objects, with each plane in the object its layer's `object_index`
says: a planar buffer from [the gbm module](#the-gbm-module-planar-buffers-for-debians-libgbm)
has an object per plane. Such a surface has no single mapping, so it serves as
an encode input (the encoder reads each plane where it is) and is refused as a
target the driver writes pictures into.

Everything the driver does to the memory of a surface a client imported is
bracketed with `DMA_BUF_IOCTL_SYNC`, on every object of the surface: a read
starts and ends with `READ`, a write with `READ | WRITE`. A write to a GBM
surface the driver allocated itself is bracketed the same way, because a
client that imported the exported descriptor samples that memory on the GPU
and the driver never unmaps its own mapping, which is the only other thing
that would take the picture there. That is what makes a
frame the GPU drew current for the CPU, and a frame the CPU wrote current for
the GPU, on a guest kernel that transfers the buffer around a CPU access; a
descriptor that is no dma-buf (a memory file) answers the request with an error
that is ignored. A mapping handed to the client by `vaDeriveImage` stays the
client's to bracket.

## Decoding straight to packed RGB

A browser converts every decoded frame to RGB before it can draw it, and under
OpenGL it asks this driver to do it through a video-processing blit. The device
can decode straight to packed RGB, and then that blit is a copy of what the
host already produced.

| condition | decoded format |
| --- | --- |
| the device does not enumerate `AR24` on its CAPTURE queue | NV12, always |
| `UML_VA_V4L2_RGB=off` | NV12 |
| `auto` (default) | packed RGB |

Converting NV12 in the guest costs far more than copying the larger packed
frame; the measurement is recorded in
[`optimization-history.md`](../../../docs/optimization-history.md).

The decision is made once, at initialisation, from what the device offers
rather than from what a client has done so far, so the first decode session of
a boot gets the same answer as every later one. The packed picture lives in
an RGB shadow of the NV12 surface, converted back when a client asks for NV12
through `vaDeriveImage`, `vaGetImage` or `vaExportSurfaceHandle`, and after
every later decode into a surface whose memory such a client holds, because
that client reads the memory without asking again. That round trip is not bit
exact, so `UML_VA_V4L2_RGB=off` stays for a client that wants the device's own
planes.

The format is negotiated before the first packet, not after the source-change
event: the device keeps the choice across that event, and changing it once the
host has a decode session asks it to switch layout underneath one.

## Lending the decoder the client's buffers

A V4L2 capture queue can be given buffers the caller owns
(`V4L2_MEMORY_DMABUF`) instead of allocating its own. Then the device writes
each decoded picture straight into a surface this driver already made for the
client, and the guest never copies the picture at all.

| `UML_VA_V4L2_CAPTURE` | the decoded pictures' memory |
| --- | --- |
| `device` | the decoder's own buffers, mapped and copied out. Always available |
| `auto` (default) | this driver's surfaces wherever they are dma-bufs at all, which is to say GBM, falling back to `device` for any queue that refuses one |

Whether a queue takes a buffer is settled by offering the real queue a real
surface as it is built, not by asking whether it supports the memory type: a
queue can accept `V4L2_MEMORY_DMABUF` and still refuse a host-visible blob,
which has no pages of its own to map. A queue that refuses gets the device's
own buffers, with the reason recorded once: `into device buffers (lend the
decoder a buffer: Invalid argument)`.

A surface still being read is never decoded over, a surface whose memory a
client already holds keeps that memory and has the picture copied into it, and
a source change takes back every lent buffer before the new queue is built.
The comments on `provideCapture` and `fillFromLent` in `driver.hpp` and on
`release` in `v4l2_decoder.hpp` hold the mechanics.

Only a capture format of one plane can be lent today. `AR24` is one plane and
is lent whole, which is the default path. `NV12M` is two planes and the device
offers nothing single-planar for it, so the one object that carries both
planes is refused and an NV12 decode keeps the decoder's own buffers. When the
capture queue learns to take one object carrying both planes, the probe finds
it with no other change.

## Encoding

An encode profile is listed only when the encoder node's menus offer that
codec and profile, and a rate-control mode only when the node offers it, so
the driver never advertises what the host did not verify. Both entry points are
the same encoder.

| attribute | value |
| --- | --- |
| `VAConfigAttribRTFormat` | `VA_RT_FORMAT_YUV420` |
| `VAConfigAttribRateControl` | the node's modes among `VA_RC_CBR`, `VA_RC_VBR`, `VA_RC_CQP` (frame rate control off, fixed quantisers) and `VA_RC_ICQ` (constant quality) |
| `VAConfigAttribEncPackedHeaders` | none: the host writes the parameter sets |
| `VAConfigAttribEncMaxRefFrames` | 1: every picture after the first predicts from the one before it |
| `VAConfigAttribEncMaxSlices` | 1 |
| `VAConfigAttribMaxPictureWidth` / `Height` | the codec's limits from `VIDIOC_ENUM_FRAMESIZES` |

A VA context takes one descriptor from the pool and runs one V4L2 encode
session on it. The session starts with the first picture, because the controls
that configure a stream (profile, level, rate-control mode, key frame interval,
entropy coder) cannot change while it runs and VA-API names them only in the
first picture's buffers. After that, what a client changes between pictures -
the bit rate, the peak rate, the quantiser bounds, the constant quantiser, the
quality, the frame rate - is applied as dynamic controls before the next frame
goes in. A stream cannot change its size; the client creates a new context.

| VA-API buffer | becomes |
| --- | --- |
| sequence parameters | the stream size (H.264 cropping gives a smaller size than the coded one; HEVC carries none, so a surface up to 15 rows or columns short of the coded size is taken as the picture and the host crops the alignment), level, tier, key frame interval, and the frame rate from the VUI timing |
| picture parameters | a forced key frame for an IDR picture, CABAC and the 8x8 transform, the initial quantiser, the coded buffer that receives the frame |
| slice parameters | the quantiser delta that, with the initial quantiser, gives the constant quantiser |
| `RateControl` | bit rate and peak (variable rate: the peak is the client's rate and the target is its percentage), quantiser bounds, ICQ quality (libva's factor, 1 best, maps linearly onto V4L2's 1..100) |
| `FrameRate`, `HRD` | the frame rate, and the buffer size |
| packed headers | ignored; the host's own are used |

Pictures go to the node as NV12 in whichever of `NV12M` and `NV12` it takes,
from NV12 surfaces directly or from ARGB ones after one conversion. The
encoder only reads a surface, so a descriptor a client imported read-only (the
state of one a graphics stack exports) is mapped read-only and serves, and each
frame is read inside one `DMA_BUF_IOCTL_SYNC` bracket. Every key
frame carries its parameter sets in the same buffer. `vaEndPicture` returns
once the frame is queued; `vaSyncSurface` on the input and `vaMapBuffer` on the
coded buffer wait up to four seconds for the frame, which comes back with the
timestamp of the picture it encodes. A frame larger than its coded buffer is
cut to it and flagged `VA_CODED_BUF_STATUS_SLICE_OVERFLOW_MASK`; a frame the
host failed is `VA_STATUS_ERROR_ENCODING_ERROR` on its surface alone.

Not offered, and refused or absent rather than approximated: B-frames
(`ip_period` above 1 is `VA_STATUS_ERROR_INVALID_PARAMETER`), more than one
slice, temporal layers, regions of interest, HEVC Main 10 and P010, and
picture encoding. The first picture of a context must carry sequence
parameters. Input handed over as a dma-buf is copied, not shared.

## Device access in a sandboxed process

A browser's GPU process initialises VA-API before it engages its sandbox, and
a sandbox that is engaged cannot open a device node, so a session created
afterwards would find nothing to talk to. The driver therefore opens a pool of
V4L2 descriptors at the first initialisation in a process, and a session takes
one from the pool and gives it back when it ends, with both of its queues
stopped and released so that the next session starts from the initialisation
sequence. The pool belongs to the process rather than to a `vaInitialize`:
libva keeps the driver loaded, and a terminate followed by another initialise
finds the same stock. Surfaces use the DRM descriptor the client passed to
`vaGetDisplayDRM`, so allocating them opens nothing either.

| `UML_VA_V4L2_POOL` | descriptors opened ahead |
| --- | --- |
| unset | four for the encoder and four for the decoder |
| `encoders,decoders` | that many of each, at most sixteen; `0,0` opens nothing ahead |

A process that is not sandboxed may take more than the pool holds: a session
opens another descriptor when the stock is empty and closes it again when it
ends. In a sandbox with the stock empty, `vaCreateContext` answers
`VA_STATUS_ERROR_MAX_NUM_EXCEEDED` and the client falls back to software.

The pool is required whenever Chromium's GPU sandbox engages. In today's guests
it does not engage: Chromium's `SandboxLinux::InitializeSandbox()` declines to
apply seccomp-bpf to a process that already has more than one thread, that
refusal is not fatal for the GPU process, and the Mesa driver stack has started
threads by the time the GPU process reaches it. `--gpu-sandbox-failures-fatal`
turns the refusal into a crash, which is the evidence. Renderers and the other
sandboxed process types are sandboxed normally, so the guest kernel is not the
reason.

## Environment

| variable | meaning |
| --- | --- |
| `LIBVA_DRIVER_NAME=uml`, `LIBVA_DRIVERS_PATH` | how libva finds this driver |
| `UML_VA_V4L2_DEVICE` | the decoder node, instead of `/dev/video-dec0` or a scan of `/dev/video*` |
| `UML_VA_V4L2_ENCODER_DEVICE` | the encoder node, instead of a scan of `/dev/video*` for a stateful encoder |
| `UML_VA_V4L2_POOL` | how many descriptors to open ahead for each role, as above |
| `UML_VA_V4L2_SURFACES` | the backing store, as above |
| `UML_VA_V4L2_HIDDEN` | `show` (default) or `skip`, as above |
| `UML_VA_V4L2_RGB` | `auto` (default) or `off`, as above |
| `UML_VA_V4L2_CAPTURE` | `auto` (default) or `device`: who owns the decoded pictures' memory, as above |
| `UML_VA_DRM_DEVICE` | the render node for GBM surfaces, instead of `/dev/dri/renderD128` |
| `UML_VA_V4L2_LOG` | a file to append diagnostics to; unset, the driver writes nothing |
| `UML_VA_V4L2_TRACE` | `1` also logs every entry point a client calls, before it runs and with its answer after, so a client that dies inside the driver leaves the call behind, and a mean of each encode input's samples, to tell pictures from empty memory |
| `UML_VA_V4L2_REPORT` | how many captured pictures to report to the log, exactly as the capture queue delivered them (default one) |
| `UML_VA_V4L2_DUMP` | a file to write one captured picture to, before any conversion or blit |
| `UML_VA_V4L2_DUMP_FRAME` | which captured picture that is, counted from the first (default zero) |

The DRM file descriptor a client passes to `vaGetDisplayDRM` is ignored: this
driver talks to the video decoder whatever device the display names.

## Building and testing

```sh
cmake --preset guest && cmake --build --preset guest
./build/uml_va_v4l2_tests          # VP9 headers, superframes, pixel paths, the pool,
                                   # and encode against an encoder node held in memory
```

`build/va_decode_probe` is the guest-side qualification client. It drives libva
the way Chromium's `VaapiVideoDecoder` does - one submission per frame of a
superframe, the whole compressed frame as the slice data - and writes the shown
frames as planar I420, so its output can be compared byte for byte with a
reference decode of the same stream:

```sh
LIBVA_DRIVER_NAME=uml LIBVA_DRIVERS_PATH=$PWD/build \
  ./build/va_decode_probe movie.ivf --out - | md5sum
```

`--vpp` additionally converts every decoded frame to ARGB through a
`VAProfileNone` video-processing context, and `--export` exports a decoded
surface as `DRM_PRIME_2`, re-imports the descriptor and compares the pixels.
`--export-early` exports every surface before anything is decoded into it and
reads each frame from that exported memory before asking the driver for the
frame any other way, which is what a browser that hands its surfaces to a
compositor does. Those are the paths a browser uses beyond decoding.

`build/va_encode_probe` is the encode counterpart. It submits pictures the way
Chromium's `VaapiVideoEncodeAccelerator` does - surfaces and coded buffers at
the aligned size, every parameter buffer with every picture, several pictures in
flight - and writes an Annex B stream. It checks what needs no decoder: every
frame came back, key frames fall where they were asked for and carry their
parameter sets, and the stream's own sequence parameter set declares the size
asked for, cropping included. `--list` prints the encode profiles and their
attributes; `--sessions` and `--contexts` repeat and interleave sessions, which
exercises descriptor reuse.

```sh
LIBVA_DRIVER_NAME=uml LIBVA_DRIVERS_PATH=$PWD/build \
  ./build/va_encode_probe --codec h264 --size 1920x1080 --rate cbr --out out.h264
```

`tests/probe_selftest.sh build` runs the probe through libva's own loader
against `fake_drv_video`, the same driver over the in-memory encoder node, with
`drm_node_shim.so` preloaded so libva-drm takes a descriptor that is no DRM
node. That covers the whole path except the Virtio video device, on a host that
has none. `tests/ffmpeg_selftest.sh build` does the same with FFmpeg's
`h264_vaapi` and `hevc_vaapi` as a second client, when `ffmpeg` is installed;
the fake's stream is not decodable, so it checks that FFmpeg's submission is
accepted and every frame returns. The decode and encode probes are Linux guest programs; the unit test
and the self-test also run on any Linux build host. Any other VA-API client
works too, because the driver implements the public interface and nothing
else.

## The gbm module: planar buffers for Debian's libgbm

Chromium allocates the frames it hands to a hardware encoder through gbm, and
Debian's libgbm cannot allocate any planar format: it looks every FourCC up in
a table of colour formats, so NV12, P010 and the three-plane layouts fail with
`EINVAL` before they reach a driver. `virtio_gpu_gbm.so`
([`gbm/`](gbm/)) is the backend module that fixes this for the virtio-gpu
device. libgbm loads a backend module named after the DRM driver of the node it
is given, so installing the file as `/usr/lib/<triplet>/gbm/virtio_gpu_gbm.so`
is the whole configuration; `GBM_BACKEND=dri` turns it off for a process.

- **It adds planar buffers and nothing else.** The module loads Mesa's own DRI
  backend (`dri_gbm.so`, beside it) and returns the device that backend makes,
  so the device is still the `drm` device Mesa's EGL platform expects. It
  replaces only the entries of that device's function table that a planar
  buffer needs; every other call, and every buffer of a colour format, is
  Mesa's own. A compositor's scanout and rendering buffers, gbm surfaces and
  EGL windows are unchanged. When it cannot load Mesa's backend, or Mesa's
  backend already allocates NV12, it makes no device or leaves the device as
  Mesa made it, and libgbm falls back to Mesa.
- **A planar buffer is one buffer object per plane**, each in the format the
  plane really has (`R8` and `GR88` for NV12, `R16` and `GR1616` for P010, three
  `R8` for YU12 and YV12), so each plane is its own resource of the guest GPU
  with its own dma-buf, at offset 0, linear. That is how Mesa describes a
  planar image to the GPU, and what Chromium imports plane by plane. The
  buffer reports its plane count, strides, one handle and descriptor per
  plane (`gbm_bo_get_fd_for_plane`) and its first plane's as the image's.
  `gbm_bo_map` and `gbm_bo_write` of a whole planar image fail with `ENOSYS`,
  because the call cannot name a plane: a client maps each plane's dma-buf.
  A planar buffer must reach EGL through its plane descriptors, not as an
  `EGL_NATIVE_PIXMAP_KHR` gbm buffer.
- **Only 64-bit.** The module is built for the guest's native architecture and
  not for the 32-bit libgbm of the x86-64 image, whose clients (Steam and Wine)
  allocate no planar buffers.
- **It depends on libgbm's backend interface, version 1**, which is frozen: a
  later version only appends. [`gbm/abi.hpp`](gbm/abi.hpp) declares that layout
  because Debian's libgbm-dev does not install the header, and a device made at
  another version is left as Mesa made it.

What a client may do with the plane descriptors depends on the guest kernel: a
descriptor Mesa exports is read-only unless the kernel exports virtio-gpu
buffers read-write, and what the GPU wrote reaches the guest's pages only where
the kernel transfers it around a dma-buf CPU access
(`DMA_BUF_IOCTL_SYNC`). The driver and Chromium bracket their accesses that way.

`tests/gbm_planar_tests.cpp` runs the adapter and the module against a fake
Mesa backend, inside `uml_va_v4l2_tests`. `gbm_planar_probe` is the guest-side
check against the real libgbm and libEGL: planar allocation and export, a gbm
surface under an EGL window surface as a compositor uses it, and the GPU
rendering into a plane and the CPU reading and writing it back. `--carrier`
repeats that for the layout the driver exports a decoded surface in (one object
with the chroma plane at an offset), and `--carrier-sample` samples NV12
pictures as a compositor's external texture, against a colour buffer as the
control.

## In the Debian desktop images

The starter Plasma image, the KDE image and the x86-64 desktop image carry
Debian's Chromium, the driver and [`chromium.d/kernarium-video`](chromium.d/kernarium-video),
which `/usr/bin/chromium` sources at start. The file names the driver
(`LIBVA_DRIVER_NAME=uml`, because libva would otherwise map the virtio-gpu node
to Mesa's driver, which has nothing to offer), passes
`--render-node-override=/dev/dri/renderD128` (the guest's render node is not a
PCI device, so Chromium finds none itself and leaves VA-API off). No udev rule,
session environment or other package is involved.

With it Chromium's VA-API decode goes through this driver: VP9 is hardware, and
H.264 and HEVC decode stay in software because the driver does not offer them.
`--disable-accelerated-video-decode` would also skip the initialisation that
encode needs.

Chromium's own hardware encode (WebCodecs `VideoEncoder`, WebRTC) is on. It is
H.264 only, since Chromium has no HEVC encode on Linux, and the file adds
`AcceleratedVideoEncoder` and `ChromeOSHWVBREncoding` (arm64 needs the latter
for variable-bitrate profiles). Three parts fit together to make it work:

- **The frames.** Chromium encodes frames that live in GPU memory, which it
  allocates through gbm as NV12 buffers. [The gbm module](#the-gbm-module-planar-buffers-for-debians-libgbm)
  allocates them.
- **The import.** Chromium hands a planar buffer to a driver as one descriptor
  per plane (`VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2`) only when it recognises
  the driver by its vendor string, and as a single descriptor with every plane
  at an offset in it for any other. A buffer of the module has a resource, and
  so a descriptor, per plane, and the single descriptor names the first plane
  only. So the driver's vendor string begins with `Mesa Gallium driver`, the
  string of the class of driver that takes the per-plane import, and continues
  with the driver's own name. Chromium treats that class as thread-safe and
  stops serialising its calls with a lock of its own, so the driver serialises
  itself: every entry point that reads or changes driver state takes the
  driver's lock, and the others are the lifecycle calls, answers from
  constants, or forwards to an entry point that locks. Keep it so when adding
  an entry point.
- **The pixels.** Content the guest GPU wrote (canvas, video and page content
  composited on the GPU) is current for the CPU only on a guest kernel that
  exports virtio-gpu dma-bufs read-write and transfers the resource around a
  dma-buf CPU access (`drm/virtio: export a dma-buf read-write` and
  `drm/virtio: transfer a 3D resource around CPU access through its dma-buf`).
  Without them a GPU-painted frame encodes black, while frames built in
  ordinary memory (a `VideoFrame` made from a buffer) are unaffected.

[`tools/rootfs/producer.sh`](../../rootfs/producer.sh) builds the driver inside
an image's own root (`producer_install_uml_va_driver`), against the libva and
libdrm the image ships, runs the unit test and the probe self-test there,
installs both files with `cmake --install --prefix /usr` and removes the build
tools; the starter and x86-64 producers call it, and the
[desktop derivation](../../rootfs/desktop/README.md) installs a driver built the
same way into the KDE image.

## In the Steam x86-64 image

[`tools/rootfs/steam-x86_64`](../../rootfs/steam-x86_64/README.md) compiles
this driver in its builder root, runs the unit test there, and installs the
module as `/usr/lib/x86_64-linux-gnu/dri/uml_drv_video.so`. The gaming session
exports `LIBVA_DRIVER_NAME=uml` and `KERNARIUM_CEF_VAAPI=1` when the module and
`/dev/video-dec0` are both present, and the session supervisor then
starts the client with Valve's `-cef-enable-vaapi`, the option that gives its
web helper the `VaapiVideoDecodeLinuxGL` and `VaapiIgnoreDriverChecks` Chromium
features. `VaapiIgnoreDriverChecks` matters
here: without it Chromium accepts only the driver vendor strings it knows.
