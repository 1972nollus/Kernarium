#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""The product's default wallpaper: a bounded copy of LineageOS's.

LineageOS ships its default wallpaper as square, nearly detail-free PNGs, one per
density bucket, and the `sw600dp` and `sw720dp` variants of every display wide
enough for a tablet are 1920 x 1920. SystemUI's ImageWallpaper fixes its surface
to the bitmap size, so a 1133 x 687 display then holds a 14 MiB buffer that
SurfaceFlinger scales down into a 1133 x 1133 wallpaper window. The overlay
replaces those two variants with a 1024 x 1024 copy of the same image: the
window, and with it the home screen's parallax range, is sized from the display
and not from the bitmap, so only the buffer shrinks, to 4 MiB.

LineageOS's overlay names the same resource, and where two overlay directories do
that, the one listed earlier in PRODUCT_PACKAGE_OVERLAYS wins (Soong gives the
earlier directory the higher priority). inherit-product expands the inherited list
where the call stands, so the copies live in their own directory,
`overlay/overlay-priority`, which `overlay/lineage_darwin.mk` lists before its
inherit-product calls; the ordinary `overlay/overlay` directory is listed after
them and ranks under LineageOS's and Cuttlefish's.

`check` verifies the committed files and that order, `generate` rewrites the files
from LineageOS's pinned `drawable-sw600dp-nodpi/default_wallpaper.png` with
ImageMagick. The verifier of the built target files uses `png_size` on the
framework runtime overlay, which is where the order shows.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

# The overlay directories whose default_wallpaper.png this product replaces. The platform
# prefers the sw720dp variant to the sw600dp one wherever it applies, so both are needed.
VARIANT_DIRECTORIES = ("drawable-sw600dp-nodpi", "drawable-sw720dp-nodpi")
OVERLAY_RESOURCES = "overlay/overlay-priority/frameworks/base/core/res/res"
# The product makefile, and the two PRODUCT_PACKAGE_OVERLAYS entries whose order matters.
PRODUCT_MAKEFILE = "overlay/lineage_darwin.mk"
PRIORITY_OVERLAY = "device/kernarium/darwin/overlay-priority"
ORDINARY_OVERLAY = "device/kernarium/darwin/overlay"
ORDINARY_RESOURCES = "overlay/overlay/frameworks/base/core/res/res"
FILE_NAME = "default_wallpaper.png"
# No variant of the built framework overlay may be larger than this, on either side.
MAX_SIDE = 1024
# LineageOS's own sw600dp and sw720dp file (the two are identical) that the copy comes from.
SOURCE_SHA256 = "ae3e5e7754300922dd499e969cf2fa892b2f0a429502ccf750289338cc18374d"
SOURCE_SIDE = 1920
# The committed copy. A different ImageMagick can write other bytes for the same pixels, so a
# regeneration prints the new digest for this line.
OUTPUT_SHA256 = "bb23a32ae02c79ce1e8ea04e856293518cebd60a68f4cd65ad7b08d9e59e7f26"
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


class WallpaperError(Exception):
    pass


def png_size(data: bytes) -> tuple[int, int, int, int]:
    """Width, height, bit depth and colour type of a PNG, read from its IHDR chunk."""
    if data[:8] != PNG_SIGNATURE or data[12:16] != b"IHDR" or len(data) < 29:
        raise WallpaperError("not a PNG with a leading IHDR chunk")
    width, height, depth, color = struct.unpack(">IIBB", data[16:26])
    return width, height, depth, color


def check_files(android_root: Path) -> None:
    """The priority overlay holds the same bounded 8-bit RGB copy under both variants."""
    resources = android_root / OVERLAY_RESOURCES
    payloads = []
    for directory in VARIANT_DIRECTORIES:
        path = resources / directory / FILE_NAME
        if not path.is_file():
            raise WallpaperError(f"the overlay has no {directory}/{FILE_NAME}")
        data = path.read_bytes()
        try:
            width, height, depth, color = png_size(data)
        except WallpaperError as error:
            raise WallpaperError(f"{directory}/{FILE_NAME} is {error}") from None
        if width != height or width > MAX_SIDE:
            raise WallpaperError(
                f"{directory}/{FILE_NAME} is {width}x{height}: the default wallpaper is a square "
                f"of at most {MAX_SIDE} pixels"
            )
        if depth != 8 or color != 2:
            raise WallpaperError(
                f"{directory}/{FILE_NAME} must be 8-bit RGB without alpha (bit depth {depth}, "
                f"colour type {color})"
            )
        payloads.append(data)
    if len(set(payloads)) != 1:
        raise WallpaperError("the sw600dp and sw720dp default wallpapers differ")
    digest = hashlib.sha256(payloads[0]).hexdigest()
    if digest != OUTPUT_SHA256:
        raise WallpaperError(
            f"the default wallpaper digest is {digest}, not {OUTPUT_SHA256}: regenerate it with "
            "`tools/default_wallpaper.py generate` and record the new digest"
        )


