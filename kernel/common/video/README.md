# Private Virtio video wire contract

`virtio_video.h` owns the packed little-endian protocol shared by the host
Virtio video device and the maintained mainline and Android kernel drivers.
This is a project-local version of the stateful Virtio video draft, using device
ID 31. It is independent of the public API and host/kernel bridge ABI.

Edit this canonical header, then use `sync-protocol.py --kernel-source PATH` to update
the copy under `include/uapi/linux/` in each patch-producing kernel checkout.
Amend the owning video topic and regenerate each line's series. Ordinary checks
use `sync-protocol.py --kernel-source PATH --verify`. Never edit generated patch output.

The protocol supports guest-page resources, separate command and event queues,
H.264/HEVC/JPEG coded input, and NV12/YUV420/P010 output. Resource flags are bit
masks. Stream IDs, buffer extents, and response capacities are validated by the
host. The wire format is shared by both guest page profiles and all maintained
kernel slices; Android has only a 16 KiB profile.
