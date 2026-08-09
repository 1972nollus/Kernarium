#!/usr/bin/env python3
"""Advisory sidecar locking shared by rootfs verification and provisioning."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import contextlib
import fcntl
import os
from pathlib import Path
from collections.abc import Iterator


def lock_path(image: Path) -> Path:
    image = image.resolve()
    return image.with_name(f".{image.name}.uml-lock")


@contextlib.contextmanager
def image_lock(
    image: Path, *, exclusive: bool, blocking: bool = True
) -> Iterator[Path]:
    """Hold the image's sidecar lock without conflicting with UML's UBD lock."""
    path = lock_path(image)
    descriptor = os.open(path, os.O_RDWR | os.O_CREAT, 0o600)
    operation = fcntl.LOCK_EX if exclusive else fcntl.LOCK_SH
    if not blocking:
        operation |= fcntl.LOCK_NB
    try:
        fcntl.flock(descriptor, operation)
        yield path
    finally:
        try:
            fcntl.flock(descriptor, fcntl.LOCK_UN)
        finally:
            os.close(descriptor)
