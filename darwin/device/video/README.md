# Virtio video decoder

`UML::VideoDevice` exposes a stateful V4L2 decoder through Virtio device 31.
The device owns command validation, stream state, guest buffer mappings, and
completion events. It reuses the common vhost-user transport; decoding belongs
to `UML::VideoDecoder` in the platform layer. No GPU context is required.

macOS enables the service with `--video`. Kernarium supplies it to each supported
kernel profile on iOS 17 and newer when hardware decoding is available.
The shared mainline and Android driver creates `/dev/video0`;
the Steam image provides the `/dev/video-dec0` udev alias used by CEF.

The Apple backend requires VideoToolbox hardware decoding. Capabilities match
the maintained Virgl video backend: H.264 Baseline, Constrained Baseline, Main,
and High; HEVC Main and Main10; and baseline JPEG, subject to host availability.
H.264 High10 is conditional on the iOS profile probe and is excluded on macOS.
The platform also decodes VP9 Profile 0 where VideoToolbox offers it as a
supplemental decoder, but this wire contract has no VP9 identifier, so the
device drops any capability the protocol cannot name rather than publishing an
unnamed format. Adding VP9 to the queue is a wire and kernel-driver change.
Decoded surfaces are copied into guest NV12, planar YUV420, or P010 buffers.
This path does not implement encoding or share GPU surfaces.

Input uses complete Annex B H.264/HEVC access units or complete JPEG frames.
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