def check_precedence(android_root: Path) -> None:
    """The priority overlay is listed before the first inherit-product call, the ordinary one after it.

    The built framework overlay is the final proof (the verifier and the canary read it), but this
    fails in the source tree, before a build, when the directory that has to win ranks under
    LineageOS's."""
    text = (android_root / PRODUCT_MAKEFILE).read_text(encoding="utf-8")
    lines = [re.sub(r"#.*", "", line).strip() for line in text.splitlines()]

    def positions(entry: str) -> list[int]:
        pattern = re.compile(r"^PRODUCT_PACKAGE_OVERLAYS\s*\+=\s*" + re.escape(entry) + r"\s*$")
        return [number for number, line in enumerate(lines) if pattern.match(line)]

    inherits = [number for number, line in enumerate(lines) if line.startswith("$(call inherit-product,")]
    if not inherits:
        raise WallpaperError(f"{PRODUCT_MAKEFILE} has no inherit-product call to order against")
    priority = positions(PRIORITY_OVERLAY)
    if len(priority) != 1:
        raise WallpaperError(f"{PRODUCT_MAKEFILE} must add {PRIORITY_OVERLAY} to PRODUCT_PACKAGE_OVERLAYS exactly once")
    if priority[0] > inherits[0]:
        raise WallpaperError(
            f"{PRODUCT_MAKEFILE} adds {PRIORITY_OVERLAY} after its first inherit-product call: the "
            "directory then ranks under LineageOS's overlay and its default wallpaper is not used"
        )
    ordinary = positions(ORDINARY_OVERLAY)
    if len(ordinary) != 1 or ordinary[0] < inherits[-1]:
        raise WallpaperError(
            f"{PRODUCT_MAKEFILE} must add {ORDINARY_OVERLAY} once, after its inherit-product calls"
        )
    for directory in VARIANT_DIRECTORIES:
        if (android_root / ORDINARY_RESOURCES / directory / FILE_NAME).exists():
            raise WallpaperError(
                f"{directory}/{FILE_NAME} is also in the ordinary overlay, which ranks under "
                "LineageOS's: keep it only in the priority overlay"
            )


def generate(android_root: Path, source: Path, magick: str) -> str:
    data = source.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != SOURCE_SHA256:
        raise WallpaperError(
            f"{source} is not LineageOS's default wallpaper ({digest}, expected {SOURCE_SHA256}); "
            "a new Lineage pin needs this tool's SOURCE_SHA256 and OUTPUT_SHA256 reviewed"
        )
    width, height, _, _ = png_size(data)
    if (width, height) != (SOURCE_SIDE, SOURCE_SIDE):
        raise WallpaperError(f"{source} is {width}x{height}, not {SOURCE_SIDE}x{SOURCE_SIDE}")
    executable = shutil.which(magick)
    if executable is None:
        raise WallpaperError(f"{magick} is not installed")
    resources = android_root / OVERLAY_RESOURCES
    with tempfile.TemporaryDirectory(prefix="default-wallpaper-") as temporary:
        output = Path(temporary) / FILE_NAME
        subprocess.run(
            [executable, str(source), "-filter", "Lanczos", "-resize", f"{MAX_SIDE}x{MAX_SIDE}!",
             "-strip", "-depth", "8", "-define", "png:compression-level=9",
             "-define", "png:exclude-chunk=all", f"PNG24:{output}"],
            check=True, stdin=subprocess.DEVNULL,
        )
        written = output.read_bytes()
        for directory in VARIANT_DIRECTORIES:
            target = resources / directory
            target.mkdir(parents=True, exist_ok=True)
            (target / FILE_NAME).write_bytes(written)
    return hashlib.sha256(written).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("action", choices=("check", "generate"), nargs="?", default="check")
    parser.add_argument("--android-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--source", type=Path,
                        help="LineageOS's drawable-sw600dp-nodpi/default_wallpaper.png (generate)")
    parser.add_argument("--magick", default="magick")
    arguments = parser.parse_args()
    try:
        if arguments.action == "generate":
            if arguments.source is None:
                raise WallpaperError("generate needs --source")
            print(f"wrote the default wallpaper, sha256 {generate(arguments.android_root, arguments.source, arguments.magick)}")
        else:
            check_files(arguments.android_root)
            check_precedence(arguments.android_root)
            print("default wallpaper overlay: ok")
    except (WallpaperError, subprocess.CalledProcessError, OSError) as error:
        print(f"default wallpaper: {error}", file=sys.stderr)
        raise SystemExit(1) from None


if __name__ == "__main__":
    main()
