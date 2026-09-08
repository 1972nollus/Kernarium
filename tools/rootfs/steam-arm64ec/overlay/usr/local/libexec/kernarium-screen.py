#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Kernarium boot and installation display, using the image's Cairo and X11."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import ctypes as C
import json
import math
from pathlib import Path
import time

P, I, U, D = C.c_void_p, C.c_int, C.c_ulong, C.c_double


def function(library, name, result, *arguments):
    operation = getattr(library, name)
    operation.restype = result
    operation.argtypes = list(arguments)
    return operation


class Canvas:
    def __init__(self, framebuffer: bool) -> None:
        self.cairo = C.CDLL("libcairo.so.2")
        self.x11 = None
        if framebuffer:
            from kernarium_drm import Scanout
            self.drm = Scanout()
            self.width, self.height = self.drm.width, self.drm.height
            self.surface = function(self.cairo, "cairo_image_surface_create_for_data", P, P, I, I, I, I)(
                self.drm.pixels, 0, self.width, self.height, self.drm.pitch)
        else:
            self.x11 = C.CDLL("libX11.so.6")
            self.display = function(self.x11, "XOpenDisplay", P, C.c_char_p)(None)
            if not self.display:
                raise RuntimeError("Gamescope Xwayland display is unavailable")
            screen = function(self.x11, "XDefaultScreen", I, P)(self.display)
            root = function(self.x11, "XRootWindow", U, P, I)(self.display, screen)
            self.width = function(self.x11, "XDisplayWidth", I, P, I)(self.display, screen)
            self.height = function(self.x11, "XDisplayHeight", I, P, I)(self.display, screen)
            self.window = function(self.x11, "XCreateSimpleWindow", U, P, U, I, I, C.c_uint, C.c_uint,
                                   C.c_uint, U, U)(self.display, root, 0, 0, self.width, self.height, 0, 0, 0)
            function(self.x11, "XStoreName", I, P, U, C.c_char_p)(self.display, self.window, b"Kernarium")
            # Gamescope's Steam session admits windows carrying the Steam UI
            # app id. Publish it before mapping, just as the client does.
            atom = function(self.x11, "XInternAtom", U, P, C.c_char_p, I)
            change = function(self.x11, "XChangeProperty", I, P, U, U, U, I, I, P, I)
            app_id = U(769)
            change(self.display, self.window, atom(self.display, b"STEAM_GAME", 0),
                   6, 32, 0, C.byref(app_id), 1)
            base = atom(self.display, b"GAMESCOPECTRL_BASELAYER_APPID", 0)
            actual, count, after, value, fmt = U(), U(), U(), P(), I()
            function(self.x11, "XGetWindowProperty", I, P, U, U, C.c_long, C.c_long, I, U,
                     P, P, P, P, P)(self.display, root, base, 0, 1, 0, 0,
                                    C.byref(actual), C.byref(fmt), C.byref(count), C.byref(after), C.byref(value))
            if not actual.value:
                change(self.display, root, base, 6, 32, 0, C.byref(app_id), 1)
            if value:
                function(self.x11, "XFree", I, P)(value)
            function(self.x11, "XMapWindow", I, P, U)(self.display, self.window)
            visual = function(self.x11, "XDefaultVisual", P, P, I)(self.display, screen)
            self.surface = function(self.cairo, "cairo_xlib_surface_create", P, P, U, P, I, I)(
                self.display, self.window, visual, self.width, self.height)
        self.context = function(self.cairo, "cairo_create", P, P)(self.surface)
        self.color = function(self.cairo, "cairo_set_source_rgb", None, P, D, D, D)
        self.rectangle = function(self.cairo, "cairo_rectangle", None, P, D, D, D, D)
        self.fill = function(self.cairo, "cairo_fill", None, P)
        self.move = function(self.cairo, "cairo_move_to", None, P, D, D)
        self.font_size = function(self.cairo, "cairo_set_font_size", None, P, D)
        self.text_extents = function(self.cairo, "cairo_text_extents", None, P, C.c_char_p, P)
        self.show_text = function(self.cairo, "cairo_show_text", None, P, C.c_char_p)
        function(self.cairo, "cairo_select_font_face", None, P, C.c_char_p, I, I)(
            self.context, b"DejaVu Sans", 0, 0)

    def text(self, value: str, size: float, y: float) -> None:
        self.font_size(self.context, size)
        value_bytes = value.encode()
        extents = (D * 6)()
        self.text_extents(self.context, value_bytes, extents)
        self.move(self.context, (self.width - extents[2]) / 2 - extents[0], y)
        self.show_text(self.context, value_bytes)

    def draw(self, message: str, fraction: float | None) -> None:
        cr = self.context
        scale = min(self.width / 1280, self.height / 720)
        self.color(cr, 0.035, 0.047, 0.078)
        self.rectangle(cr, 0, 0, self.width, self.height)
        self.fill(cr)
        self.color(cr, 0.39, 0.71, 1.0)
        self.text("K", 82 * scale, self.height * 0.40)
        self.color(cr, 0.94, 0.96, 1.0)
        self.text("Kernarium", 38 * scale, self.height * 0.50)
        bar_width = min(520 * scale, self.width * 0.8)
        left, top = (self.width - bar_width) / 2, self.height * 0.61
        self.color(cr, 0.13, 0.17, 0.24)
        self.rectangle(cr, left, top, bar_width, 4 * scale)
        self.fill(cr)
        self.color(cr, 0.39, 0.71, 1.0)
        if fraction is None:
            offset = (math.sin(time.monotonic() * 1.5) + 1) / 2 * 0.75
            self.rectangle(cr, left + bar_width * offset, top, bar_width * 0.25, 4 * scale)
        else:
            self.rectangle(cr, left, top, bar_width * min(max(fraction, 0), 1), 4 * scale)
        self.fill(cr)
        self.color(cr, 0.66, 0.73, 0.83)
        # Wrap long recovery messages without cutting off the actionable text.
        import textwrap
        for index, line in enumerate(textwrap.wrap(message, 74)[:4]):
            self.text(line, 17 * scale, self.height * 0.69 + index * 25 * scale)
        function(self.cairo, "cairo_surface_flush", None, P)(self.surface)
        if self.x11:
            function(self.x11, "XFlush", I, P)(self.display)
        else:
            self.drm.present()

    def close(self) -> None:
        function(self.cairo, "cairo_destroy", None, P)(self.context)
        function(self.cairo, "cairo_surface_destroy", None, P)(self.surface)
        if self.x11:
            function(self.x11, "XDestroyWindow", I, P, U)(self.display, self.window)
            function(self.x11, "XCloseDisplay", I, P)(self.display)
        else:
            self.drm.close()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--framebuffer", action="store_true")
    parser.add_argument("--status", type=Path, default=Path("/run/kernarium-progress.json"))
    args = parser.parse_args()
    canvas = Canvas(args.framebuffer)
    deadline = time.monotonic() + (600 if args.framebuffer else 3600)
    while time.monotonic() < deadline:
        if args.framebuffer and Path("/run/kernarium-stop-splash").exists():
            break
        try:
            state = json.loads(args.status.read_text())
        except (OSError, ValueError):
            state = {"message": "Starting Steam", "fraction": None}
        canvas.draw(state["message"], state.get("fraction"))
        time.sleep(0.25)
    canvas.close()


if __name__ == "__main__":
    main()
