# Private Virtio video wire contract

`virtio_video.h` owns the packed little-endian protocol shared by the host
Virtio video devices and the maintained mainline and Android kernel drivers.
This is a project-local version of the stateful Virtio video draft. The decoder
is device ID 31 and the encoder device ID 30. It is independent of the public
API and host/kernel bridge ABI.

Edit this canonical header, then use `sync-protocol.py --kernel-source PATH` to update
the copy under `include/uapi/linux/` in each patch-producing kernel checkout.
Amend the owning video topic and regenerate each line's series. Ordinary checks
use `sync-protocol.py --kernel-source PATH --verify`. Never edit generated patch output.

The protocol supports guest-page resources, separate command and event queues,
H.264/HEVC/JPEG/VP9 coded input, and NV12/YUV420/P010 or packed BGRA8888 output.
The packed format needs no driver change: the driver already maps it to
`V4L2_PIX_FMT_ABGR32`, and plane count, stride, and plane size come from the
host's negotiated parameters. Resource flags are bit
masks. A scatter entry normally names guest-physical memory; with
`VIRTIO_VIDEO_F_RESOURCE_SHM_REGION` negotiated, an entry may instead name an
offset inside a virtio shared-memory region, which is how a guest describes a
host-visible buffer that has no guest-physical address. Stream IDs, buffer extents, and response capacities are validated by the
host. The wire format is shared by all maintained
kernel slices.

## Failed capture buffers

A capture buffer the device completes with `VIRTIO_VIDEO_DEQUEUE_FLAG_ERR` or
`VIRTIO_VIDEO_DEQUEUE_FLAG_EOS`, on the decoder and the encoder alike, reaches
the client with no payload and the timestamp of the answer, as one holding a
picture does. The device names the picture or frame it failed to deliver, so
the client can tell which one was lost, and answers 0 for a buffer that stands
for none, such as the end-of-stream marker or one returned by a flush. The
`guest-video-error-answer` test has a device fail a capture buffer and checks
that the client reads the timestamp it named.

## Decoder resolution change

The device ends a picture size with one more capture buffer, empty and flagged
`VIRTIO_VIDEO_DEQUEUE_FLAG_EOS`, after every picture of that size, and only then
posts `VIRTIO_VIDEO_EVENT_DECODER_RESOLUTION_CHANGED`. The driver reports the
buffer as a `V4L2_BUF_FLAG_LAST` buffer with no payload and reads the new
parameters before it queues `V4L2_EVENT_SOURCE_CHANGE`. The command queue
completes in order, so a client that sees the event finds every picture of the
old size and the LAST buffer already dequeueable, and `VIDIOC_DQBUF` then fails
with `EPIPE` until it restarts the capture queue. A drain the client has asked
for ends the old size by itself, at the drain's own last buffer after the
capture queue is set up again, so the change gets no marker of its own: a client
takes the first LAST buffer it sees as the end of its drain. The
[device README](../../../darwin/device/video/README.md) owns the device's side.
The `guest-v4l2-decode` probe holds both orders.

## Encoder (device ID 30)

The encoder is its own device instance, and its commands are additions the
decoder never uses, so decoder behavior and a driver without the encoder are
unaffected. The encoder offers `VIRTIO_VIDEO_F_V4L2_CONTROLS`, the driver binds
device 30 only when it is negotiated, and a decoder never offers it. The input
queue carries raw frames (V4L2 OUTPUT) and the output queue carries coded
buffers (V4L2 CAPTURE). `NV16` and `NV24` are raw formats only an encoder
accepts. The comment above `virtio_video_resource_queue_resp` in
`virtio_video.h` states the buffer rules: timestamps, decode order, completion
of an input buffer once the host has copied it, and drain.

Controls cross the wire as V4L2 control IDs, V4L2 menu indices and V4L2 enum
values, which are stable UAPI. The host lists every control it supports with
its type, range, step, default, menu skip mask and flags, and the driver
registers exactly that list, skips an ID it does not know or a menu or compound
type the kernel defines differently, and forwards values unchanged, so a
capability the host learns needs no driver change. A boolean or button is
registered as the host describes it, because the V4L2 framework's table types
some booleans the UAPI names, `PREPEND_SPSPPS_TO_IDR` and
`H264_HIERARCHICAL_CODING` among them, as integers. The
fractional frame interval and the colorimetry of the raw frames travel in a
separate stream-information command. The
[device README](../../../darwin/device/video/README.md#encoder-controls) owns
which controls the host publishes.

The encoder driver gives the host these guarantees. It sends a raw frame only
once the coded queue streams and no drain is pending, so the host builds its
session from settled controls; until then the frames wait in the driver. It
reports the host's own plane layout and ignores a client's strides. The
four colorimetry fields of the raw queue (`colorspace`, `xfer_func`,
`ycbcr_enc` and `quantization`) go to the host as the client states them,
`V4L2_COLORSPACE_RAW` with an explicit transfer function and range among
them, and `G_FMT` reports them as stated: the host's answer is read back for
the coded queue only, which carries what the stream states. A format's coded
and raw masks are read in both directions, and a raw format
is listed under its contiguous V4L2 name, and also under the per-plane name
when the host reports `PER_PLANE`. A packed format is named for its memory
order: BGRA8888 is `AR24` and RGBA8888 is `AB24`, though the decoder's table
keeps its older `RGB4` for RGBA8888. STREAMOFF on either queue sends
`QUEUE_CLEAR`, and STREAMOFF of the raw queue during a drain sends
`STREAM_DRAIN` again, so the next coded buffer is the empty end of stream.
A `CODEC_CONFIG` buffer reaches the client with no frame-type flag, because
V4L2 has none for it.

`virtio_video_v4l2_ids.h` lists the V4L2 numbers the host uses, one
`#define UML_<kernel symbol> <number>` per value. `sync-protocol.py` checks each
against the UAPI headers (`v4l2-controls.h` and `videodev2.h`) of the tree it
synchronizes, and `--verify` checks them as well, so the host and the kernel it
is built beside cannot disagree.
