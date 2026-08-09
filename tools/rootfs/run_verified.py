#!/usr/bin/env python3
"""Run a command under a shared lock with EROFS integrity pre/post checks."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import subprocess
import sys
from pathlib import Path
from collections.abc import Sequence

from image_lock import image_lock
from verify import DEFAULT_MANIFEST, ROOT, VerificationError, verify


def parse_arguments(arguments: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, default=ROOT / "rootfs.erofs")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    options = parser.parse_args(arguments)
    if options.command[:1] == ["--"]:
        options.command = options.command[1:]
    if not options.command:
        parser.error("a command is required after --")
    return options


def main(arguments: Sequence[str] | None = None) -> int:
    options = parse_arguments(arguments)
    image = options.image.resolve()
    manifest = options.manifest.resolve()
    try:
        with image_lock(image, exclusive=False):
            verify(image, manifest)
            status = 1
            try:
                status = subprocess.run(options.command, check=False).returncode
            finally:
                verify(image, manifest)
            return status
    except (OSError, VerificationError) as error:
        print(f"verified rootfs command failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
