#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pin the Virtio-GPU context attachment of handles opened before a context.

Both maintained kernel lines must attach a buffer a file imported before it
had a context once that context is created. Without it, Android's gralloc
mapper reads an imported buffer through a context the host never attached
the buffer to, and every such readback, including every screenshot, fails
with VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import csv
from pathlib import Path


SUBJECT = "drm/virtio: attach a buffer imported before the context exists"
PATHS = (
    "drivers/gpu/drm/virtio/virtgpu_context_attach.h",
    "drivers/gpu/drm/virtio/virtgpu_gem.c",
    "drivers/gpu/drm/virtio/virtgpu_ioctl.c",
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def topic(series: Path, subject: str) -> str:
    with (series / "manifest.tsv").open(encoding="utf-8", newline="") as stream:
        rows = list(
            csv.DictReader(
                (line for line in stream if not line.startswith("#")),
                delimiter="\t",
            )
        )
    matches = [row for row in rows if row["subject"] == subject]
    require(len(matches) == 1, f"{series}: expected one topic '{subject}'")
    return (series / matches[0]["filename"]).read_text(encoding="utf-8")


def verify(line: str, patch: str) -> None:
    changed = tuple(
        header.split(" b/", 1)[1]
        for header in patch.splitlines()
        if header.startswith("diff --git ")
    )
    require(
        tuple(sorted(changed)) == PATHS,
        f"{line}: topic must change exactly {PATHS}, not {changed}",
    )

    opened = patch.find("int virtio_gpu_gem_object_open(")
    locked = patch.find("+\tmutex_lock(&vfpriv->context_lock);", opened)
    attached = patch.find("virtio_gpu_cmd_context_attach_resource", locked)
    deferred = patch.find("+\tret = virtio_gpu_defer_handle(vfpriv, objs);", attached)
    require(
        -1 not in (opened, locked, attached, deferred)
        and opened < locked < attached < deferred,
        f"{line}: a handle opened without a context must wait for it under "
        "the context lock",
    )

    closed = patch.find("void virtio_gpu_gem_object_close(")
    forgotten = patch.find("+\t\tvirtio_gpu_forget_handle(vfpriv, obj);", closed)
    require(
        -1 not in (closed, forgotten) and closed < forgotten,
        f"{line}: closing a waiting handle must drop it, not detach it",
    )

    created = patch.find("+\tvirtio_gpu_attach_pending_handles(vgdev, vfpriv);")
    flag = patch.rfind(" \tvfpriv->context_created = true;", 0, created)
    require(
        -1 not in (created, flag) and flag < created,
        f"{line}: creating a context must attach the file's waiting handles",
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mainline-series", type=Path, required=True)
    parser.add_argument("--android-series", type=Path, required=True)
    arguments = parser.parse_args()

    verify("mainline", topic(arguments.mainline_series.resolve(), SUBJECT))
    verify(
        "android",
        topic(arguments.android_series.resolve(), f"UML-DARWIN-LOCAL: {SUBJECT}"),
    )
    print("Virtio-GPU pending context attachment verified on both lines")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
