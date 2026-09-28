# VA-API backend over the Virtio video decoder

`uml_drv_video.so` is a libva backend driver that lets a VA-API client in a
Linux guest decode video through the [Virtio video
decoder](../../../darwin/device/video/README.md), whose V4L2 stateful
mem2mem node the guest kernel publishes as `/dev/video0` and the Steam
appliances alias to `/dev/video-dec0`. It exists because Valve's x86-64 CEF
carries only the VA-API decode path, while the device speaks V4L2, and because
no packaged libva backend drives a *stateful* decoder.

It is a guest component: it is built natively for the guest it runs in, against
that guest's own libva and libdrm, and is never part of the Apple host build.

## What it offers

| | |
| --- | --- |
| entry point | `__vaDriverInit_1_22` (VA-API 1.22, libva 2.22 - Debian trixie) |
| driver name | `uml`, so `LIBVA_DRIVER_NAME=uml` loads `uml_drv_video.so` |
| decode | `VAProfileVP9Profile0` + `VAEntrypointVLD`, `VA_RT_FORMAT_YUV420`, NV12 |
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

The driver keeps one V4L2 decode session per VA decode context. Each
`vaBeginPicture` / `vaRenderPicture` / `vaEndPicture` becomes one compressed
packet on the OUTPUT queue, tagged with a unique timestamp; both queues use
`V4L2_BUF_FLAG_TIMESTAMP_COPY`, so the CAPTURE buffer that comes back names the
submission that produced it. The decoded NV12 picture is copied into the target
surface, and the CAPTURE buffer is returned to the decoder.

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

## Decoding straight to packed RGB

A browser converts every decoded frame to RGB before it can draw it, and under
OpenGL it asks this driver to do it through a video-processing blit. The device
can decode straight to packed RGB, and then that blit is a copy of what the
host already produced.

| condition | decoded format |
| --- | --- |
| the device does not enumerate `AR24` on its CAPTURE queue | NV12, always |
| `UML_VA_V4L2_RGB=off` | NV12 |
| `auto` (default) or `force` | packed RGB |

It is the single largest thing in the guest-side video path: converting NV12
in the guest is a scalar pass over every pixel, while copying packed `AR24` is
a row `memcpy`, so carrying the larger packed frame out of the device costs a
small fraction of the conversion it saves. The cost is the arithmetic, not the
memory: a large `memcpy` in this guest runs at the same rate whether it touches
anonymous memory, a `gbm_bo_map`ped buffer or a V4L2 `MMAP` capture plane. The
measurement is recorded in
[`optimization-history.md`](../../../docs/optimization-history.md).

The decision is made once, at initialisation, from what the device offers
rather than from what a client has done so far, so the first decode session of
a boot gets the same answer as every later one. A client that reads NV12 back
rather than blitting pays the conversion in the other direction, and the round
trip is not bit exact, so `UML_VA_V4L2_RGB=off` stays for a client that wants
the device's own planes.

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

Whether a queue takes a buffer is settled by offering it one, not by asking
whether it supports the memory type: a queue accepts `V4L2_MEMORY_DMABUF` and
then refuses the buffer, because it maps an imported one through its own
memory operations and a buffer with no pages of its own - a host-visible blob,
which is what a virtualised graphics stack hands out - has nothing there to
map. Nor can a synthetic queue answer for the real one, since a queue
negotiates its own geometry. So the real queue is offered a real surface as
it is built, and one that refuses gets the device's own buffers instead, with
the reason recorded once: `into device buffers (lend the decoder a buffer:
Invalid argument)`.

The driver holds one buffer per capture slot and a pool of the ones the
decoder has given back. A surface that shows a picture takes its buffer over;
the slot is refilled from the pool before it is queued again; and a buffer
returns to the pool only when nothing refers to it any more, so a surface
still being read is never decoded over. A surface whose own memory a client
already holds - through `vaExportSurfaceHandle` or `vaDeriveImage` - keeps
that memory, and the picture is copied into it instead, because a descriptor
a client cached must not start naming a different buffer.

A source change gives the whole queue back: `STREAMOFF`, `REQBUFS(0)` naming
the memory type the queue really had, then the new format and a queue built
from scratch. The first slot of a new queue takes back everything the old one
held, so nothing is leaked and nothing is lent twice.

