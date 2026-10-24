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
registers exactly that list, skips an ID it does not know, and forwards values
unchanged, so a capability the host learns needs no driver change. The
fractional frame interval and the colorimetry of the raw frames travel in a
separate stream-information command. The
[device README](../../../darwin/device/video/README.md#encoder-controls) owns
which controls the host publishes.

`virtio_video_v4l2_ids.h` lists the V4L2 numbers the host uses, one
`#define UML_<kernel symbol> <number>` per value. `sync-protocol.py` checks each
against the UAPI headers (`v4l2-controls.h` and `videodev2.h`) of the tree it
synchronizes, and `--verify` checks them as well, so the host and the kernel it
is built beside cannot disagree.
