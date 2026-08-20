#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from release_common import verify_release  # noqa: E402


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--release", type=Path, required=True)
    parser.add_argument("--acceptance", type=Path)
    parser.add_argument("--zstd", type=Path)
    parser.add_argument("--expanded-disk", type=Path)
    parser.add_argument(
        "--installed",
        action="store_true",
        help=(
            "validate an installed release and its expanded disk without "
            "retesting the transfer archive"
        ),
    )
    args = parser.parse_args()

    if args.installed and (
        args.expanded_disk is not None or args.zstd is not None
    ):
        parser.error("--installed cannot be combined with --expanded-disk or --zstd")
    expanded_disk = args.expanded_disk
    if args.installed:
        expanded_disk = args.release / "guest/android-17-uml-arm64.img"

    android_root = Path(__file__).resolve().parents[1]
    manifest = verify_release(
        args.release,
        android_root,
        acceptance_path=args.acceptance,
        zstd=args.zstd,
        expanded_disk=expanded_disk,
        allow_expanded_disk=expanded_disk is not None,
        verify_archive=not args.installed,
    )
    print(f"verified Android release: {manifest['directory_name']}")


if __name__ == "__main__":
    main()
