# Virtio video decoder

`UML::VideoDevice` exposes a stateful V4L2 decoder through Virtio device 31.
The device owns command validation, stream state, guest buffer mappings, and
completion events. It reuses the common vhost-user transport; decoding belongs
to `UML::VideoDecoder` in the platform layer. No GPU context is required.

macOS enables the service with `--video`. Kernarium supplies it to each supported
kernel profile when hardware decoding is available.
The shared mainline and Android driver creates `/dev/video0`;
the Steam image provides the `/dev/video-dec0` udev alias used by CEF.

The Apple backend requires VideoToolbox hardware decoding. Its capabilities are
H.264 Baseline, Constrained Baseline, Main, and High; HEVC Main and Main10; and
baseline JPEG, subject to host availability.
H.264 High10 is conditional on the iOS profile probe and is excluded on macOS.
VP9 Profile 0 is added where VideoToolbox offers it as a supplemental decoder,
which every Apple silicon host does; the wire contract and the maintained
driver already name VP9, so this needs no kernel or protocol change. The device
still drops a platform capability the protocol cannot name rather than
publishing an unnamed format on the queue.
Decoded surfaces are copied into guest NV12, planar YUV420, P010, or packed
BGRA buffers. This path does not implement encoding.

Packed BGRA is a single plane of 8-bit B, G, R, A samples, `width * 4` bytes per
row and opaque alpha, reached over the wire as `VIRTIO_VIDEO_FORMAT_BGRA8888`
and in the guest as `V4L2_PIX_FMT_ABGR32`. It is offered for every stream
whatever the coded bit depth is, because a client that can only draw through a
32-bit RGB surface would otherwise convert every frame on a guest processor,
which costs far more than decoding it. Selecting it makes the host decoder
produce the packed samples itself, so no processor in the path converts a
frame.

A client negotiates its capture format only after the source-change event, and
a decoder cannot change a running session's output layout without discarding
the reference frames the next picture needs. So that the choice still reaches
the decoder before its first picture, the device reads each packet's headers
without decoding it, reports the size that raises the source change, and holds
the packet until the client has chosen - by naming a capture format or by
starting the capture queue with the one it was given. The held packets are then
decoded from the stream's own random access point, in the chosen layout, so an
ordinary session converts nothing. That wait is bounded by a packet count and a
byte count; reaching either means the client is never going to choose, and the
device decodes what it holds in the planar layout nobody overrode rather than
stalling the stream.

A client that changes its capture format after frames have already been
delivered is asking mid-stream, where the decoder can only adopt the layout at
the next random access point. It states the layout each frame actually carries,
and the device converts the frames that still arrive planar. The conversion
uses the matrix and range the frame reports, which the decoder reads back from
the surface the hardware produced rather than inferring again, so a converted
frame and a packed one are the same picture and a stream does not change
appearance when the client negotiates part way through.

A capture buffer may also be a host-visible Virtio-GPU blob, which has no
guest-physical address because it lives in the vhost-user shared-memory region
the GPU backend publishes. With `VIRTIO_VIDEO_F_RESOURCE_SHM_REGION` the guest
names such a plane by region and offset, and the device resolves it through the
shared-memory index both backends hold, so decoded planes land directly in the
memory the renderer imported. The device offers the feature only when it was
given that index. A range leaves the index before its owner revokes it from the
guest, and every transfer resolves again, so a buffer whose blob has gone
completes with an error rather than reaching memory the guest no longer has.
The guest chooses the plane layout it negotiated: planes packed into one
dma-buf follow one another at their negotiated sizes.

Input uses complete Annex B H.264/HEVC access units, complete JPEG frames, or
complete VP9 frames.
Streams retain timestamps, signal format changes, drain delayed output, and
complete cancellation and EOS through the command and event queues. Pending
event buffers are bounded by the transport's maximum virtqueue size; full
queues apply backpressure instead of completing empty events.

Whether an application decoded in hardware cannot be settled from inside the
guest: a player that falls back to software shows the same correct picture, and
an open device node is not a decode. Setting `UML_VIDEO_ACTIVITY_LOG` to a
writable path makes the device append the profiles it advertised, every stream a
guest opened with its codec and profile, and each stream's packet, byte, and
delivered-frame totals when it closes. It is a file because the emulator's
stderr shares the guest console. Unset, the device opens and writes nothing.

`virtio-video-protocol` exercises buffer layouts across 4 KiB and 16 KiB guest
memory boundaries, stream lifecycle, EOS, and event backpressure with a synthetic
decoder. `platform-video-decoder` decodes the checked-in generated fixtures on
Apple hardware. Both are included in the `check-gpu` workflow. The optional `guest-v4l2-decode` CTest runs the actual V4L2 driver
against every advertised codec profile and verifies pixels and clean shutdown.
See [testing](../../../docs/testing.md) for the guest probe input.
