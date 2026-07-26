#!/usr/bin/env python3
"""Print the EGLDevice associated with a GBM display.

This is intentionally dependency-free so the immutable guest harness can copy
it into its tmpfs overlay and run it with the guest's Python interpreter.
"""

from __future__ import annotations

import ctypes
import os
import sys


EGL_PLATFORM_GBM_KHR = 0x31D7
EGL_DEVICE_EXT = 0x322C
EGL_EXTENSIONS = 0x3055
EGL_DRM_DEVICE_FILE_EXT = 0x3233
EGL_DRM_RENDER_NODE_FILE_EXT = 0x3377


def check_pointer(value: int | None, operation: str) -> int:
    if not value:
        raise RuntimeError(f"{operation} returned null")
    return value


def main() -> int:
    node = sys.argv[1] if len(sys.argv) > 1 else "/dev/dri/card0"
    gbm = ctypes.CDLL("libgbm.so.1")
    egl = ctypes.CDLL("libEGL.so.1")

    gbm.gbm_create_device.argtypes = [ctypes.c_int]
    gbm.gbm_create_device.restype = ctypes.c_void_p
    gbm.gbm_device_destroy.argtypes = [ctypes.c_void_p]

    egl.eglGetPlatformDisplay.argtypes = [
        ctypes.c_uint,
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_ssize_t),
    ]
    egl.eglGetPlatformDisplay.restype = ctypes.c_void_p
    egl.eglInitialize.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(ctypes.c_int),
    ]
    egl.eglInitialize.restype = ctypes.c_uint
    egl.eglTerminate.argtypes = [ctypes.c_void_p]
    egl.eglTerminate.restype = ctypes.c_uint
    egl.eglGetProcAddress.argtypes = [ctypes.c_char_p]
    egl.eglGetProcAddress.restype = ctypes.c_void_p

    descriptor = os.open(node, os.O_RDWR | os.O_CLOEXEC)
    device = None
    display = None
    try:
        device = check_pointer(gbm.gbm_create_device(descriptor), "gbm_create_device")
        display = check_pointer(
            egl.eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, device, None),
            "eglGetPlatformDisplay",
        )
        major = ctypes.c_int()
        minor = ctypes.c_int()
        if not egl.eglInitialize(display, ctypes.byref(major), ctypes.byref(minor)):
            raise RuntimeError("eglInitialize failed")

        query_display_address = check_pointer(
            egl.eglGetProcAddress(b"eglQueryDisplayAttribEXT"),
            "eglGetProcAddress(eglQueryDisplayAttribEXT)",
        )
        query_device_address = check_pointer(
            egl.eglGetProcAddress(b"eglQueryDeviceStringEXT"),
            "eglGetProcAddress(eglQueryDeviceStringEXT)",
        )
        query_display = ctypes.CFUNCTYPE(
            ctypes.c_uint, ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(ctypes.c_ssize_t)
        )(query_display_address)
        query_device = ctypes.CFUNCTYPE(
            ctypes.c_char_p, ctypes.c_void_p, ctypes.c_int
        )(query_device_address)

        egl_device = ctypes.c_ssize_t()
        if not query_display(display, EGL_DEVICE_EXT, ctypes.byref(egl_device)):
            raise RuntimeError("eglQueryDisplayAttribEXT(EGL_DEVICE_EXT) failed")

        print(f"EGL_VERSION={major.value}.{minor.value}")
        for name, attribute in (
            ("EGL_DEVICE_EXTENSIONS", EGL_EXTENSIONS),
            ("EGL_DRM_DEVICE_FILE_EXT", EGL_DRM_DEVICE_FILE_EXT),
            ("EGL_DRM_RENDER_NODE_FILE_EXT", EGL_DRM_RENDER_NODE_FILE_EXT),
        ):
            value = query_device(egl_device.value, attribute)
            print(f"{name}={value.decode() if value else '<null>'}")
    finally:
        if display:
            egl.eglTerminate(display)
        if device:
            gbm.gbm_device_destroy(device)
        os.close(descriptor)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
