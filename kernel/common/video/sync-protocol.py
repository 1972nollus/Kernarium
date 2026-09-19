#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Synchronize the private Virtio video wire header into a kernel topic tree."""
__import__("sys").dont_write_bytecode = True

import argparse
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--kernel-source", type=Path, required=True)
parser.add_argument("--verify", action="store_true")
args = parser.parse_args()
source = Path(__file__).with_name("virtio_video.h")
destination = args.kernel_source / "include/uapi/linux/virtio_video.h"
if not (args.kernel_source / "Makefile").is_file() or not destination.parent.is_dir():
    parser.error("--kernel-source must name a prepared Linux source tree")
if args.verify:
    if not destination.is_file() or source.read_bytes() != destination.read_bytes():
        parser.error("Virtio video protocol copy is stale")
else:
    destination.write_bytes(source.read_bytes())
