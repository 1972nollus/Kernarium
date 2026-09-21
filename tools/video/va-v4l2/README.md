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
| images | `vaDeriveImage` (maps the surface itself), `vaCreateImage` + `vaGetImage` + `vaPutImage` for NV12, I420 and ARGB |

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
OpenGL it asks this driver to do it through a video-processing blit. That
conversion costs more on a guest processor than the decode itself, so when the
device can deliver packed RGB the driver asks for it and the blit becomes a
copy of what the host already produced.

| condition | decoded format |
| --- | --- |
| the device does not enumerate `AR24` on its CAPTURE queue | NV12, always |
| `UML_VA_V4L2_RGB=off` | NV12 |
| `UML_VA_V4L2_RGB=force` | packed RGB |
| `auto` (default) | packed RGB when the decoded picture also reaches the client without a guest-side copy, otherwise NV12 |

`auto` decides once, at initialisation, from what the device offers and
whether its capture queue imports dma-bufs - not from what a client has done
so far - so the first decode session of a boot gets the same answer as every
later one.

The copy is why `auto` waits for an importing capture queue: a packed frame is
2.6 times the bytes of NV12, and in a guest, moving it costs about what
converting it costs. Measured on one 1920x1200 frame: 164 ms to convert NV12,
187 ms to copy the packed frame the device produced. Packed decode pays when
the decoder writes into the client's own buffer and nothing is copied at all.

The format is negotiated before the first packet, not after the source-change
event: the device keeps the choice across that event, and changing it once the
host has a decode session asks it to switch layout underneath one.

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
exports `LIBVA_DRIVER_NAME=uml` when the module and `/dev/video-dec0` are both
present, and the client is then started with Valve's `-cef-enable-vaapi`, the
option that gives its web helper the `VaapiVideoDecodeLinuxGL` and
`VaapiIgnoreDriverChecks` Chromium features. `VaapiIgnoreDriverChecks` matters
here: without it Chromium accepts only the driver vendor strings it knows.
