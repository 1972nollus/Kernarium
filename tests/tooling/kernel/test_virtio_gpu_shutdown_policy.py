#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pin the Virtio-GPU system-shutdown ordering carried by the kernel series."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import csv
from pathlib import Path


SUBJECT = "drm/virtio: shut down atomic state before unplug"
DRIVER = "drivers/gpu/drm/virtio/virtgpu_drv.c"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--series-manifest", type=Path, required=True)
    parser.add_argument("--series-directory", type=Path, required=True)
    arguments = parser.parse_args()

    with arguments.series_manifest.resolve().open(
        encoding="utf-8", newline=""
    ) as stream:
        rows = list(
            csv.DictReader(
                (line for line in stream if not line.startswith("#")),
                delimiter="\t",
            )
        )
    matches = [row for row in rows if row["subject"] == SUBJECT]
    require(len(matches) == 1, "shutdown policy must have one exact topic")

    patch = (
        arguments.series_directory.resolve() / matches[0]["filename"]
    ).read_text(encoding="utf-8")
    require(
        patch.count(f"diff --git a/{DRIVER} b/{DRIVER}") == 1,
        "shutdown topic must change only the owned Virtio-GPU driver path",
    )
    require(
        patch.count("diff --git ") == 1,
        "shutdown topic unexpectedly changes another path",
    )

    function = patch.find("static void virtio_gpu_shutdown")
    feature = patch.find(
        "+\tif (drm_core_check_feature(dev, DRIVER_ATOMIC))", function
    )
    shutdown = patch.find("+\t\tdrm_atomic_helper_shutdown(dev);", feature)
    unplug = patch.find(" \tdrm_dev_unplug(dev);", shutdown)
    require(
        -1 not in (function, feature, shutdown, unplug)
        and function < feature < shutdown < unplug,
        "atomic scanout must shut down before Virtio-GPU is unplugged",
    )
    require(
        "-\tdrm_dev_unplug(dev);" not in patch,
        "shutdown topic must retain the standard device unplug",
    )

    print("Virtio-GPU shutdown ordering verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
