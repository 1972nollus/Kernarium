#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Guest side of guest-video-teardown: keep coded buffers in the decoder.

Usage: video_teardown_probe.py PACKET FIFO
       video_teardown_probe.py --gone

Opens /dev/video0, sets an H.264 coded format, fills every coded buffer
with the access unit in PACKET, queues them all and starts the coded queue.
No capture format is ever chosen, so the device keeps the packets it has
read and the driver still owns their buffers. The client prints
VIDEO_CLIENT_STREAMING and waits for a line on FIFO. It then tries one
dequeue and closes the device, printing VIDEO_CLIENT_CLOSED with the
dequeue's errno (0 if one succeeded), so a close that waits for a buffer
nobody returns never prints it.

With --gone it succeeds only if opening /dev/video0 fails as it does for a
removed device: with ENOENT once the node has gone, or ENXIO or ENODEV.
"""

import array
import errno
import fcntl
import mmap
import os
import struct
import sys

# The arm64 Linux values of these requests and of the structure sizes and
# offsets below.
VIDIOC_S_FMT = 0xC0D05605
VIDIOC_REQBUFS = 0xC0145608
VIDIOC_QUERYBUF = 0xC0585609
VIDIOC_QBUF = 0xC058560F
VIDIOC_DQBUF = 0xC0585611
VIDIOC_STREAMON = 0x40045612
OUTPUT_MPLANE = 10
MEMORY_MMAP = 1
H264 = int.from_bytes(b"H264", "little")
FORMAT_SIZE = 208
REQUEST_SIZE = 20
BUFFER_SIZE = 88
PLANE_SIZE = 64
MAX_PLANES = 8
COUNT = 4


def planes() -> array.array:
    return array.array("B", bytes(MAX_PLANES * PLANE_SIZE))


def buffer(index: int, plane_array: array.array, count: int) -> bytearray:
    """A struct v4l2_buffer for coded buffer ``index`` and its planes."""
    value = bytearray(BUFFER_SIZE)
    struct.pack_into("<II", value, 0, index, OUTPUT_MPLANE)
    struct.pack_into("<qq", value, 24, 7, index + 1)
    struct.pack_into("<I", value, 60, MEMORY_MMAP)
    struct.pack_into("<Q", value, 64, plane_array.buffer_info()[0])
    struct.pack_into("<I", value, 72, count)
    return value


def gone() -> int:
    try:
        os.close(os.open("/dev/video0", os.O_RDWR))
    except OSError as error:
        if error.errno in (errno.ENOENT, errno.ENXIO, errno.ENODEV):
            print(f"VIDEO_CLIENT_GONE errno={error.errno}", flush=True)
            return 0
        raise
    raise RuntimeError("/dev/video0 still opens")


def main() -> int:
    if sys.argv[1:] == ["--gone"]:
        return gone()
    packet = open(sys.argv[1], "rb").read()
    fifo = sys.argv[2]
    fd = os.open("/dev/video0", os.O_RDWR | os.O_NONBLOCK)

    form = bytearray(FORMAT_SIZE)
    struct.pack_into("<I", form, 0, OUTPUT_MPLANE)
    struct.pack_into("<III", form, 8, 128, 96, H264)
    struct.pack_into("<I", form, 28, len(packet))
    form[188] = 1
    fcntl.ioctl(fd, VIDIOC_S_FMT, form)

    request = bytearray(REQUEST_SIZE)
    struct.pack_into("<III", request, 0, COUNT, OUTPUT_MPLANE, MEMORY_MMAP)
    fcntl.ioctl(fd, VIDIOC_REQBUFS, request)
    count = struct.unpack_from("<I", request, 0)[0]
    if count < 1:
        raise RuntimeError("REQBUFS gave no coded buffer")

    lengths = []
    mappings = []
    for index in range(count):
        plane_array = planes()
        query = buffer(index, plane_array, MAX_PLANES)
        fcntl.ioctl(fd, VIDIOC_QUERYBUF, query)
        length, offset = struct.unpack_from("<II", plane_array, 4)
        if length < len(packet):
            raise RuntimeError(f"coded buffer {index} holds {length} bytes")
        mapping = mmap.mmap(
            fd, length, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE,
            offset=offset)
        mapping[:len(packet)] = packet
        lengths.append(length)
        mappings.append(mapping)

    fcntl.ioctl(fd, VIDIOC_STREAMON, struct.pack("<i", OUTPUT_MPLANE))
    for index in range(count):
        plane_array = planes()
        struct.pack_into("<II", plane_array, 0, len(packet), lengths[index])
        fcntl.ioctl(fd, VIDIOC_QBUF, buffer(index, plane_array, 1))
    print(f"VIDEO_CLIENT_STREAMING buffers={count}", flush=True)

    with open(fifo) as trigger:
        trigger.readline()

    result = 0
    try:
        fcntl.ioctl(fd, VIDIOC_DQBUF, buffer(0, planes(), MAX_PLANES))
    except OSError as error:
        result = error.errno or errno.EIO
    for mapping in mappings:
        mapping.close()
    os.close(fd)
    print(f"VIDEO_CLIENT_CLOSED dqbuf_errno={result}", flush=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:  # noqa: BLE001 - reported to the runner
        print(f"VIDEO_CLIENT_ERROR {type(error).__name__}: {error}",
              flush=True)
        raise SystemExit(1)