Only a capture format of one plane can be lent today. `AR24` is one plane and
is lent whole, which is the default path. `NV12M` is two planes and the device
offers nothing single-planar for it, so the one object that carries both
planes is refused (`lend the decoder a buffer: Invalid argument`) and an NV12
decode keeps the decoder's own buffers. When the capture queue learns to take
one object carrying both planes, the probe finds it and that copy goes away
with no other change.

What it is worth is one copy a frame and no more: copying a frame out of a
capture buffer is a row `memcpy` that is small beside the frame's decoding. It
is here because the copy is real and avoidable, not because it is fast.

The surface's own format stays NV12: the packed picture lives in a shadow
buffer, and `vaDeriveImage`, `vaGetImage` and `vaExportSurfaceHandle` convert
it back the moment a client really wants NV12. A client that only ever
converts never pays for that.

## Environment

| variable | meaning |
| --- | --- |
| `LIBVA_DRIVER_NAME=uml`, `LIBVA_DRIVERS_PATH` | how libva finds this driver |
| `UML_VA_V4L2_DEVICE` | the decoder node, instead of `/dev/video-dec0` or a scan of `/dev/video*` |
| `UML_VA_V4L2_SURFACES` | the backing store, as above |
| `UML_VA_V4L2_HIDDEN` | `show` (default) or `skip`, as above |
| `UML_VA_V4L2_RGB` | `auto` (default), `off` or `force`, as above |
| `UML_VA_V4L2_CAPTURE` | `auto` (default) or `device`: who owns the decoded pictures' memory, as above |
| `UML_VA_DRM_DEVICE` | the render node for GBM surfaces, instead of `/dev/dri/renderD128` |
| `UML_VA_V4L2_LOG` | a file to append diagnostics to; unset, the driver writes nothing |
| `UML_VA_V4L2_REPORT` | how many captured pictures to report to the log, exactly as the capture queue delivered them (default one) |
| `UML_VA_V4L2_DUMP` | a file to write one captured picture to, before any conversion or blit |
| `UML_VA_V4L2_DUMP_FRAME` | which captured picture that is, counted from the first (default zero) |

The DRM file descriptor a client passes to `vaGetDisplayDRM` is ignored: this
driver talks to the video decoder whatever device the display names.

## Building and testing

```sh
cmake --preset guest && cmake --build --preset guest
./build/uml_va_v4l2_tests          # VP9 headers, superframes, pixel paths
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

Both binaries are Linux guest programs; the unit test also runs on any Linux
build host.

Any VA-API client works, because the driver implements the public interface and
nothing else. An FFmpeg configured for this one job needs no codec closure:

```sh
./configure --disable-everything --enable-vaapi --enable-decoder=vp9 \
  --enable-hwaccel=vp9_vaapi --enable-parser=vp9 --enable-demuxer=ivf \
  --enable-filter=hwdownload --enable-filter=format --enable-filter=scale \
  --enable-encoder=rawvideo --enable-muxer=rawvideo --enable-ffmpeg
ffmpeg -hwaccel vaapi -hwaccel_output_format vaapi -i movie.ivf \
  -vf hwdownload,format=nv12 -f rawvideo -pix_fmt yuv420p - | md5sum
```

`--vpp` additionally converts every decoded frame to ARGB through a
`VAProfileNone` video-processing context, and `--export` exports a decoded
surface as `DRM_PRIME_2`, re-imports the descriptor and compares the pixels.
Those are the two paths a browser uses beyond decoding.

## In the Steam x86-64 image

[`tools/rootfs/steam-x86_64`](../../rootfs/steam-x86_64/README.md) compiles
this driver in its builder root, runs the unit test there, and installs the
module as `/usr/lib/x86_64-linux-gnu/dri/uml_drv_video.so`. The gaming session
exports `LIBVA_DRIVER_NAME=uml` and `KERNARIUM_CEF_VAAPI=1` when the module and
`/dev/video-dec0` are both present, and the shared session supervisor then
starts the client with Valve's `-cef-enable-vaapi`, the option that gives its
web helper the `VaapiVideoDecodeLinuxGL` and `VaapiIgnoreDriverChecks` Chromium
features. `VaapiIgnoreDriverChecks` matters
here: without it Chromium accepts only the driver vendor strings it knows.
