#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Own a temporary primary DRM scanout for the early Kernarium splash."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import ctypes as C
import fcntl
import mmap
import os
from pathlib import Path
import struct
import time

U32, U16, P = C.c_uint32, C.c_uint16, C.c_void_p


# Public libdrm structures from xf86drmMode.h. These are native ARM64 layouts.
class Mode(C.Structure):
    _fields_ = [("clock", U32), *[(name, U16) for name in (
        "hdisplay", "hsync_start", "hsync_end", "htotal", "hskew",
        "vdisplay", "vsync_start", "vsync_end", "vtotal", "vscan")],
        ("vrefresh", U32), ("flags", U32), ("type", U32), ("name", C.c_char * 32)]


class Resources(C.Structure):
    _fields_ = [item for name in ("fbs", "crtcs", "connectors", "encoders")
                for item in (("count_" + name, C.c_int), (name, C.POINTER(U32)))] + [
                    (name, U32) for name in ("min_width", "max_width", "min_height", "max_height")]


class Connector(C.Structure):
    _fields_ = [(name, U32) for name in (
        "connector_id", "encoder_id", "connector_type", "connector_type_id",
        "connection", "mmWidth", "mmHeight", "subpixel")] + [
        ("count_modes", C.c_int), ("modes", C.POINTER(Mode)),
        ("count_props", C.c_int), ("props", C.POINTER(U32)), ("prop_values", C.POINTER(C.c_uint64)),
        ("count_encoders", C.c_int), ("encoders", C.POINTER(U32))]


class Scanout:
    def bind(self, name, result, *arguments):
        operation = getattr(self.library, name)
        operation.restype, operation.argtypes = result, list(arguments)
        return operation

    def checked(self, value: int, operation: str) -> None:
        if value:
            raise OSError(C.get_errno(), f"DRM splash {operation} failed")

    def __init__(self) -> None:
        deadline = time.monotonic() + 30
        while not Path("/dev/dri/card0").exists():
            if time.monotonic() > deadline:
                raise RuntimeError("DRM boot display did not appear")
            time.sleep(0.1)
        self.library = C.CDLL("libdrm.so.2", use_errno=True)
        self.fd = os.open("/dev/dri/card0", os.O_RDWR | os.O_CLOEXEC)
        self.checked(self.bind("drmSetMaster", C.c_int, C.c_int)(self.fd), "master acquisition")
        resources = self.bind("drmModeGetResources", C.POINTER(Resources), C.c_int)(self.fd)
        if not resources or resources.contents.count_crtcs < 1:
            raise RuntimeError("DRM has no boot scanout")
        mode = None
        try:
            self.crtc = resources.contents.crtcs[0]
            for index in range(resources.contents.count_connectors):
                connector = self.bind("drmModeGetConnector", C.POINTER(Connector), C.c_int, U32)(
                    self.fd, resources.contents.connectors[index])
                if not connector:
                    continue
                try:
                    if connector.contents.connection == 1 and connector.contents.count_modes:
                        self.connector = U32(connector.contents.connector_id)
                        mode = Mode.from_buffer_copy(connector.contents.modes[0])
                        break
                finally:
                    self.bind("drmModeFreeConnector", None, P)(connector)
        finally:
            self.bind("drmModeFreeResources", None, P)(resources)
        if mode is None:
            raise RuntimeError("DRM has no connected boot display")
        self.width, self.height = mode.hdisplay, mode.vdisplay
        create = bytearray(struct.pack("6IQ", self.height, self.width, 32, 0, 0, 0, 0))
        fcntl.ioctl(self.fd, 0xC02064B2, create)  # DRM_IOCTL_MODE_CREATE_DUMB
        _, _, _, _, self.handle, self.pitch, length = struct.unpack("6IQ", create)
        mapping = bytearray(struct.pack("IIQ", self.handle, 0, 0))
        fcntl.ioctl(self.fd, 0xC01064B3, mapping)  # DRM_IOCTL_MODE_MAP_DUMB
        offset = struct.unpack("IIQ", mapping)[2]
        self.mapping = mmap.mmap(self.fd, length, offset=offset)
        self.pixels = (C.c_ubyte * length).from_buffer(self.mapping)
        self.framebuffer = U32()
        self.checked(self.bind("drmModeAddFB", C.c_int, C.c_int, U32, U32, C.c_uint8, C.c_uint8,
                               U32, U32, P)(self.fd, self.width, self.height, 24, 32,
                                           self.pitch, self.handle, C.byref(self.framebuffer)), "framebuffer creation")
        self.checked(self.bind("drmModeSetCrtc", C.c_int, C.c_int, U32, U32, U32, U32, P, C.c_int, P)(
            self.fd, self.crtc, self.framebuffer.value, 0, 0, C.byref(self.connector), 1, C.byref(mode)), "modeset")

    def present(self) -> None:
        self.checked(self.bind("drmModeDirtyFB", C.c_int, C.c_int, U32, P, U32)(
            self.fd, self.framebuffer.value, None, 0), "pixel update")

    def close(self) -> None:
        self.bind("drmModeRmFB", C.c_int, C.c_int, U32)(self.fd, self.framebuffer.value)
        del self.pixels
        self.mapping.close()
        fcntl.ioctl(self.fd, 0xC00464B4, struct.pack("I", self.handle))
        self.bind("drmDropMaster", C.c_int, C.c_int)(self.fd)
        os.close(self.fd)
