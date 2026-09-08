#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Publish a staged dependency prefix with Darwin's exclusive rename."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import ctypes
import errno
import os
from pathlib import Path
import sys


RENAME_EXCL = 0x00000004


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print("usage: publish-prefix.py SOURCE DESTINATION", file=sys.stderr)
        return 2

    source = Path(argv[1])
    destination = Path(argv[2])
    if not source.is_dir() or source.is_symlink():
        print(f"invalid staged prefix: {source}", file=sys.stderr)
        return 2
    if source.parent.resolve() != destination.parent.resolve():
        print("source and destination must share a parent", file=sys.stderr)
        return 2

    libc = ctypes.CDLL(None, use_errno=True)
    rename_exclusive = libc.renamex_np
    rename_exclusive.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint]
    rename_exclusive.restype = ctypes.c_int
    if rename_exclusive(
        os.fsencode(source), os.fsencode(destination), RENAME_EXCL
    ) == 0:
        return 0

    error = ctypes.get_errno()
    if error == errno.EEXIST:
        return 1
    print(f"exclusive prefix publication failed: {os.strerror(error)}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
