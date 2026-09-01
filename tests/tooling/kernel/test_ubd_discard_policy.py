#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pin the direct-image UBD discard path in both maintained kernel lines."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
from pathlib import Path


DRIVER = "arch/um/drivers/ubd_kern.c"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def verify_patch(path: Path) -> None:
    patch = path.resolve().read_text(encoding="utf-8")
    label = path.parent.parent.parent.name
    require(
        patch.count(f"diff --git a/{DRIVER} b/{DRIVER}") == 1
        and patch.count("diff --git ") == 1,
        f"{label} discard topic must change only the UBD driver",
    )
    for token in (
        "+\tbool direct;",
        "+\tio_req->direct = !dev->cow.file;",
        "UBD_MAX_DIRECT_DISCARD_SECTORS",
    ):
        require(token in patch, f"{label} discard topic lacks {token!r}")

    direct = patch.find(
        "+\tif (req_op(req->req) == REQ_OP_DISCARD && req->direct) {"
    )
    punch = patch.find(
        "+\t\tn = os_falloc_punch(req->fds[0], req->offset, desc->length);",
        direct,
    )
    finish = patch.find("+\t\treturn;", punch)
    generic = patch.find(" \tnsectors = desc->length / req->sectorsize;", finish)
    require(
        -1 not in (direct, punch, finish, generic)
        and direct < punch < finish < generic,
        f"{label} direct discard does not bypass the COW sector-mask loop",
    )
    require(
        "req->offset += desc->length" not in patch,
        f"{label} direct discard mutates state needed only by segmented COW I/O",
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mainline-patch", type=Path, required=True)
    parser.add_argument("--android-patch", type=Path, required=True)
    arguments = parser.parse_args()

    verify_patch(arguments.mainline_patch)
    verify_patch(arguments.android_patch)
    print("direct-image UBD discard policy verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
